"""Protect experiment summaries from treating failed or absent runs as measurements."""

import unittest

from run_remote_cpp import summarize, affinity_settings


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



if __name__ == '__main__':
    unittest.main()
