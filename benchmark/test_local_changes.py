import copy
import unittest

from run_local_changes import summarize, validate


class LocalChangesTests(unittest.TestCase):
    def setUp(self):
        self.oracle = {
            'manifest': {'updates': 100, 'initialization_messages': 4,
                         'finite_average_mbps': 2, 'final_book': [{'symbol': 'TEST', 'quantity': 12}]},
            'final_l1': [{'symbol': 'TEST', 'bid': {'quantity': 12}, 'last': None}],
            'l1_decisions': 104, 'l1_updates': 90, 'l1_no_updates': 14, 'retained_trades': 2,
        }
        self.result = {
            'benchmark_valid': True, 'complete': True, 'book_current': True,
            'timing_boundary': 'recvmsg_return_to_message_l1_decision',
            'rx_to_l1_decision': {'samples': 100, 'p50_ns': 10, 'p99_ns': 20, 'p99_9_ns': 30},
            'warmup_messages': 4, 'final_book': self.oracle['manifest']['final_book'],
            **{k: v for k, v in self.oracle.items() if k != 'manifest'},
            **{k: 0 for k in ('kernel_dropped_packets', 'ring_dropped_packets', 'truncated_packets',
                              'sequence_errors', 'missing_messages', 'protocol_errors',
                              'skipped_market_messages', 'gap_ranges_dropped')},
        }
        self.affinity = {'monitor_error': '', 'roles': {
            role: {'requested_cpu': cpu, 'observations': 2,
                   'initial': {'allowed_cpus': [cpu]}, 'last': {'allowed_cpus': [cpu]}}
            for role, cpu in [('rx', 2), ('l2', 4)]}}

    def checks(self, rate=2):
        return validate(self.result, self.oracle, self.affinity, rate)[0]

    def test_valid_report(self):
        self.assertTrue(all(self.checks().values()))

    def test_reject_loss_despite_valid_flag(self):
        self.result['ring_dropped_packets'] = 1
        self.assertFalse(self.checks()['loss_free'])

    def test_reject_wrong_state_or_count(self):
        self.result['final_l1'] = []
        self.result['retained_trades'] += 1
        checks = self.checks()
        self.assertFalse(checks['l1'])
        self.assertFalse(checks['counts'])

    def test_reject_missing_samples_and_wrong_warmup(self):
        self.result['rx_to_l1_decision']['samples'] = 99
        self.result['warmup_messages'] = 3
        self.assertFalse(self.checks()['timing'])
        self.assertFalse(self.checks()['warmup'])

    def test_reject_bad_placement_or_rate(self):
        self.affinity['roles']['rx']['last']['allowed_cpus'] = [2, 3]
        self.assertFalse(self.checks()['affinity'])
        self.assertFalse(self.checks(1)['rate'])
        self.assertFalse(self.checks(None)['rate'])

    def test_summary_excludes_failed_attempt_and_keeps_regression(self):
        variants = {'baseline-cpp': {'language': 'cpp', 'change': 'baseline'},
                    'both-cpp': {'language': 'cpp', 'change': 'both'}}
        baseline = {'variant': 'baseline-cpp', 'valid': True,
                    'latency': {'p50_ns': 10, 'p99_ns': 20, 'p99_9_ns': 30}}
        regression = copy.deepcopy(baseline)
        regression['variant'] = 'both-cpp'
        regression['latency']['p50_ns'] = 12
        rows = summarize([baseline, regression, {'variant': 'both-cpp', 'valid': False}], variants)
        self.assertEqual((rows[1]['attempts'], rows[1]['valid_runs']), (2, 1))
        self.assertAlmostEqual(rows[1]['p50_ns_change_pct'], 20)
        self.assertEqual(rows[1]['min_p99_9_ns'], 30)


if __name__ == '__main__':
    unittest.main()
