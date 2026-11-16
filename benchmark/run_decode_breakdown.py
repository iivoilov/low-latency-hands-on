#!/usr/bin/env python3
"""Compare PITCH decoders by layout, real input values, and message ordering."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import sys


def summarize(result):
    expected = {'original', 'packed'}
    if any(r['decoder'] == 'zero-copy' for r in result['runs']):
        expected.add('zero-copy')
    by_repeat = {}
    for row in result['runs']:
        pair = by_repeat.setdefault(row['repeat'], {})
        if row['decoder'] in pair:
            raise ValueError('Duplicate decoder/repetition')
        pair[row['decoder']] = row
    if not by_repeat:
        raise ValueError('No measured pairs')
    changes = []
    for pair in by_repeat.values():
        if set(pair) != expected:
            raise ValueError('Incomplete pair')
        old, new = pair['original'], pair['packed']
        if any(row['checksum'] != old['checksum'] for row in pair.values()):
            raise ValueError('Output checksum mismatch')
        changes.append(100 * (new['ns_per_message'] / old['ns_per_message'] - 1))
    old = statistics.median(p['original']['ns_per_message'] for p in by_repeat.values())
    new = statistics.median(p['packed']['ns_per_message'] for p in by_repeat.values())
    summary = {'original_ns': old, 'packed_ns': new, 'median_time_change_pct': 100 * (new / old - 1),
            'paired_change_median_pct': statistics.median(changes),
            'paired_change_min_pct': min(changes), 'paired_change_max_pct': max(changes),
            'packed_faster_pairs': sum(c < 0 for c in changes), 'pairs': len(changes)}
    if 'zero-copy' in expected:
        zero = statistics.median(p['zero-copy']['ns_per_message'] for p in by_repeat.values())
        summary.update({'zero_copy_ns': zero,
                        'zero_copy_vs_original_pct': 100 * (zero / old - 1),
                        'zero_copy_vs_packed_pct': 100 * (zero / new - 1)})
        for baseline in ('original', 'packed'):
            paired = [100 * (p['zero-copy']['ns_per_message'] / p[baseline]['ns_per_message'] - 1)
                      for p in by_repeat.values()]
            summary.update({f'zero_copy_faster_{baseline}_pairs': sum(v < 0 for v in paired),
                            f'zero_copy_vs_{baseline}_paired_min_pct': min(paired),
                            f'zero_copy_vs_{baseline}_paired_max_pct': max(paired)})
    return summary


def cases(fixtures, pcap, pcap_types):
    result = []
    for line in fixtures.read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        name, hex_bytes, *_ = line.split()
        wire = bytes.fromhex(hex_bytes)
        result.append({'name': f'fixture-{name}', 'family': 'fixture-layout', 'type': wire[1],
                       'length': len(wire), 'input': fixtures,
                       'args': ['--type', hex(wire[1]), '--length', str(len(wire))]})
    for kind in pcap_types:
        result.append({'name': f'pcap-type-{kind:02x}', 'family': 'pcap-type', 'type': kind,
                       'input': pcap, 'args': ['--type', hex(kind)]})
    for order in ('input', 'grouped', 'shuffled'):
        result.append({'name': f'fixtures-1024-{order}', 'family': 'fixture-order',
                       'input': fixtures, 'args': ['--copies', '1024', '--order', order]})
        result.append({'name': f'pcap-{order}', 'family': 'pcap-order',
                       'input': pcap, 'args': ['--order', order]})
        # Same five order layouts as the synthetic PCAP, with equal weights and
        # cache-resident bytes. Controls are deliberately excluded in this case.
        result.append({'name': f'order-fixtures-1024-{order}', 'family': 'order-fixture-order',
                       'input': fixtures,
                       'args': ['--copies', '1024', '--order', order,
                                '--type', '0x21', '--type', '0x23', '--type', '0x25',
                                '--type', '0x27', '--type', '0x29']})
    result.append({'name': 'fixtures-23-input', 'family': 'fixture-order',
                   'input': fixtures, 'args': []})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/pitch_decode_bench'))
    parser.add_argument('--fixtures', type=Path, default=Path('processors/tests/equities_messages.tsv'))
    parser.add_argument('--pcap', type=Path, required=True)
    parser.add_argument('--pcap-types', default='0x20,0x21,0x23,0x25,0x27,0x29,0x97',
                        help='Types present in the selected PCAP (default: current generator subset)')
    parser.add_argument('--output', type=Path, required=True, help='Fresh directory for every attempt')
    parser.add_argument('--cpu', type=int, default=3)
    parser.add_argument('--observer-cpu', type=int, default=15)
    parser.add_argument('--messages', type=int, default=5_000_000)
    parser.add_argument('--warmup', type=int, default=100_000)
    parser.add_argument('--repeats', type=int, default=9)
    parser.add_argument('--seed', type=int, default=20261114)
    parser.add_argument('--include-zero-copy', action='store_true', help='Compare all three implementations')
    args = parser.parse_args()
    binary, fixtures, pcap = (p.resolve() for p in (args.binary, args.fixtures, args.pcap))
    for p in (binary, fixtures, pcap):
        if not p.is_file():
            parser.error(f'Missing input {p}')
    if not {args.cpu, args.observer_cpu} <= os.sched_getaffinity(0):
        parser.error('Requested CPUs are unavailable')
    if args.cpu == args.observer_cpu:
        parser.error('Use a separate observer CPU')
    taskset = shutil.which('taskset')
    if not taskset:
        parser.error('taskset is required')
    pcap_types = [int(t, 0) for t in args.pcap_types.split(',')]
    plan = cases(fixtures, pcap, pcap_types)
    random.Random(args.seed).shuffle(plan)
    args.output.mkdir(parents=True, exist_ok=False)
    os.sched_setaffinity(0, {args.observer_cpu})
    root = Path(__file__).resolve().parents[1]
    sources = [Path(__file__).resolve(), root / 'benchmark/pitch_decode.cpp', root / 'CMakeLists.txt',
               *sorted((root / 'processors/cpp/src').glob('pitch_message*.cpp')),
               root / 'processors/cpp/include/low_latency/pitch_message.hpp']
    sources.append(root / 'processors/cpp/src/pitch_wire_layouts.hpp')
    metadata = {'platform': platform.platform(), 'cpu': args.cpu, 'observer_cpu': args.observer_cpu,
                'cpu_model': next(l.split(':', 1)[1].strip() for l in Path('/proc/cpuinfo').read_text().splitlines()
                                  if l.startswith('model name')),
                'seed': args.seed, 'load_before': Path('/proc/loadavg').read_text().strip(),
                'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                'source_sha256': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
                'input_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (fixtures, pcap)},
                'attempts': []}
    compile_commands = binary.parent / 'compile_commands.json'
    if compile_commands.exists():
        metadata['compile_commands'] = [e for e in json.loads(compile_commands.read_text())
                                        if Path(e['file']) in sources]
    for p in sources:
        target = args.output / 'source' / p.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, target)
    metadata_path = args.output / 'metadata.json'
    metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
    summaries = []
    failed = False
    for number, case in enumerate(plan, 1):
        flag = '--fixtures' if case['input'] == fixtures else '--pcap'
        command = [taskset, '-c', str(args.cpu), str(binary), flag, str(case['input']),
                   '--messages', str(args.messages), '--warmup', str(args.warmup),
                   '--repeats', str(args.repeats), '--seed', str(args.seed), *case['args']]
        if args.include_zero_copy:
            command += ['--decoders', 'original,packed,zero-copy']
        record = {k: v for k, v in case.items() if k != 'input'}
        record.update({'command': command, 'input': str(case['input'])})
        metadata['attempts'].append(record)
        metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
        stdout_path = args.output / (case['name'] + '.json')
        try:
            with stdout_path.open('x') as stdout, (args.output / (case['name'] + '.stderr')).open('x') as stderr:
                try:
                    record['exit_code'] = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=60).returncode
                except subprocess.TimeoutExpired:
                    record['exit_code'] = 'timeout'
            if record['exit_code'] != 0:
                raise RuntimeError('Benchmark invocation failed')
            result = json.loads(stdout_path.read_text())
            if result['affinity_cpus'] != [args.cpu] or any(
                    r['start_cpu'] != args.cpu or r['end_cpu'] != args.cpu for r in result['runs']):
                raise RuntimeError('CPU placement was not verified')
            if len(result['runs']) != args.repeats * (3 if args.include_zero_copy else 2):
                raise RuntimeError('Incomplete run set')
            summary = {'name': case['name'], 'family': case['family'],
                       **{k: case[k] for k in ('type', 'length') if k in case},
                       'corpus_messages': result['corpus_messages'], 'corpus_bytes': result['corpus_bytes'],
                       'counts': result['message_type_counts'], **summarize(result)}
            record['valid'] = True
            summaries.append(summary)
            zero_note = (f"; zero-copy {summary['zero_copy_ns']:.3f} ns "
                         f"({summary['zero_copy_vs_original_pct']:+.1f}% vs original)"
                         if args.include_zero_copy else '')
            print(f"{number}/{len(plan)} {case['name']}: "
                  f"{summary['original_ns']:.3f} -> {summary['packed_ns']:.3f} ns "
                  f"({summary['median_time_change_pct']:+.1f}%), "
                  f"packed faster {summary['packed_faster_pairs']}/{summary['pairs']}{zero_note}", flush=True)
        except (OSError, ValueError, RuntimeError) as error:
            record.update({'valid': False, 'error': str(error)})
            failed = True
            print(f"{number}/{len(plan)} {case['name']}: FAILED {error}", flush=True)
        metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
        (args.output / 'summary.json').write_text(json.dumps(summaries, indent=2) + '\n')
    metadata['load_after'] = Path('/proc/loadavg').read_text().strip()
    metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
