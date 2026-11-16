#!/usr/bin/env python3
"""Freeze S&P 500 constituent symbols and dated iShares IVV reference prices.

Reads downloaded files only. Networking and price lookup never occur in replay.
The original vendor snapshot remains separate from source control.
"""

import argparse
import csv
import datetime
from decimal import Decimal
import hashlib
import io
import json
from pathlib import Path


HOLDINGS_URL = 'https://www.ishares.com/us/products/239726/ishares-core-s-p-500-etf/latest-holdings.csv'
CONSTITUENTS_URL = 'https://raw.githubusercontent.com/datasets/s-and-p-500-companies/main/data/constituents.csv'


def convert(holdings, constituents):
    text = holdings.read_text(encoding='utf-8-sig')
    metadata = list(csv.reader(io.StringIO(text[:text.index('Ticker,Name,')])))
    date = next(row[1] for row in metadata if row and row[0] == 'Fund Holdings as of')
    as_of = datetime.datetime.strptime(date, '%b %d, %Y').date().isoformat()
    with constituents.open(encoding='utf-8-sig') as stream:
        members = [row['Symbol'] for row in csv.DictReader(stream)]
    if len(members) < 500 or len(set(members)) != len(members):
        raise ValueError('Need at least 500 unique constituent symbols')
    prices = {}
    excluded = []
    for row in csv.DictReader(io.StringIO(text[text.index('Ticker,Name,'):])):
        if row.get('Asset Class') != 'Equity':
            continue
        symbol = row['Ticker'].replace(' ', '.')  # BRK B / BF B -> BRK.B / BF.B.
        if row.get('Type') != 'EQUITY' or symbol not in members:
            excluded.append(row['Ticker'])
            continue
        if row['Currency'] != 'USD' or row['Market Currency'] != 'USD':
            raise ValueError(f'Non-USD price for {symbol}')
        price = Decimal(row['Price'].replace(',', '')) * 10000
        if not price.is_finite() or price != price.to_integral_value() or not 10000 < price < 2**63 - 10000:
            raise ValueError(f'Invalid reference price for {symbol}')
        if symbol in prices:
            raise ValueError(f'Duplicate holding: {symbol}')
        if not 1 <= len(symbol) <= 6 or any(not '!' <= ch <= '~' or ch == ',' for ch in symbol):
            raise ValueError(f'Symbol does not fit generator wire layout: {symbol}')
        prices[symbol] = int(price)
    missing = set(members) - prices.keys()
    if missing:
        raise ValueError(f'Missing constituent prices: {sorted(missing)}')
    rows = sorted(prices.items())
    provenance = {
        'reference_as_of': as_of,
        'price_kind': 'IVV holdings vendor reference price; not a verified official closing print',
        'universe_kind': 'S&P 500 constituent list cross-checked against IVV equity holdings',
        'symbol_count': len(rows), 'symbol_aliases': {'BRK B': 'BRK.B', 'BF B': 'BF.B'},
        'excluded_equity_holdings': excluded,
        'inputs': {name: {'url': url, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                   for name, path, url in [('holdings', holdings, HOLDINGS_URL),
                                           ('constituents', constituents, CONSTITUENTS_URL)]},
    }
    return rows, provenance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--holdings', type=Path, required=True)
    parser.add_argument('--constituents', type=Path, required=True)
    parser.add_argument('--directory', type=Path, required=True, help='New generated-set directory; must not exist')
    args = parser.parse_args()
    rows, provenance = convert(args.holdings, args.constituents)
    args.directory.mkdir(parents=True, exist_ok=False)
    output = args.directory / 'universe.csv'
    with output.open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['symbol', 'price_1e4'])
        writer.writerows(rows)
    provenance['universe_sha256'] = hashlib.sha256(output.read_bytes()).hexdigest()
    (args.directory / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print(json.dumps(provenance, indent=2))


if __name__ == '__main__':
    main()
