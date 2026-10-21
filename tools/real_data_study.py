#!/usr/bin/env python3
"""Real-data study: what the engine's alerts mean on real trade tapes.

Reads a tick CSV (engine schema) and the engine's results CSV for it, and reports:

- How often each detector fires, against the nominal rate the threshold was built for
  (alpha = 0.27%, a Gaussian 3-sigma tail): the engine's MAD + Student-t(4) statistic,
  and a Gaussian EWMA z-score (alpha 0.05, 30 warm-up windows) recomputed here exactly
  as `include/ewma_zscore.hpp` does it.
- Whether an alert says anything about price: the absolute and signed return over the
  next 5 seconds after alert windows versus all other traded windows, in basis points.
  The signed version asks whether the price moves in the direction of the imbalance.

Needs numpy and pandas (analysis only; the engine itself stays dependency-free).

    python tools/real_data_study.py data/raw/btc_20240805.csv data/raw/btc_20240805.results.csv --label "BTC 2024-08-05"
"""
from __future__ import annotations

import argparse
import json

import numpy as np
import pandas as pd

T_CRITICAL = 6.62021  # engine's |t| threshold for dof=4 at the Gaussian 3-sigma tail probability
NOMINAL_RATE = 0.0027
HORIZON_WINDOWS = 10  # 10 x 500 ms = 5 s


def ewma_z(x: np.ndarray, alpha: float = 0.05, warmup: int = 30, min_std: float = 1e-6) -> np.ndarray:
    """Same recursion as include/ewma_zscore.hpp: score against prior mean/variance, then update."""
    z = np.full(len(x), np.nan)
    mean = var = 0.0
    for i, v in enumerate(x):
        if i < warmup:
            # running mean/variance seed
            n = i + 1
            delta = v - mean
            mean += delta / n
            var += (delta * (v - mean) - var) / n
            continue
        std = max(np.sqrt(var), min_std)
        z[i] = (v - mean) / std
        diff = v - mean
        mean += alpha * diff
        var = (1 - alpha) * (var + alpha * diff * diff)
    return z


def study(ticks_path: str, results_path: str, window_ms: int = 500) -> dict:
    res = pd.read_csv(results_path)
    ticks = pd.read_csv(ticks_path, usecols=["timestamp_ms", "symbol", "price"])
    ticks["window_start_ms"] = ticks["timestamp_ms"] // window_ms * window_ms
    last_px = ticks.groupby(["symbol", "window_start_ms"])["price"].last()

    out = {}
    for symbol, r in res.groupby("symbol"):
        r = r.sort_values("window_start_ms").reset_index(drop=True)
        scored = r["robust_t_stat"].notna()
        robust_alert = r["alert"].eq(1)
        z = ewma_z(r["net_imbalance"].to_numpy())
        gauss_alert = pd.Series(np.abs(z) >= 3.0, index=r.index) & ~np.isnan(z)

        px = last_px.loc[symbol].reindex(r["window_start_ms"]).ffill().to_numpy()
        fwd = np.full(len(r), np.nan)
        fwd[:-HORIZON_WINDOWS] = (px[HORIZON_WINDOWS:] / px[:-HORIZON_WINDOWS] - 1) * 1e4
        traded = r["trade_count"].gt(0).to_numpy()
        sign = np.sign(r["net_imbalance"].to_numpy())

        def move(mask):
            m = mask & traded & ~np.isnan(fwd)
            if m.sum() == 0:
                return None
            return {
                "n": int(m.sum()),
                "mean_abs_bps": round(float(np.nanmean(np.abs(fwd[m]))), 2),
                "signed_bps_in_imbalance_direction": round(float(np.nanmean(fwd[m] * sign[m])), 2),
                "share_moving_with_imbalance": round(float(np.mean((fwd[m] * sign[m]) > 0)), 3),
            }

        n_scored = int(scored.sum())
        out[symbol] = {
            "windows": int(len(r)),
            "share_empty_windows": round(float((~traded).mean()), 3),
            "robust_alerts": int(robust_alert.sum()),
            "robust_alert_rate": round(robust_alert.sum() / max(n_scored, 1), 4),
            "gaussian_alerts": int(gauss_alert.sum()),
            "gaussian_alert_rate": round(float(gauss_alert.sum()) / max(int((~np.isnan(z)).sum()), 1), 4),
            "nominal_rate": NOMINAL_RATE,
            "after_robust_alert": move(robust_alert.to_numpy()),
            "after_gaussian_alert": move(gauss_alert.to_numpy()),
            "all_traded_windows": move(np.ones(len(r), dtype=bool)),
        }
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ticks")
    parser.add_argument("results")
    parser.add_argument("--label", default="")
    parser.add_argument("--json", default=None, help="Append the summary to this JSON file.")
    args = parser.parse_args()
    summary = {"label": args.label, "symbols": study(args.ticks, args.results)}
    print(json.dumps(summary, indent=2))
    if args.json:
        try:
            with open(args.json, encoding="utf-8") as f:
                existing = json.load(f)
        except FileNotFoundError:
            existing = []
        existing = [e for e in existing if e.get("label") != args.label] + [summary]
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(existing, f, indent=2)


if __name__ == "__main__":
    main()
