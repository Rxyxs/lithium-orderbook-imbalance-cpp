#pragma once

// Streaming order-flow-imbalance (OFI) engine, windowed on wall-clock
// time (default 500ms) and tracked independently per symbol so a single
// tick stream carrying several lithium tickers (SQM, ALB, PLS, ...) can
// be processed in one pass.
//
// Per window:
//   buy_volume     = sum(size) over Buy-side ticks in the window
//   sell_volume    = sum(size) over Sell-side ticks in the window
//   net_imbalance  = buy_volume - sell_volume   (raw signed volume delta,
//                    unbounded -- this is what gets EWMA z-scored)
//   imbalance_ratio= net_imbalance / (buy_volume + sell_volume), in
//                    [-1, +1]; 0 when the window has no volume. Reported
//                    for readability only, NOT fed to the z-score.
//
// The z-score tracks the raw signed volume delta rather than the
// bounded ratio deliberately: on a thin window (a handful of trades)
// the ratio trivially saturates near +-1 regardless of whether the
// imbalance is a real, sustained shift or just two random trades
// landing on the same side -- that saturation makes ordinary noise
// look as extreme as a genuine event and buries real shocks under it.
// The raw delta instead scales with participation (a shock that also
// brings more/larger trades produces a bigger delta, not just a
// pinned ratio). Note this is a *trade-based* signed-volume imbalance,
// not the book-based OFI from Cont, Kukanov & Stoikov (2014) (which is
// derived from best-bid/ask price and size deltas) -- this engine's
// CSV schema carries executed trades, not L2 book snapshots, so it
// measures realized taker-side pressure rather than resting-order
// changes. Conceptually adjacent, not a reimplementation of that paper.
//
// Windows are anchored to wall-clock time, not to tick count: if a
// symbol goes quiet, the intervening empty windows are still emitted
// (net_imbalance = 0, volume = 0) so the EWMA z-score tracker sees a
// continuous, evenly-spaced series rather than being silently skipped
// ahead.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ewma_zscore.hpp"
#include "tick.hpp"

namespace loi {

struct WindowResult {
    std::string symbol;
    std::int64_t window_start_ms;
    double buy_volume;
    double sell_volume;
    double net_imbalance;
    double imbalance_ratio;
    std::size_t trade_count;
    std::optional<double> z_score; // nullopt while the symbol is warming up
};

class OrderFlowImbalanceEngine {
public:
    explicit OrderFlowImbalanceEngine(std::int64_t window_ms = 500, double ewma_alpha = 0.05,
                                       std::size_t warmup_windows = 30)
        : window_ms_(window_ms), ewma_alpha_(ewma_alpha), warmup_windows_(warmup_windows) {}

    // Feeds one tick. May produce zero or more finalized WindowResult
    // entries: one per window boundary crossed (including synthetic
    // empty windows for gaps), for the tick's symbol only.
    std::vector<WindowResult> on_tick(const Tick& tick) {
        std::vector<WindowResult> results;
        SymbolState& state = symbol_state_[tick.symbol];

        const std::int64_t bucket = floor_div(tick.timestamp_ms, window_ms_);

        if (!state.initialized) {
            state.initialized = true;
            state.current_bucket = bucket;
        }

        while (state.current_bucket < bucket) {
            results.push_back(finalize_window(tick.symbol, state));
            ++state.current_bucket;
        }

        if (tick.side == Side::Buy) {
            state.buy_volume += tick.size;
        } else {
            state.sell_volume += tick.size;
        }
        ++state.trade_count;

        return results;
    }

    // Flushes the final in-progress window for every symbol seen so far.
    // Call once after the input stream is exhausted.
    std::vector<WindowResult> flush() {
        std::vector<WindowResult> results;
        for (auto& [symbol, state] : symbol_state_) {
            if (state.initialized && (state.trade_count > 0 || state.buy_volume > 0 ||
                                       state.sell_volume > 0)) {
                results.push_back(finalize_window(symbol, state));
            }
        }
        return results;
    }

private:
    struct SymbolState {
        bool initialized = false;
        std::int64_t current_bucket = 0;
        double buy_volume = 0.0;
        double sell_volume = 0.0;
        std::size_t trade_count = 0;
        std::optional<EwmaZScore> tracker;
    };

    WindowResult finalize_window(const std::string& symbol, SymbolState& state) {
        const double total = state.buy_volume + state.sell_volume;
        const double net_imbalance = state.buy_volume - state.sell_volume;
        const double imbalance_ratio = total > 0.0 ? net_imbalance / total : 0.0;

        if (!state.tracker) {
            state.tracker.emplace(ewma_alpha_, warmup_windows_);
        }
        const std::optional<double> z = state.tracker->update(net_imbalance);

        WindowResult result{symbol,
                             state.current_bucket * window_ms_,
                             state.buy_volume,
                             state.sell_volume,
                             net_imbalance,
                             imbalance_ratio,
                             state.trade_count,
                             z};

        state.buy_volume = 0.0;
        state.sell_volume = 0.0;
        state.trade_count = 0;

        return result;
    }

    static std::int64_t floor_div(std::int64_t a, std::int64_t b) {
        const std::int64_t q = a / b;
        const std::int64_t r = a % b;
        return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
    }

    std::int64_t window_ms_;
    double ewma_alpha_;
    std::size_t warmup_windows_;
    std::unordered_map<std::string, SymbolState> symbol_state_;
};

} // namespace loi
