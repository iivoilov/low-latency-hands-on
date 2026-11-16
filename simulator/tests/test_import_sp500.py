"""Only fabricated constituent and holdings records are used here."""

import csv
from pathlib import Path
import tempfile
import unittest

from simulator.import_sp500 import convert


class ImportTests(unittest.TestCase):
    def test_complete_snapshot_and_missing_price(self):
        with tempfile.TemporaryDirectory() as directory:
            holdings, members = (Path(directory) / name for name in ('holdings.csv', 'members.csv'))
            symbols = ['BRK.B', *[f'S{i:03}' for i in range(499)]]
            members.write_text('Symbol\n' + '\n'.join(symbols) + '\n')
            with holdings.open('w', newline='') as stream:
                writer = csv.writer(stream)
                writer.writerow(['Fund Holdings as of', 'Nov 13, 2026'])
                writer.writerow(['Ticker', 'Name', 'Type', 'Asset Class', 'Price', 'Currency', 'Market Currency'])
                for symbol in symbols:
                    writer.writerow([symbol.replace('.', ' '), 'Fabricated', 'EQUITY', 'Equity', '123.45', 'USD', 'USD'])
                writer.writerow(['STUB', 'Residual', 'EQUITY', 'Equity', '0.01', 'USD', 'USD'])
            rows, provenance = convert(holdings, members)
            self.assertEqual(rows, [(s, 1234500) for s in sorted(symbols)])
            self.assertEqual(provenance['reference_as_of'], '2026-11-13')
            self.assertEqual(provenance['excluded_equity_holdings'], ['STUB'])
            members.write_text(members.read_text() + 'MISSING\n')
            with self.assertRaisesRegex(ValueError, 'Missing constituent prices'):
                convert(holdings, members)


if __name__ == '__main__':
    unittest.main()
