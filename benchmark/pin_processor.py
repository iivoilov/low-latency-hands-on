#!/usr/bin/env python3
"""Observe and optionally pin the existing two-thread Linux processor before READY."""

import argparse
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import sys
import threading


def snapshot(tid):
    directory = Path('/proc') / str(tid)
    # Field 39 is the last CPU. The comm field can contain spaces and parentheses.
    fields = (directory / 'stat').read_text().rsplit(')', 1)[1].split()
    counters = {key: int(value) for key, value in re.findall(
        r'^\s*(se\.nr_migrations|nr_switches|nr_voluntary_switches|nr_involuntary_switches)\s*:\s*(\d+)',
        (directory / 'sched').read_text(), re.MULTILINE)}
    return {'allowed_cpus': sorted(os.sched_getaffinity(tid)),
            'last_cpu': int(fields[36]), **counters}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rx-cpu', type=int)
    parser.add_argument('--l2-cpu', type=int)
    parser.add_argument('--observer-cpu', type=int)
    parser.add_argument('--startup-cpu', type=int, help='Constrain startup/first-touch allocation before per-thread placement')
    parser.add_argument('--metadata', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('A processor command is required after --')
    allowed = sorted(os.sched_getaffinity(0))
    for cpu in (args.rx_cpu, args.l2_cpu, args.observer_cpu, args.startup_cpu):
        if cpu is not None and cpu not in allowed:
            parser.error(f'CPU {cpu} is outside the inherited allowed set {allowed}')
    if args.metadata.exists():
        parser.error('Affinity metadata already exists')

    if args.startup_cpu is not None:
        taskset = shutil.which('taskset')
        if not taskset:
            parser.error('--startup-cpu requires taskset')
        command = [taskset, '--cpu-list', str(args.startup_cpu), *command]
    child = subprocess.Popen(command, stdout=subprocess.PIPE, text=True)
    finished = threading.Event()
    monitor = None
    metadata = {'inherited_allowed_cpus': allowed, 'observer_cpu': args.observer_cpu,
                'startup_cpu': args.startup_cpu,
                'sample_interval_seconds': 0.25, 'roles': {}, 'monitor_error': ''}
    old_handlers = {}
    try:
        for signum in (signal.SIGTERM, signal.SIGINT):
            old_handlers[signum] = signal.signal(signum, lambda sig, frame: child.send_signal(sig)
                                                 if child.poll() is None else None)
        # Pin the observer only after fork/exec so the child inherits the broad mask.
        if args.observer_cpu is not None:
            os.sched_setaffinity(0, {args.observer_cpu})
        ready = child.stdout.readline()
        if not re.match(r'READY port=\d+', ready):
            raise RuntimeError('Processor did not emit READY: ' + ready.strip())
        tids = sorted(int(path.name) for path in (Path('/proc') / str(child.pid) / 'task').iterdir())
        workers = [tid for tid in tids if tid != child.pid]
        if len(tids) != 2 or len(workers) != 1:
            raise RuntimeError('Expected main L2 thread and exactly one RX worker')
        for role, tid, cpu in [('l2', child.pid, args.l2_cpu), ('rx', workers[0], args.rx_cpu)]:
            if cpu is not None:
                os.sched_setaffinity(tid, {cpu})
            initial = snapshot(tid)
            if cpu is not None and initial['allowed_cpus'] != [cpu]:
                raise RuntimeError(f'{role} affinity verification failed')
            metadata['roles'][role] = {'tid': tid, 'requested_cpu': cpu, 'initial': initial,
                                       'last': initial, 'sampled_cpus': [], 'observations': 0}
        args.metadata.write_text(json.dumps(metadata, indent=2) + '\n')

        def observe():
            while not finished.wait(metadata['sample_interval_seconds']):
                for role in metadata['roles'].values():
                    try:
                        current = snapshot(role['tid'])
                    except (FileNotFoundError, ProcessLookupError):
                        continue
                    except OSError as error:
                        metadata['monitor_error'] = str(error)
                        return
                    role['last'] = current
                    role['observations'] += 1
                    role['sampled_cpus'] = sorted(set(role['sampled_cpus']) | {current['last_cpu']})

        monitor = threading.Thread(target=observe)
        monitor.start()
        # The sender sees READY only after both requested masks have been verified.
        print(ready, end='', flush=True)
        for line in child.stdout:
            print(line, end='', flush=True)
        returncode = child.wait()
        finished.set()
        monitor.join()
        for role in metadata['roles'].values():
            role['migration_delta'] = (role['last'].get('se.nr_migrations', 0) -
                                       role['initial'].get('se.nr_migrations', 0))
            role['involuntary_switch_delta'] = (role['last'].get('nr_involuntary_switches', 0) -
                                                role['initial'].get('nr_involuntary_switches', 0))
        args.metadata.write_text(json.dumps(metadata, indent=2) + '\n')
        return returncode
    finally:
        finished.set()
        if monitor is not None:
            monitor.join()
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        for signum, handler in old_handlers.items():
            signal.signal(signum, handler)


if __name__ == '__main__':
    raise SystemExit(main())
