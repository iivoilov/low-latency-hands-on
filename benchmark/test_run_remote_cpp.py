"""Protect experiment summaries from treating failed or absent runs as measurements."""

import unittest
from pathlib import Path
import subprocess
import sys
import tempfile

from run_remote_cpp import summarize, affinity_settings, latency_measurement


def record(valid, tail, rate=9.2):
    return {'build': 'test', 'valid': valid, 'rx_to_l2': {
        'p50_ns': tail // 4, 'p99_ns': tail // 2, 'p99_9_ns': tail},
        'sender_mbps': rate, 'rate_within_5pct': rate is not None}


class SummaryTests(unittest.TestCase):
    def test_failed_run_is_counted_but_excluded_from_latency(self):
        result = summarize([record(True, 100), record(False, 10000), record(True, 300)], ['test'])[0]
        self.assertEqual((result['attempts'], result['valid_runs'], result['invalid_runs']), (3, 2, 1))
        self.assertEqual(result['median_p99_9_ns'], 200)
        self.assertEqual((result['min_p99_9_ns'], result['max_p99_9_ns']), (100, 300))

    def test_all_failed_runs_have_no_latency_estimate(self):
        result = summarize([record(False, 10)], ['test'])[0]
        self.assertEqual((result['attempts'], result['valid_runs'], result['invalid_runs']), (1, 0, 1))
        self.assertIsNone(result['median_p99_9_ns'])
        self.assertIsNone(result['median_sender_mbps'])
        self.assertFalse(result['all_rates_within_5pct'])


    def test_absent_build_has_no_measurements(self):
        result = summarize([], ['test'])[0]
        self.assertEqual(result['attempts'], 0)
        self.assertIsNone(result['median_p50_ns'])

    def test_missing_sender_rate_is_not_reported_as_verified(self):
        result = summarize([record(True, 100, None)], ['test'])[0]
        self.assertIsNone(result['median_sender_mbps'])
        self.assertFalse(result['all_rates_within_5pct'])

    def test_l1_and_historical_l2_are_never_combined(self):
        old = record(True, 100)
        new = record(True, 900)
        new['latency'] = new.pop('rx_to_l2')
        new['timing_boundary'] = 'recvmsg_return_to_message_l1_decision'
        summary = summarize([old, new], ['test'])
        self.assertEqual(len(summary), 2)
        self.assertEqual([r['median_p99_9_ns'] for r in summary], [100, 900])
        self.assertTrue(all(r['attempts'] == 1 for r in summary))

    def test_metric_boundary_must_agree(self):
        metric, boundary, latency = latency_measurement({'rx_to_l1_decision': {'samples': 12}})
        self.assertEqual((metric, boundary, latency['samples']),
                         ('rx_to_l1_decision', 'recvmsg_return_to_message_l1_decision', 12))
        with self.assertRaises(ValueError):
            latency_measurement({'rx_to_l1_decision': {},
                                 'timing_boundary': 'recvmsg_return_to_message_l2_update'})


class AffinityBaselineTests(unittest.TestCase):
    baseline = {'rx_cpu': 2, 'l2_cpu': 3, 'observer_cpu': 15}

    def test_default_uses_selected_baseline(self):
        self.assertEqual(affinity_settings(self.baseline), self.baseline)

    def test_explicit_unpinned_control_keeps_observer(self):
        self.assertEqual(affinity_settings(self.baseline, unpinned=True),
                         {'rx_cpu': None, 'l2_cpu': None, 'observer_cpu': 15})

    def test_overrides_remain_available(self):
        self.assertEqual(affinity_settings(self.baseline, rx_cpu=10, l2_cpu=11)['rx_cpu'], 10)

    def test_conflicting_unpinned_arguments_are_rejected(self):
        with self.assertRaises(ValueError):
            affinity_settings(self.baseline, rx_cpu=2, unpinned=True)

    def test_baseline_is_scoped_to_its_receiver_host(self):
        baseline = {**self.baseline, 'receiver_host': 'ryzen'}
        self.assertEqual(affinity_settings(baseline, receiver_host='ryzen'), self.baseline)
        for overrides in ({}, {'rx_cpu': 2}, {'rx_cpu': 2, 'l2_cpu': 3}):
            with self.assertRaisesRegex(ValueError, 'configuration is for ryzen'):
                affinity_settings(baseline, receiver_host='epyc', **overrides)
        self.assertEqual(affinity_settings(baseline, receiver_host='epyc',
                                           rx_cpu=10, l2_cpu=11, observer_cpu=12),
                         {'rx_cpu': 10, 'l2_cpu': 11, 'observer_cpu': 12})

    def test_other_receiver_unpinned_does_not_inherit_observer_cpu(self):
        baseline = {**self.baseline, 'receiver_host': 'ryzen'}
        self.assertEqual(affinity_settings(baseline, receiver_host='epyc', unpinned=True),
                         {'rx_cpu': None, 'l2_cpu': None, 'observer_cpu': None})

    def test_cli_rejects_foreign_receiver_before_creating_results(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'unused-results'
            command = [sys.executable, str(Path(__file__).with_name('run_remote_cpp.py')),
                       '--receiver-host', 'epyc', '--sender-host', 'ryzen',
                       '--receiver-root', '/unused', '--sender-root', '/unused',
                       '--receiver-interface', '127.0.0.1', '--sender-interface', '127.0.0.1',
                       '--builds', 'gcc-o3', '--profile', 'missing.pcap', '--output', str(output)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 2)
            self.assertIn('configuration is for ryzen', result.stderr)
            self.assertFalse(output.exists())



if __name__ == '__main__':
    unittest.main()
