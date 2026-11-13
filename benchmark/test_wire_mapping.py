import csv
import json
from pathlib import Path
import tempfile
import unittest
from analyze_wire_capture import Mapping, analyze, summary

class MappingTests(unittest.TestCase):
    def row(self, h, m, error=10):
        return {'hardware_phc_ns':h,'monotonic_before_ns':m-error,'monotonic_after_ns':m+error}

    def test_preserves_nanoseconds_with_unix_epoch_values(self):
        epoch=1_800_000_000_000_000_000
        m=1_000_000_000_000
        mapping=Mapping([self.row(epoch,m),self.row(epoch+1_000_000,m+1_000_100)])
        latency,error=mapping.latency(epoch+500_000,m+500_050+123)
        self.assertEqual(latency,123)
        self.assertEqual(error,10)

    def test_tracks_linear_drift_without_extrapolation(self):
        mapping=Mapping([self.row(1000,5000),self.row(2000,6100),self.row(3000,7200)])
        self.assertEqual(mapping.latency(1500,5600)[0],50)
        self.assertEqual(mapping.residuals(),[0])
        for h in [999,3001]:
            with self.assertRaises(ValueError):mapping.latency(h,10000)

    def test_rejects_nonmonotonic_clock_samples(self):
        with self.assertRaises(ValueError):Mapping([self.row(1000,5000),self.row(999,6100)])

    def test_keeps_negative_values_and_nearest_rank(self):
        mapping=Mapping([self.row(1000,5000),self.row(2000,6000)])
        self.assertEqual(mapping.latency(1500,5499)[0],-1)
        self.assertEqual(summary(list(range(1,1001)))['p99_9_ns'],999)

    def analyze_software(self, realtime_offsets):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / 'capture'
            Path(str(prefix) + '-capture.json').write_text(json.dumps(
                {'packets': len(realtime_offsets), 'capture_valid': True, 'mode': 'software'}))
            rows = []
            for i, offset in enumerate(realtime_offsets):
                user = 10000 + i * 1000
                rows.append({'user_monotonic_ns': user, 'user_realtime_ns': user + offset,
                             'monotonic_after_realtime_ns': user + 20,
                             'hardware_phc_ns': 0, 'software_realtime_ns': user + offset - 100})
            with Path(str(prefix) + '-packets.csv').open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
                writer.writeheader()
                writer.writerows(rows)
            return analyze(prefix)

    def test_software_conversion_uses_clock_read_brackets(self):
        result = self.analyze_software([100000, 100000])
        self.assertTrue(result['measurement_valid'])
        self.assertEqual(result['software_to_user_ns']['p50_ns'], 90)
        self.assertEqual(result['realtime_monotonic_mapping']['uncertainty_half_width_ns'], 10)

    def test_realtime_step_invalidates_software_measurement(self):
        result = self.analyze_software([100000, 101000])
        self.assertFalse(result['measurement_valid'])
        self.assertFalse(result['realtime_monotonic_mapping']['valid'])
        self.assertEqual(result['software_to_user_ns']['samples'], 0)

if __name__=='__main__':unittest.main()
