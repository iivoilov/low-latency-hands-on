#!/usr/bin/env python3
"""Convert captured PHC timestamps to MONOTONIC using bracketing samples."""
import argparse
import bisect
import csv
import json
import math
from pathlib import Path
import statistics


def summary(values):
    if not values:
        return {'samples': 0, 'min_ns': None, 'p50_ns': None, 'p99_ns': None,
                'p99_9_ns': None, 'max_ns': None, 'mean_ns': None}
    data = sorted(values)
    p = lambda q: data[math.ceil(len(data) * q) - 1]
    return {'samples': len(data), 'min_ns': data[0], 'p50_ns': p(.5),
            'p99_ns': p(.99), 'p99_9_ns': p(.999), 'max_ns': data[-1],
            'mean_ns': statistics.mean(data)}


class Mapping:
    def __init__(self, rows):
        self.anchors = []
        for row in rows:
            h = int(row['hardware_phc_ns'])
            before, after = int(row['monotonic_before_ns']), int(row['monotonic_after_ns'])
            if after < before:
                raise ValueError('Decreasing calibration bracket')
            self.anchors.append((h, before + (after - before) // 2, (after - before) / 2))
        if len(self.anchors) < 2 or any(b[0] <= a[0] or b[1] <= a[1] for a, b in zip(self.anchors, self.anchors[1:])):
            raise ValueError('Need strictly increasing PHC and MONOTONIC anchors')
        self.times = [a[0] for a in self.anchors]

    @staticmethod
    def latency_between(hardware, user, a, b):
        fraction = (hardware - a[0]) / (b[0] - a[0])
        offset_a = a[1] - a[0]
        offset_delta = (b[1] - b[0]) - offset_a
        # Cancel large epoch offsets as integers before any floating-point math.
        latency = (user - (hardware + offset_a)) - fraction * offset_delta
        uncertainty = (1 - fraction) * a[2] + fraction * b[2]
        return latency, uncertainty

    def latency(self, hardware, user):
        if hardware < self.times[0] or hardware > self.times[-1]:
            raise ValueError('Packet is outside the calibration interval; no extrapolation')
        i = min(max(bisect.bisect_right(self.times, hardware) - 1, 0), len(self.times) - 2)
        return self.latency_between(hardware, user, self.anchors[i], self.anchors[i + 1])

    def residuals(self):
        return [self.latency_between(b[0], b[1], a, c)[0]
                for a, b, c in zip(self.anchors, self.anchors[1:], self.anchors[2:])]


def analyze(prefix):
    capture = json.loads(Path(str(prefix) + '-capture.json').read_text())
    with Path(str(prefix) + '-packets.csv').open() as stream:
        rows = [{k: int(v) for k, v in row.items()} for row in csv.DictReader(stream)]
    if len(rows) != capture['packets']:
        raise ValueError('Packet CSV disagrees with capture metadata')
    lower = max((r['user_realtime_ns'] - r['monotonic_after_realtime_ns'] for r in rows), default=0)
    upper = min((r['user_realtime_ns'] - r['user_monotonic_ns'] for r in rows), default=-1)
    real_mapping_valid = lower <= upper
    realtime_offset = lower + (upper - lower) // 2
    software = [r['user_monotonic_ns'] - (r['software_realtime_ns'] - realtime_offset)
                for r in rows if r['software_realtime_ns']] if real_mapping_valid else []
    output = {'capture': capture, 'packet_samples': len(rows),
              'software_to_user_ns': summary(software),
              'realtime_monotonic_mapping': {'valid': real_mapping_valid,
                  'offset_lower_ns': lower, 'offset_upper_ns': upper,
                  'uncertainty_half_width_ns': (upper - lower) / 2 if real_mapping_valid else None},
              'hardware_to_user_ns': summary([]), 'hardware_to_kernel_ns': summary([]),
              'hardware_mapping': None}
    if capture['mode'] == 'hardware':
        with Path(str(prefix) + '-clocks.csv').open() as stream:
            mapping = Mapping(list(csv.DictReader(stream)))
        wire, kernel, uncertainties = [], [], []
        outside = missing = 0
        for r in rows:
            if not r['hardware_phc_ns']:
                missing += 1
                continue
            try:
                latency, error = mapping.latency(r['hardware_phc_ns'], r['user_monotonic_ns'])
            except ValueError:
                outside += 1
                continue
            wire.append(latency); uncertainties.append(error)
            if real_mapping_valid and r['software_realtime_ns']:
                to_kernel, _ = mapping.latency(r['hardware_phc_ns'], r['software_realtime_ns'] - realtime_offset)
                kernel.append(to_kernel)
        residuals = mapping.residuals()
        output.update(hardware_to_user_ns=summary(wire), hardware_to_kernel_ns=summary(kernel))
        output['hardware_mapping'] = {
            'method': 'piecewise-linear midpoint of minimum-width MONOTONIC/PHC/MONOTONIC brackets (25 candidates)',
            'anchors': len(mapping.anchors), 'read_half_width_ns': summary([a[2] for a in mapping.anchors]),
            'interpolated_half_width_ns': summary(uncertainties),
            'leave_one_out_residual_abs_ns': summary([abs(v) for v in residuals]),
            'missing_hardware_timestamps': missing, 'outside_calibration_interval': outside,
            'negative_latency_estimates': sum(v < 0 for v in wire),
            'negative_beyond_bracket': sum(v < -e for v, e in zip(wire, uncertainties)),
            'note': 'Bracket half-widths describe read uncertainty. Interpolation assumes smooth drift; residuals are diagnostic, not a certified error bound.'}
    output['measurement_valid'] = bool(capture['capture_valid'] and real_mapping_valid and
        (capture['mode'] == 'software' or (len(rows) == output['hardware_to_user_ns']['samples'] and
         output['hardware_mapping']['negative_beyond_bracket'] == 0)))
    destination = Path(str(prefix) + '-analysis.json')
    destination.write_text(json.dumps(output, indent=2) + '\n')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path)
    args = parser.parse_args()
    result = analyze(args.prefix)
    print(json.dumps({k: result[k] for k in ['measurement_valid', 'software_to_user_ns', 'hardware_to_user_ns', 'hardware_mapping']}, indent=2))
    if not result['measurement_valid']:
        raise SystemExit(1)

if __name__ == '__main__':
    main()
