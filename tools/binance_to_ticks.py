#!/usr/bin/env python3
"""Converts a Binance aggTrades daily ZIP (https://data.binance.vision) into the engine's
tick schema (``timestamp_ms,symbol,price,size,side``).

Unlike the IEX feed, Binance records the aggressor of every trade: ``is_buyer_maker``
true means the buyer was resting and the seller hit it, so the trade is a sell. No
inference is needed, which makes it the reference for how well Lee-Ready signing works.

    python tools/binance_to_ticks.py data/raw/BTCUSDT-aggTrades-2024-08-05.zip --out data/raw/btc_20240805.csv
"""
from __future__ import annotations

import argparse
import csv
import io
import zipfile


def convert(path: str, out_path: str, symbol: str | None = None) -> int:
    with zipfile.ZipFile(path) as z:
        name = z.namelist()[0]
        symbol = symbol or name.split("-")[0]
        n = 0
        with z.open(name) as raw, open(out_path, "w", newline="") as out:
            writer = csv.writer(out)
            writer.writerow(["timestamp_ms", "symbol", "price", "size", "side"])
            for row in csv.reader(io.TextIOWrapper(raw, encoding="ascii")):
                if not row or not row[0].isdigit():
                    continue  # header line in newer files
                # agg_id, price, qty, first_id, last_id, transact_time, is_buyer_maker, is_best_match
                ts = int(row[5])
                if ts > 10**14:  # microseconds in files from 2025 on
                    ts //= 1000
                side = "S" if row[6].strip().lower() == "true" else "B"
                writer.writerow([ts, symbol, row[1], row[2], side])
                n += 1
    return n


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("zip")
    parser.add_argument("--out", required=True)
    parser.add_argument("--symbol", default=None)
    args = parser.parse_args()
    print(convert(args.zip, args.out, args.symbol), "trades")


if __name__ == "__main__":
    main()
