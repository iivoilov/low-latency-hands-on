"""Keep paired decoder summaries honest about missing or inconsistent runs."""

import unittest

from run_decode_breakdown import summarize


def run(repeat, decoder, ns, checksum=123):
    return {'repeat': repeat, 'decoder': decoder, 'ns_per_message': ns, 'checksum': checksum}


class BreakdownTests(unittest.TestCase):
    def test_three_decoders_use_complete_matching_rounds(self):
        rows = [run(1, 'packed', 8), run(1, 'zero-copy', 4), run(1, 'original', 10)]
        summary = summarize({'runs': rows})
        self.assertEqual(summary['zero_copy_ns'], 4)
        self.assertEqual(summary['zero_copy_vs_original_pct'], -60)
        self.assertEqual(summary['zero_copy_vs_packed_pct'], -50)
        self.assertEqual(summary['zero_copy_faster_original_pairs'], 1)
        with self.assertRaises(ValueError):
            summarize({'runs': rows + [run(2, 'original', 10), run(2, 'packed', 8)]})
        with self.assertRaises(ValueError):
            summarize({'runs': rows[:-2] + [run(1, 'zero-copy', 4, checksum=124), run(1, 'original', 10)]})
    def test_pairing_uses_repeat_not_execution_order(self):
        summary = summarize({'runs': [run(1, 'packed', 5), run(1, 'original', 10),
                                      run(2, 'original', 8), run(2, 'packed', 10)]})
        self.assertEqual(summary['original_ns'], 9)
        self.assertEqual(summary['packed_ns'], 7.5)
        self.assertEqual(summary['packed_faster_pairs'], 1)
        self.assertEqual((summary['paired_change_min_pct'], summary['paired_change_max_pct']), (-50, 25))

    def test_missing_duplicate_or_wrong_results_cannot_be_summarized(self):
        for runs in [[], [run(1, 'original', 10)],
                     [run(1, 'original', 10), run(1, 'original', 10), run(1, 'packed', 5)],
                     [run(1, 'original', 10), run(1, 'packed', 5, checksum=124)]]:
            with self.assertRaises(ValueError):
                summarize({'runs': runs})
