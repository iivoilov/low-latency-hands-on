"""Check launch ordering and per-thread affinity using a fabricated two-thread child."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


LAUNCHER = Path(__file__).with_name('pin_processor.py')
CHILD = """
import threading,time
done=threading.Event()
worker=threading.Thread(target=done.wait)
worker.start()
print('READY port=12345',flush=True)
time.sleep(0.8)
done.set()
worker.join()
print('finished',flush=True)
"""


class LauncherTests(unittest.TestCase):
    def launch(self, *flags):
        with tempfile.TemporaryDirectory() as directory:
            metadata = Path(directory) / 'affinity.json'
            process = subprocess.Popen([sys.executable, str(LAUNCHER), '--metadata', str(metadata),
                                        *map(str, flags), '--', sys.executable, '-u', '-c', CHILD],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                self.assertEqual(process.stdout.readline(), 'READY port=12345\n')
                initial = json.loads(metadata.read_text())  # Must exist before READY is forwarded.
                stdout, stderr = process.communicate(timeout=4)
                self.assertEqual(process.returncode, 0, stderr)
                self.assertIn('finished', stdout)
                return initial, json.loads(metadata.read_text())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()

    def test_pins_distinct_roles_before_forwarding_ready(self):
        cpus = sorted(os.sched_getaffinity(0))
        rx, l2 = cpus[0], cpus[-1]
        initial, final = self.launch('--rx-cpu', rx, '--l2-cpu', l2)
        self.assertNotEqual(initial['roles']['rx']['tid'], initial['roles']['l2']['tid'])
        for role, cpu in [('rx', rx), ('l2', l2)]:
            self.assertEqual(initial['roles'][role]['initial']['allowed_cpus'], [cpu])
            self.assertEqual(final['roles'][role]['last']['allowed_cpus'], [cpu])
            self.assertGreater(final['roles'][role]['observations'], 0)

    def test_observer_pinning_preserves_child_default_mask(self):
        allowed = sorted(os.sched_getaffinity(0))
        initial, final = self.launch('--observer-cpu', allowed[-1])
        for role in ('rx', 'l2'):
            self.assertEqual(initial['roles'][role]['initial']['allowed_cpus'], allowed)
            self.assertEqual(final['roles'][role]['last']['allowed_cpus'], allowed)

    def test_rejects_cpu_outside_inherited_mask(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, str(LAUNCHER), '--metadata', directory + '/affinity.json',
                                     '--rx-cpu', str(max(os.sched_getaffinity(0)) + 1),
                                     '--', sys.executable, '-c', 'raise Exception("must not launch")'],
                                    capture_output=True, text=True, timeout=3)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('outside the inherited allowed set', result.stderr)
            self.assertNotIn('must not launch', result.stderr)

    def test_startup_cpu_controls_initial_allocation_mask(self):
        cpu = min(os.sched_getaffinity(0))
        initial, final = self.launch('--startup-cpu', cpu)
        for role in ('rx', 'l2'):
            self.assertEqual(initial['roles'][role]['initial']['allowed_cpus'], [cpu])
        self.assertEqual(final['startup_cpu'], cpu)

    def test_rejects_unexpected_thread_count(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, str(LAUNCHER), '--metadata', directory + '/affinity.json',
                                     '--', sys.executable, '-u', '-c',
                                     "import time; print('READY port=12345',flush=True); time.sleep(5)"],
                                    capture_output=True, text=True, timeout=4)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('READY port=', result.stdout)
            self.assertIn('exactly one RX worker', result.stderr)


if __name__ == '__main__':
    unittest.main()
