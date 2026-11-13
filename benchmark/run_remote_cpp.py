#!/usr/bin/env python3
"""Run already-built OS-socket processors with a separate multicast sender."""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import random
import re
import select
import shlex
import statistics
import subprocess


def ssh(host, arguments):
    return ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', host, shlex.join(map(str, arguments))]


def remote(host, arguments, timeout=20):
    result = subprocess.run(ssh(host, arguments), capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{host}: {result.stderr.strip()} {result.stdout.strip()}')
    return result.stdout


INVENTORY = r'''
import hashlib,json,os,platform,shutil,subprocess,sys
from pathlib import Path
root=Path(sys.argv[1]); interface=sys.argv[2]
addresses=json.loads(subprocess.run(['ip','-j','-4','address','show'],capture_output=True,text=True,check=True).stdout)
nic=next(n for n in addresses if any(a.get('local')==interface for a in n.get('addr_info',[])))
device=Path('/sys/class/net')/nic['ifname']
link={}
for key in ['speed','mtu','operstate']:
    try: link[key]=(device/key).read_text().strip()
    except OSError: pass
if (device/'device/driver').exists(): link['driver']=(device/'device/driver').resolve().name
cpu=next((s.split(':',1)[1].strip() for s in Path('/proc/cpuinfo').read_text().splitlines() if s.startswith('model name')),'')
info={'host':platform.uname()._asdict(),'cpu_model':cpu,'allowed_cpus':sorted(os.sched_getaffinity(0)),
      'loadavg':Path('/proc/loadavg').read_text().strip(),'interface':nic,'link':link,
      'timeout_program':shutil.which('timeout'),'thread_affinity':'OS default; no explicit pinning'}
manifest=root/'.deploy/source-manifest.json'
if manifest.exists(): info['deployment']=json.loads(manifest.read_text())
if manifest.exists():
    sources={name:hashlib.sha256((root/name).read_bytes()).hexdigest()
             for name in info['deployment']['files'] if name=='CMakeLists.txt' or name.endswith(('.cpp','.hpp'))}
    info['compiled_source_sha256']=hashlib.sha256(json.dumps(sources,sort_keys=True).encode()).hexdigest()
    info['source_files_match_deployment']=all(value==info['deployment']['files'][name] for name,value in sources.items())
    rust_sources={name:hashlib.sha256((root/name).read_bytes()).hexdigest()
                  for name in info['deployment']['files'] if name.startswith('processors/rust/')
                  and (name.endswith('.rs') or name.endswith(('Cargo.toml','Cargo.lock')))}
    info['rust_source_sha256']=hashlib.sha256(json.dumps(rust_sources,sort_keys=True).encode()).hexdigest()
    info['rust_source_files_match_deployment']=bool(rust_sources) and all(
        value==info['deployment']['files'][name] for name,value in rust_sources.items())
info['cpu_policy']={}
for key in ['scaling_driver','scaling_governor','energy_performance_preference']:
    path=Path('/sys/devices/system/cpu/cpu0/cpufreq')/key
    if path.is_file(): info['cpu_policy'][key]=path.read_text().strip()
print(json.dumps(info))
'''


def summarize(records, names):
    summary = []
    for name in names:
        attempts = [record for record in records if record['build'] == name]
        runs = [record for record in attempts if record['valid']]
        rates = [record['sender_mbps'] for record in runs if record['sender_mbps'] is not None]
        summary.append({
            'build': name, 'attempts': len(attempts), 'valid_runs': len(runs),
            'invalid_runs': len(attempts) - len(runs),
            **{f'median_{key}': statistics.median(r['rx_to_l2'][key] for r in runs) if runs else None
               for key in ('p50_ns', 'p99_ns', 'p99_9_ns')},
            'min_p99_9_ns': min(r['rx_to_l2']['p99_9_ns'] for r in runs) if runs else None,
            'max_p99_9_ns': max(r['rx_to_l2']['p99_9_ns'] for r in runs) if runs else None,
            'median_sender_mbps': statistics.median(rates) if rates else None,
            'all_rates_within_5pct': bool(runs) and all(r['rate_within_5pct'] for r in runs),
        })
    return summary


def affinity_settings(baseline, rx_cpu=None, l2_cpu=None, observer_cpu=None, unpinned=False):
    if unpinned and (rx_cpu is not None or l2_cpu is not None):
        raise ValueError('--unpinned cannot be combined with RX/L2 CPU overrides')
    return {
        'rx_cpu': None if unpinned else (baseline['rx_cpu'] if rx_cpu is None else rx_cpu),
        'l2_cpu': None if unpinned else (baseline['l2_cpu'] if l2_cpu is None else l2_cpu),
        'observer_cpu': baseline['observer_cpu'] if observer_cpu is None else observer_cpu,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receiver-host', required=True)
    parser.add_argument('--sender-host', required=True)
    parser.add_argument('--receiver-root', required=True)
    parser.add_argument('--sender-root', required=True)
    parser.add_argument('--receiver-interface', required=True)
    parser.add_argument('--sender-interface', required=True)
    parser.add_argument('--builds', nargs='+', required=True, help='Receiver build names under build/')
    parser.add_argument('--sender-build', default='sender')
    parser.add_argument('--profile', type=Path, required=True, help='Local PCAP, relative to the project root')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--continue-on-invalid', action='store_true',
                        help='Keep failed attempts and run remaining trials; return failure after the batch')
    parser.add_argument('--observe-affinity', action='store_true', help='Record per-thread placement before READY and during the run')
    parser.add_argument('--affinity-config', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'configs/baseline.json',
                        help='Baseline RX/L2 placement configuration')
    parser.add_argument('--unpinned', action='store_true', help='Explicitly leave RX/L2 placement to the OS')
    parser.add_argument('--rx-cpu', type=int, help='Pin the existing RX worker to this logical CPU')
    parser.add_argument('--l2-cpu', type=int, help='Pin the main L2 thread to this logical CPU')
    parser.add_argument('--observer-cpu', type=int, help='Pin the external placement observer; processor inherits its original mask')
    parser.add_argument('--startup-cpu', type=int, help='CPU for startup allocations before RX/L2 thread pinning')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    baseline = json.loads(args.affinity_config.read_text())
    try:
        settings = affinity_settings(baseline, args.rx_cpu, args.l2_cpu, args.observer_cpu, args.unpinned)
    except ValueError as error:
        parser.error(str(error))
    args.rx_cpu, args.l2_cpu, args.observer_cpu = (settings[k] for k in ('rx_cpu', 'l2_cpu', 'observer_cpu'))
    observe_affinity = args.observe_affinity or any(cpu is not None for cpu in (args.rx_cpu, args.l2_cpu, args.observer_cpu, args.startup_cpu))
    if any(cpu is not None and cpu < 0 for cpu in (args.rx_cpu, args.l2_cpu, args.observer_cpu, args.startup_cpu)):
        parser.error('CPU numbers must be nonnegative')
    if args.repeats < 1 or any(not re.fullmatch(r'[A-Za-z0-9_-]+', name) for name in args.builds):
        parser.error('Need positive repeats and simple build names')
    if args.profile.is_absolute() or '..' in args.profile.parts:
        parser.error('--profile must stay relative to the project root')
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.output.name):
        parser.error('Use a simple output-directory basename for the remote result directory')
    args.output.mkdir(parents=True, exist_ok=False)
    manifest = json.loads(args.profile.with_suffix('.json').read_text())
    run_timeout = max(15, math.ceil(manifest['duration_ns'] / 1e9 * 1.5 + 10))
    digest = hashlib.sha256(args.profile.read_bytes()).hexdigest()
    receiver_root = PurePosixPath(args.receiver_root)
    sender_root = PurePosixPath(args.sender_root)
    remote_results = receiver_root / 'analysis' / args.output.name
    receiver_info = json.loads(remote(args.receiver_host, ['python3', '-c', INVENTORY,
                                                         receiver_root, args.receiver_interface]))
    sender_info = json.loads(remote(args.sender_host, ['python3', '-c', INVENTORY,
                                                     sender_root, args.sender_interface]))
    if not receiver_info['timeout_program'] or not sender_info['timeout_program']:
        raise RuntimeError('Need an installed timeout utility on both hosts')
    remote_digest = remote(args.sender_host, ['sha256sum', sender_root / str(args.profile)]).split()[0]
    if remote_digest != digest:
        raise RuntimeError('Sender workload does not match the local PCAP')
    if receiver_info.get('deployment', {}).get('source_id') != sender_info.get('deployment', {}).get('source_id'):
        raise RuntimeError('Receiver and sender source deployments differ')
    if not all(info.get('source_files_match_deployment') for info in (receiver_info, sender_info)):
        raise RuntimeError('Compiled source files changed after deployment')
    if receiver_info['compiled_source_sha256'] != sender_info['compiled_source_sha256']:
        raise RuntimeError('Receiver and sender compiled source hashes differ')
    create = 'from pathlib import Path; import sys; Path(sys.argv[1]).mkdir(parents=True,exist_ok=False)'
    remote(args.receiver_host, ['python3', '-c', create, remote_results])
    builds = {}
    for name in args.builds:
        directory = args.output / 'builds' / name
        directory.mkdir(parents=True)
        build_root = receiver_root / 'build' / name
        detect = ('from pathlib import Path; import sys; p=Path(sys.argv[1]); '
                  'print(p.read_text() if p.is_file() else "{}")')
        info = json.loads(remote(args.receiver_host, ['python3', '-c', detect, build_root / 'build-info.json']))
        if info:
            builds[name] = info
            (directory / 'build-info.json').write_text(json.dumps(info, indent=2) + '\n')
        else:
            for filename in ('CMakeCache.txt', 'compile_commands.json'):
                content = remote(args.receiver_host, ['cat', build_root / filename])
                (directory / filename).write_text(content)
            cache = (directory / 'CMakeCache.txt').read_text()
            builds[name] = {key: value for key, value in re.findall(
                r'^(CMAKE_CXX_COMPILER|CMAKE_CXX_FLAGS|CMAKE_CXX_FLAGS_RELEASE):[^=]+=([^\n]*)$', cache, re.MULTILINE)}
            builds[name]['binary_name'] = 'processor_cpp'
        binary_name = builds[name]['binary_name']
        if not re.fullmatch(r'[A-Za-z0-9_-]+', binary_name):
            raise RuntimeError('Unsafe binary name in build metadata')
        binary = build_root / binary_name
        builds[name]['binary_sha256'] = remote(args.receiver_host, ['sha256sum', binary]).split()[0]
        (directory / 'ldd.txt').write_text(remote(args.receiver_host, ['ldd', binary]))
        if builds[name].get('implementation') == 'rust-os':
            if not all(info.get('rust_source_files_match_deployment') for info in (receiver_info, sender_info)):
                raise RuntimeError('Rust sources do not match the deployment')
            if receiver_info['rust_source_sha256'] != sender_info['rust_source_sha256']:
                raise RuntimeError('Receiver and sender Rust source snapshots differ')
            if builds[name]['rust_source_sha256'] != receiver_info['rust_source_sha256']:
                raise RuntimeError('Rust build metadata refers to a different source snapshot')
    jobs = []
    for repeat in range(1, args.repeats + 1):
        order = list(args.builds)
        random.Random(42 + repeat).shuffle(order)
        jobs.extend((name, repeat) for name in order)
    context = {'receiver': receiver_info, 'sender': sender_info, 'builds': builds,
               'workload': manifest, 'workload_path': str(args.profile), 'workload_sha256': digest,
               'repeats': args.repeats, 'order_seed': 42, 'execution_order': jobs,
               'continue_on_invalid': args.continue_on_invalid,
               'baseline_affinity': baseline,
               'receiver_startup_cpu': args.startup_cpu,
               'affinity_requested': {'observe': observe_affinity, 'rx_cpu': args.rx_cpu,
                                      'l2_cpu': args.l2_cpu, 'observer_cpu': args.observer_cpu},
               'run_timeout_seconds': run_timeout,
               'remote_results': str(remote_results), 'results': []}
    context_path = args.output / 'context.json'
    context_path.write_text(json.dumps(context, indent=2) + '\n')
    records = []
    for name, repeat in jobs:
        run_name = f'{name}-r{repeat:02d}'
        destination = args.output / run_name
        destination.mkdir()
        remote_run = remote_results / run_name
        remote(args.receiver_host, ['python3', '-c', create, remote_run])
        remote_report = remote_run / 'processor.json'
        command = [receiver_info['timeout_program'], '--signal=TERM', f'{run_timeout}s',
                   receiver_root / 'build' / name / builds[name]['binary_name'],
                   '--interface', args.receiver_interface, '--port', '0',
                   '--unit', str(manifest['unit']), '--warmup-messages', str(manifest['initialization_messages']),
                   '--messages', str(manifest['updates']), '--output', remote_report]
        if observe_affinity:
            launcher = ['python3', receiver_root / 'benchmark' / 'pin_processor.py',
                        '--metadata', remote_run / 'affinity.json']
            for flag, cpu in [('--rx-cpu', args.rx_cpu), ('--l2-cpu', args.l2_cpu), ('--observer-cpu', args.observer_cpu), ('--startup-cpu', args.startup_cpu)]:
                if cpu is not None:
                    launcher.extend([flag, str(cpu)])
            command = command[:3] + launcher + ['--'] + command[3:]
        receiver = subprocess.Popen(ssh(args.receiver_host, command), stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
        ready = ''
        try:
            if not select.select([receiver.stdout], [], [], 6)[0]:
                raise RuntimeError('Receiver did not become ready')
            ready = receiver.stdout.readline()
            match = re.match(r'READY port=(\d+)', ready)
            if not match:
                raise RuntimeError('Receiver startup failed: ' + ready)
            sender_command = [sender_info['timeout_program'], '--signal=TERM', f'{run_timeout}s',
                              sender_root / 'build' / args.sender_build / 'play_feed',
                              sender_root / str(args.profile), '--interface', args.sender_interface,
                              '--port', match.group(1), '--spin']
            player = subprocess.run(ssh(args.sender_host, sender_command), capture_output=True,
                                    text=True, timeout=run_timeout + 10)
            stdout, stderr = receiver.communicate(timeout=run_timeout + 10)
            (destination / 'receiver.txt').write_text(ready + stdout)
            (destination / 'receiver.stderr.txt').write_text(stderr)
            (destination / 'player.txt').write_text(player.stdout)
            (destination / 'player.stderr.txt').write_text(player.stderr)
            result_text = remote(args.receiver_host, ['cat', remote_report])
            (destination / 'processor.json').write_text(result_text)
            result = json.loads(result_text)
            affinity = None
            if observe_affinity:
                content = remote(args.receiver_host, ['cat', remote_run / 'affinity.json'])
                (destination / 'affinity.json').write_text(content)
                affinity = json.loads(content)
            affinity_verified = affinity is None or (not affinity['monitor_error'] and all(
                role['observations'] > 0 and
                (role['requested_cpu'] is None or
                 role['initial']['allowed_cpus'] == role['last']['allowed_cpus'] == [role['requested_cpu']])
                for role in affinity['roles'].values()))
            l2_matches = result['final_book'] == manifest['final_book']
            valid = (receiver.returncode == 0 and player.returncode == 0 and result['benchmark_valid']
                     and result['complete'] and result['book_current'] and l2_matches
                     and result['rx_to_l2']['samples'] == manifest['updates'] and affinity_verified)
            rate_match = re.search(r'actual_mbps=([\d.eE+-]+)', player.stdout)
            sender_mbps = float(rate_match.group(1)) if rate_match else None
            record = {'build': name, 'repeat': repeat, 'valid': valid, 'l2_matches': l2_matches,
                      'error': result['error'], 'complete': result['complete'],
                      'affinity': affinity,
                      'affinity_verified': affinity_verified,
                      'receiver_exit': receiver.returncode, 'sender_exit': player.returncode,
                      'rx_to_l2': result['rx_to_l2'], 'queue_wait': result['queue_wait'],
                      'sender_mbps': sender_mbps,
                      'rate_within_5pct': sender_mbps is not None and abs(sender_mbps / manifest['finite_average_mbps'] - 1) < 0.05,
                      'kernel_drops': result['kernel_dropped_packets'], 'ring_drops': result['ring_dropped_packets'],
                      'missing_messages': result['missing_messages'], 'receiver_command': list(map(str, command)),
                      'sequence_errors': result['sequence_errors'],
                      'pending_gap_ranges': result['pending_gap_ranges'],
                      'sender_command': list(map(str, sender_command))}
            for key in ('max_lateness_ns', 'schedule_slip_ns'):
                match = re.search(rf'{key}=(\d+)', player.stdout)
                record[key] = int(match.group(1)) if match else None
            (destination / 'run.json').write_text(json.dumps(record, indent=2) + '\n')
            records.append(record)
            context['results'] = records
            context_path.write_text(json.dumps(context, indent=2) + '\n')
            print(json.dumps({key: record[key] for key in ('build', 'repeat', 'valid', 'rx_to_l2', 'sender_mbps')}), flush=True)
            if not valid and not args.continue_on_invalid:
                raise RuntimeError(f'{run_name} failed validation; preserved results under {destination}')
        finally:
            if receiver.poll() is None:
                receiver.kill()
                stdout, stderr = receiver.communicate()
                (destination / 'receiver.txt').write_text(ready + stdout)
                (destination / 'receiver.stderr.txt').write_text(stderr)
    summary = summarize(records, args.builds)
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    with (args.output / 'summary.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    print(json.dumps({'output': str(args.output), 'summary': summary}, indent=2))
    if any(not record['valid'] for record in records):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
