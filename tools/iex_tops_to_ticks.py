#!/usr/bin/env python3
"""Converts an IEX HIST TOPS 1.6 capture (``*.pcap.gz``) into the engine's tick schema
(``timestamp_ms,symbol,price,size,side``) for a list of symbols.

IEX publishes every trade printed on its exchange, free, at
https://iextrading.com/trading/market-data/#hist (one gzipped pcap per day). The feed
has no aggressor flag, so each trade is signed with the Lee-Ready rule against the IEX
quote in force when it printed: above the quote midpoint is a buy, below is a sell, and
a trade exactly at the midpoint takes the sign of the last price change (tick rule).

Standard library only (gzip + struct), streaming: memory stays flat on a 3 GB file.

    python tools/iex_tops_to_ticks.py data/raw/iex_tops_20230421.pcap.gz \
        --symbols SQM,ALB,LTHM,LAC,PLL --out data/raw/iex_lithium_20230421.csv

Limitations, stated in the README: IEX handles a few percent of US equity volume, so
this is IEX's slice of the tape, and the quotes are IEX's own best bid/offer, not the NBBO.
"""
from __future__ import annotations

import argparse
import csv
import gzip
import struct
import sys

PCAP_GLOBAL_HEADER = 24
PCAP_RECORD_HEADER = struct.Struct("<IIII")
ETH_IP_UDP = 14 + 20 + 8
IEXTP_HEADER = 40

QUOTE = 0x51  # 'Q' quote update
TRADE = 0x54  # 'T' trade report
QUOTE_BODY = struct.Struct("<BBq8sIqqI")   # type, flags, ts_ns, symbol, bid_size, bid_px, ask_px, ask_size
TRADE_BODY = struct.Struct("<BBq8sIqq")    # type, sale_flags, ts_ns, symbol, size, price, trade_id
PRICE_SCALE = 1e-4


PCAPNG_MAGIC = b"\x0a\x0d\x0d\x0a"
PCAPNG_EPB = 6  # Enhanced Packet Block


def _packets(f):
    """Yields raw link-layer packets from a classic pcap or a pcapng stream (IEX ships
    pcapng from 2018 on)."""
    magic = f.read(4)
    if magic == PCAPNG_MAGIC:
        block_len = struct.unpack("<I", f.read(4))[0]
        f.read(block_len - 8)  # rest of the section header block
        while True:
            head = f.read(8)
            if len(head) < 8:
                return
            block_type, block_len = struct.unpack("<II", head)
            body = f.read(block_len - 8)
            if block_type == PCAPNG_EPB:
                captured = struct.unpack_from("<I", body, 12)[0]
                yield body[20:20 + captured]
        return
    f.read(PCAP_GLOBAL_HEADER - 4)
    while True:
        header = f.read(PCAP_RECORD_HEADER.size)
        if len(header) < PCAP_RECORD_HEADER.size:
            return
        _, _, incl_len, _ = PCAP_RECORD_HEADER.unpack(header)
        yield f.read(incl_len)


def convert(path: str, symbols: set[str], out_path: str) -> dict:
    quotes: dict[str, tuple[float, float]] = {}
    last_price: dict[str, float] = {}
    last_sign: dict[str, str] = {}
    counts = {"trades": 0, "at_mid_tick_rule": 0, "no_quote": 0}

    with gzip.open(path, "rb") as f, open(out_path, "w", newline="") as out:
        writer = csv.writer(out)
        writer.writerow(["timestamp_ms", "symbol", "price", "size", "side"])
        for packet in _packets(f):
            offset = ETH_IP_UDP + IEXTP_HEADER
            if len(packet) <= offset:
                continue
            msg_count = struct.unpack_from("<H", packet, ETH_IP_UDP + 14)[0]
            for _ in range(msg_count):
                if offset + 2 > len(packet):
                    break
                length = struct.unpack_from("<H", packet, offset)[0]
                body = offset + 2
                offset = body + length
                kind = packet[body]
                if kind == QUOTE and length >= QUOTE_BODY.size:
                    _, _, _, sym, bid_size, bid_px, ask_px, ask_size = QUOTE_BODY.unpack_from(packet, body)
                    symbol = sym.rstrip(b" ").decode("latin-1")
                    if symbol in symbols:
                        if bid_size > 0 and ask_size > 0 and bid_px > 0 and ask_px > 0:
                            quotes[symbol] = (bid_px * PRICE_SCALE, ask_px * PRICE_SCALE)
                        else:
                            quotes.pop(symbol, None)
                elif kind == TRADE and length >= TRADE_BODY.size:
                    _, _, ts_ns, sym, size, price_raw, _ = TRADE_BODY.unpack_from(packet, body)
                    symbol = sym.rstrip(b" ").decode("latin-1")
                    if symbol not in symbols:
                        continue
                    price = price_raw * PRICE_SCALE
                    quote = quotes.get(symbol)
                    side = None
                    if quote is not None:
                        mid = (quote[0] + quote[1]) / 2
                        if price > mid:
                            side = "B"
                        elif price < mid:
                            side = "S"
                        else:
                            counts["at_mid_tick_rule"] += 1
                    else:
                        counts["no_quote"] += 1
                    if side is None:
                        previous = last_price.get(symbol)
                        if previous is None or price == previous:
                            side = last_sign.get(symbol, "B")
                        else:
                            side = "B" if price > previous else "S"
                    last_price[symbol] = price
                    last_sign[symbol] = side
                    writer.writerow([ts_ns // 1_000_000, symbol, f"{price:.4f}", size, side])
                    counts["trades"] += 1
    return counts


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("pcap")
    parser.add_argument("--symbols", required=True, help="Comma-separated, e.g. SQM,ALB")
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    counts = convert(args.pcap, {s.strip().upper() for s in args.symbols.split(",")}, args.out)
    print(counts, file=sys.stderr)


if __name__ == "__main__":
    main()
