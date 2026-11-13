#!/usr/bin/env python3
"""Capture receiver-side packet timestamps with an already-built remote probe."""
import argparse
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import select
import subprocess

from analyze_wire_capture import analyze
from run_remote_cpp import INVENTORY, remote, ssh


STATE = r'''
import json,sys
from pathlib import Path
sys.path.insert(0,str(Path(sys.argv[1])/'benchmark'))
from hwtstamp_session import configuration,phc_index
bond=sys.argv[2]
active=(Path('/sys/class/net')/bond/'bonding/active_slave').read_text().strip()
members=(Path('/sys/class/net')/bond/'bonding/slaves').read_text().split()
print(json.dumps({'active_slave':active,'members':members,
 'active_phc':'/dev/ptp'+str(phc_index(active)),
 'configuration':{n:configuration(n) for n in [bond,*members]}}))
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('receiver-host', 'sender-host', 'receiver-root', 'sender-root',
                 'receiver-interface', 'sender-interface', 'receiver-build', 'sender-build', 'bond'):
        parser.add_argument('--' + name, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--phc')
    mode.add_argument('--software-only', action='store_true')
    parser.add_argument('--rx-cpu', type=int, default=2)
    parser.add_argument('--calibration-cpu', type=int, default=15)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=1)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.profile.is_absolute() or '..' in args.profile.parts or args.repeats < 1:
        parser.error('Need a relative profile and positive repeat count')
    if not all(re.fullmatch(r'[A-Za-z0-9_-]+', n) for n in
               (args.output.name, args.receiver_build, args.sender_build, args.bond)):
        parser.error('Use simple build, bond and output directory names')
    args.output.mkdir(parents=True, exist_ok=False)
    receiver_root, sender_root = PurePosixPath(args.receiver_root), PurePosixPath(args.sender_root)
    workload = json.loads(args.profile.with_suffix('.json').read_text())
    digest = hashlib.sha256(args.profile.read_bytes()).hexdigest()
    if remote(args.sender_host, ['sha256sum', sender_root / args.profile.as_posix()]).split()[0] != digest:
        raise RuntimeError('Sender workload checksum differs')
    receiver_info = json.loads(remote(args.receiver_host, ['python3', '-c', INVENTORY, receiver_root, args.receiver_interface]))
    sender_info = json.loads(remote(args.sender_host, ['python3', '-c', INVENTORY, sender_root, args.sender_interface]))
    probe = receiver_root / 'build' / args.receiver_build / 'wire_rx_probe'
    player = sender_root / 'build' / args.sender_build / 'play_feed'
    context = {'receiver': receiver_info, 'sender': sender_info, 'workload': workload,
               'workload_path': str(args.profile), 'workload_sha256': digest,
               'probe_sha256': remote(args.receiver_host, ['sha256sum', probe]).split()[0],
               'player_sha256': remote(args.sender_host, ['sha256sum', player]).split()[0],
               'arguments': {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
               'results': []}
    timeout = math.ceil(workload['duration_ns'] / 1e9 * 2 + 20)
    remote_dir = receiver_root / 'analysis' / args.output.name
    remote(args.receiver_host, ['python3', '-c', 'from pathlib import Path; import sys; Path(sys.argv[1]).mkdir(parents=True,exist_ok=False)', remote_dir])
    for repeat in range(1, args.repeats + 1):
        name = f'r{repeat:02d}'
        prefix = args.output / name
        remote_prefix = remote_dir / name
        state = json.loads(remote(args.receiver_host, ['python3', '-c', STATE, receiver_root, args.bond]))
        if not args.software_only and (state['active_phc'] != args.phc or
                state['configuration'][state['active_slave']]['rx_filter'] != 1):
            raise RuntimeError('Active PHC/configuration does not match requested hardware capture')
        record = {'repeat': repeat, 'before': state, 'valid': False}
        command = ['timeout', '--signal=TERM', str(timeout) + 's', probe,
                   '--interface', args.receiver_interface, '--port', '0', '--bond', args.bond,
                   '--packets', str(workload['packets']), '--rx-cpu', str(args.rx_cpu),
                   '--calibration-cpu', str(args.calibration_cpu), '--output-prefix', remote_prefix]
        command += ['--software-only'] if args.software_only else ['--phc', args.phc]
        record['receiver_command'] = list(map(str, command))
        receiver = subprocess.Popen(ssh(args.receiver_host, command), stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
        ready, stdout, stderr = '', '', ''
        try:
            if not select.select([receiver.stdout], [], [], 10)[0]:
                raise RuntimeError('Probe did not become ready')
            ready = receiver.stdout.readline()
            match = re.match(r'READY port=(\d+)', ready)
            if not match:
                raise RuntimeError('Probe startup failed: ' + ready)
            send_command = ['timeout', '--signal=TERM', str(timeout) + 's', player,
                            sender_root / args.profile.as_posix(), '--interface', args.sender_interface,
                            '--port', match.group(1), '--spin']
            record['sender_command'] = list(map(str, send_command))
            sent = subprocess.run(ssh(args.sender_host, send_command), capture_output=True, text=True, timeout=timeout + 10)
            Path(str(prefix) + '-player.txt').write_text(sent.stdout)
            Path(str(prefix) + '-player.stderr.txt').write_text(sent.stderr)
            stdout, stderr = receiver.communicate(timeout=timeout + 10)
            record.update(receiver_exit=receiver.returncode, sender_exit=sent.returncode)
            for suffix in ('-packets.csv', '-clocks.csv', '-capture.json'):
                subprocess.run(['scp', '-q', f'{args.receiver_host}:{remote_prefix}{suffix}', str(prefix) + suffix], check=True)
            analysis = analyze(prefix)
            record['analysis'] = analysis
            record['valid'] = bool(receiver.returncode == 0 and sent.returncode == 0 and
                analysis['measurement_valid'] and analysis['capture']['pitch_messages'] == workload['pitch_messages'])
        except Exception as error:
            record['error'] = str(error)
        finally:
            if receiver.poll() is None:
                receiver.kill()
                stdout, stderr = receiver.communicate()
            Path(str(prefix) + '-receiver.txt').write_text(ready + stdout)
            Path(str(prefix) + '-receiver.stderr.txt').write_text(stderr)
            record['after'] = json.loads(remote(args.receiver_host, ['python3', '-c', STATE, receiver_root, args.bond]))
            if record['before'] != record['after']:
                record['valid'] = False
                record['state_changed'] = True
            context['results'].append(record)
            (args.output / 'context.json').write_text(json.dumps(context, indent=2) + '\n')
        print(json.dumps({'repeat': repeat, 'valid': record['valid'],
                          'analysis': record.get('analysis'), 'error': record.get('error')}), flush=True)
    if any(not r['valid'] for r in context['results']):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
