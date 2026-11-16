#!/usr/bin/env python3
"""Compare frozen processor variants on Linux loopback; not a remote-host benchmark."""

import argparse
import csv
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import select
import statistics
import subprocess
import sys

from run_remote_cpp import latency_measurement


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate(result, oracle, affinity, rate):
    manifest = oracle['manifest']
    zero_fields = ('kernel_dropped_packets', 'ring_dropped_packets', 'truncated_packets',
                   'sequence_errors', 'missing_messages', 'protocol_errors',
                   'skipped_market_messages', 'gap_ranges_dropped')
    metric, boundary, latency = latency_measurement(result)
    masks = not affinity['monitor_error'] and all(
        r['observations'] > 0 and r['initial']['allowed_cpus'] == r['last']['allowed_cpus'] == [r['requested_cpu']]
        for r in affinity['roles'].values()) and set(affinity['roles']) == {'rx', 'l2'}
    checks = {
        'processor_valid': result['benchmark_valid'] and result['complete'] and result['book_current'],
        'timing': metric == 'rx_to_l1_decision' and latency['samples'] == manifest['updates'],
        'loss_free': all(result[k] == 0 for k in zero_fields),
        'warmup': result['warmup_messages'] == manifest['initialization_messages'],
        'counts': all(result[k] == oracle[k] for k in ('l1_decisions', 'l1_updates', 'l1_no_updates', 'retained_trades')),
        'l2': result['final_book'] == manifest['final_book'],
        'l1': result['final_l1'] == oracle['final_l1'],
        'affinity': masks,
        'rate': rate is not None and abs(rate / manifest['finite_average_mbps'] - 1) <= .05,
    }
    return checks, boundary, latency


def summarize(records, variants):
    rows = []
    for name, variant in variants.items():
        attempts = [r for r in records if r['variant'] == name]
        valid = [r for r in attempts if r['valid']]
        row = {'variant': name, 'language': variant['language'], 'change': variant['change'],
               'attempts': len(attempts), 'valid_runs': len(valid)}
        for metric in ('p50_ns', 'p99_ns', 'p99_9_ns'):
            row[metric] = statistics.median(r['latency'][metric] for r in valid) if valid else None
        tails = [r['latency']['p99_9_ns'] for r in valid]
        row['min_p99_9_ns'] = min(tails) if tails else None
        row['max_p99_9_ns'] = max(tails) if tails else None
        rows.append(row)
    for row in rows:
        baseline = next(r for r in rows if r['language'] == row['language'] and r['change'] == 'baseline')
        for metric in ('p50_ns', 'p99_ns', 'p99_9_ns'):
            old, new = baseline[metric], row[metric]
            row[metric + '_change_pct'] = 100 * (new / old - 1) if old and new is not None else None
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True,
                        help='Frozen variant binaries/source manifests, oracle, repetitions and explicit local CPU roles')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    variants = config['variants']
    if config['repeats'] < 1 or any(not re.fullmatch(r'[a-zA-Z0-9_-]+', n) for n in variants):
        parser.error('Need positive repetitions and simple variant names')
    cpus = [config[k] for k in ('rx_cpu', 'l2_cpu', 'sender_cpu', 'observer_cpu')]
    if len(set(cpus)) != 4 or not set(cpus) <= os.sched_getaffinity(0):
        parser.error('Supply four distinct allowed local CPU roles')
    oracle = json.loads(Path(config['oracle_file']).read_text())[config['profile']]
    profile = Path(oracle['path']).resolve()
    if sha256(profile) != oracle['sha256']:
        raise ValueError('Workload differs from independent wire oracle')
    args.output.mkdir(parents=True, exist_ok=False)
    context = {'config': config, 'platform': platform.uname()._asdict(),
               'cpuinfo': Path('/proc/cpuinfo').read_text(), 'allowed_cpus': sorted(os.sched_getaffinity(0)),
               'profile_sha256': sha256(profile), 'oracle_sha256': sha256(config['oracle_file']),
               'sender_sha256': sha256(config['sender']), 'harness_sha256': sha256(__file__), 'variants': {},
               'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
    for name, variant in variants.items():
        source_manifest = Path(variant['source_manifest'])
        files = json.loads(source_manifest.read_text())
        source_root = source_manifest.parent / 'src'
        if any(sha256(source_root / p) != digest for p, digest in files.items()):
            raise ValueError('Source snapshot changed: ' + name)
        context['variants'][name] = {**variant, 'binary_sha256': sha256(variant['binary']), 'source_files': files}
    rng = random.Random(config['seed'])
    plan = []
    for repeat in range(1, config['repeats'] + 1):
        names = list(variants)
        rng.shuffle(names)
        plan.extend((repeat, name) for name in names)
    context['plan'] = plan
    (args.output / 'context.json').write_text(json.dumps(context, indent=2) + '\n')
    launcher = Path(__file__).with_name('pin_processor.py').resolve()
    records = []
    timeout = max(30, oracle['manifest']['duration_ns'] / 1e9 * 2 + 10)
    for repeat, name in plan:
        destination = args.output / f'{name}-r{repeat:02d}'
        destination.mkdir()
        command = [sys.executable, str(launcher), '--rx-cpu', str(config['rx_cpu']),
                   '--l2-cpu', str(config['l2_cpu']), '--startup-cpu', str(config['l2_cpu']),
                   '--observer-cpu', str(config['observer_cpu']), '--metadata', str(destination / 'affinity.json'),
                   '--', variants[name]['binary'], '--interface', '127.0.0.1', '--port', '0',
                   '--warmup-messages', str(oracle['manifest']['initialization_messages']),
                   '--unit', str(oracle['manifest']['unit']), '--messages', str(oracle['manifest']['updates']),
                   '--output', str(destination / 'processor.json')]
        row = {'variant': name, 'repeat': repeat, 'valid': False, 'command': command,
               'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'loadavg': Path('/proc/loadavg').read_text().strip()}
        with (destination / 'receiver.txt').open('w') as log, (destination / 'receiver.stderr.txt').open('w') as err:
            receiver = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=err, text=True)
            try:
                if not select.select([receiver.stdout], [], [], 10)[0]:
                    raise RuntimeError('No READY within 10 seconds')
                ready = receiver.stdout.readline()
                log.write(ready)
                match = re.match(r'READY port=(\d+)', ready)
                if not match:
                    raise RuntimeError('Receiver startup failed: ' + ready)
                sender_command = ['taskset', '--cpu-list', str(config['sender_cpu']), config['sender'],
                                  str(profile), '--interface', '127.0.0.1', '--port', match[1], '--spin']
                row['sender_command'] = sender_command
                player = subprocess.run(sender_command, capture_output=True, text=True, timeout=timeout)
                (destination / 'player.txt').write_text(player.stdout)
                (destination / 'player.stderr.txt').write_text(player.stderr)
                stdout, _ = receiver.communicate(timeout=10)
                log.write(stdout)
                result = json.loads((destination / 'processor.json').read_text())
                affinity = json.loads((destination / 'affinity.json').read_text())
                match = re.search(r'actual_mbps=([\d.eE+-]+)', player.stdout)
                rate = float(match[1]) if match else None
                checks, boundary, latency = validate(result, oracle, affinity, rate)
                row.update(checks=checks, timing_boundary=boundary, latency=latency,
                           sender_mbps=rate, queue_wait=result['queue_wait'], ring_high_watermark=result['ring_high_watermark'],
                           receiver_exit=receiver.returncode, sender_exit=player.returncode,
                           valid=all(checks.values()) and receiver.returncode == player.returncode == 0)
            except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
                row['error'] = str(error)
            finally:
                if receiver.poll() is None:
                    receiver.terminate()
                    try:
                        receiver.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        receiver.kill()
                        receiver.wait()
        (destination / 'run.json').write_text(json.dumps(row, indent=2) + '\n')
        records.append(row)
        (args.output / 'runs.json').write_text(json.dumps(records, indent=2) + '\n')
        rows = summarize(records, variants)
        (args.output / 'summary.json').write_text(json.dumps(rows, indent=2) + '\n')
        with (args.output / 'summary.csv').open('w', newline='') as out:
            writer = csv.DictWriter(out, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        print(json.dumps({'trial': len(records), 'total': len(plan), 'variant': name, 'repeat': repeat,
                          'valid': row['valid'], 'latency': row.get('latency'), 'error': row.get('error')}), flush=True)
    return 0 if all(r['valid'] for r in records) else 1


if __name__ == '__main__':
    raise SystemExit(main())
