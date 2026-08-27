#pragma once

// Robust, heavy-tail-aware replacement for EwmaZScore (see
// ewma_zscore.hpp, kept in this codebase as the reference/legacy
// Gaussian tracker -- ewma_vs_robust_alert_comparison in
// tests/test_engine.cpp measures the difference directly). Standardizes
// each observation against the **median** and **Median Absolute
// Deviation (MAD)** of a fixed-size sliding window of recent
// observations, instead of an exponentially-weighted mean/variance.
//
// Why this fixes the false-alarm problem a Gaussian EWMA z-score has on
// order-flow data (see README.md, "Why MAD + Student-t, not a Gaussian
// EWMA", for the full statistical argument):
//
//   1. Trade sizes -- and therefore signed volume imbalance -- are
//      right-skewed / fat-tailed, not Gaussian (a handful of large
//      block trades routinely dwarf the bulk of ordinary-sized prints).
//      A mean/variance estimator is itself dragged around by those
//      large values: one big trade inflates the *denominator* that
//      later observations are scored against, and a single downstream
//      -184,855-style blowup (see ewma_zscore.hpp's own doc comment) is
//      a mean/variance failure mode, not a coincidence.
//   2. The median and MAD have a **breakdown point of 50%** -- up to
//      half the window can be arbitrarily extreme without dragging the
//      location/scale estimate off the bulk of the data -- versus 0%
//      for the mean/variance (a single unbounded outlier can move them
//      arbitrarily far). That is the textbook definition of a robust
//      estimator (Huber, "Robust Statistics", 1981) and it is exactly
//      the property a fat-tailed, block-trade-driven series needs.
//
// The estimator only supplies the standardized statistic; the decision
// of *how extreme is extreme* is calibrated separately against a
// Student-t distribution (robust_stats.hpp's student_t_critical_value),
// because MAD alone still assumes the *tails* of the standardized
// statistic behave close to Gaussian -- Student-t with a low degrees-of-
// freedom explicitly does not, which is the second half of the fix (see
// main.cpp, where the alert threshold is derived).
//
// The window is a fixed-size sliding buffer (not exponentially
// weighted): a running median has no O(1) exact online update the way a
// running mean/variance does, and re-sorting a small window (tens to a
// couple hundred elements) once per 500ms-window update is
// computationally negligible -- this is not a per-tick hot path.

#include <algorithm>
#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

#include "robust_stats.hpp"

namespace loi {

class RobustZScore {
public:
    // window_size:     max number of past observations kept for the
    //                   median/MAD estimate (a fixed lookback, not a
    //                   decay factor -- there is no analogue of EWMA's
    //                   alpha here).
    // warmup_samples:   observations required before the first score is
    //                   produced. Clamped so it never exceeds
    //                   window_size (a warm-up requirement larger than
    //                   the window itself could never be satisfied).
    // min_mad:          scale floor (post kMadToSigma scaling) so a
    //                   perfectly flat warm-up window can't collapse
    //                   the denominator to zero -- same purpose as
    //                   EwmaZScore's min_std, same underlying bug it
    //                   guards against.
    explicit RobustZScore(std::size_t window_size = 60, std::size_t warmup_samples = 30,
                           double min_mad = 1e-6)
        : window_size_(window_size),
          warmup_samples_(std::min(warmup_samples, window_size)),
          min_mad_(min_mad) {}

    // Feeds one observation. Returns the robust t-statistic of `x`
    // against the median/MAD of all *prior* observations in the window
    // (x is folded into the window only after being scored, mirroring
    // EwmaZScore's anti-lookahead discipline: a shock can't dilute the
    // very baseline used to judge it), or std::nullopt during warm-up.
    std::optional<double> update(double x) {
        if (history_.size() < warmup_samples_) {
            push(x);
            return std::nullopt;
        }

        std::vector<double> snapshot(history_.begin(), history_.end());
        const double med = median_inplace(snapshot);

        std::vector<double> deviations(history_.begin(), history_.end());
        const double raw_mad = mad_inplace(deviations, med);
        const double scale = std::max(raw_mad * kMadToSigma, min_mad_);

        const double t_stat = (x - med) / scale;

        last_median_ = med;
        last_scale_ = scale;
        push(x);

        return t_stat;
    }

    bool is_warmed_up() const { return history_.size() >= warmup_samples_; }
    double last_median() const { return last_median_; }
    double last_scale() const { return last_scale_; }
    std::size_t window_size() const { return window_size_; }

private:
    void push(double x) {
        history_.push_back(x);
        if (history_.size() > window_size_) history_.pop_front();
    }

    std::size_t window_size_;
    std::size_t warmup_samples_;
    double min_mad_;

    std::deque<double> history_;
    double last_median_ = 0.0;
    double last_scale_ = 0.0;
};

} // namespace loi
