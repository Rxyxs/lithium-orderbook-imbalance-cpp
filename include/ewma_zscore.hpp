#pragma once

// Exponentially-weighted moving average / variance z-score tracker.
//
// A naive EWMA z-score divides by the variance estimate from the very
// first sample, which is either zero or based on a single noisy point.
// A sibling project (market-tick-anomaly-engine-cpp) hit this in
// production: the 2nd data point divided by a near-zero variance floor
// and produced a z-score of -184,855, permanently saturating every
// downstream alert. This tracker avoids that by running a fixed
// warm-up phase (plain running mean/variance) before switching to the
// exponential update, and by flooring the variance so a quiet warm-up
// window can never make the denominator collapse to zero.

#include <cmath>
#include <cstddef>
#include <optional>

namespace loi {

class EwmaZScore {
public:
    // alpha:            EWMA decay factor in (0, 1]; higher = more reactive.
    // warmup_samples:   number of observations used to seed mean/variance
    //                   via a plain running average before EWMA takes over.
    // min_std:          variance floor (as a std-dev) so a near-constant
    //                   warm-up window can't blow up later z-scores.
    explicit EwmaZScore(double alpha, std::size_t warmup_samples = 30, double min_std = 1e-6)
        : alpha_(alpha), warmup_samples_(warmup_samples), min_std_(min_std) {}

    // Feeds one observation. Returns the z-score of `x` against the mean
    // and variance estimated from all *prior* observations, or
    // std::nullopt while still in the warm-up phase (not enough history
    // yet to trust a variance estimate).
    std::optional<double> update(double x) {
        if (count_ < warmup_samples_) {
            // Welford's online algorithm for the warm-up running mean/variance.
            ++count_;
            const double delta = x - mean_;
            mean_ += delta / static_cast<double>(count_);
            const double delta2 = x - mean_;
            m2_ += delta * delta2;

            if (count_ == warmup_samples_) {
                variance_ = count_ > 1 ? m2_ / static_cast<double>(count_ - 1) : 0.0;
            }
            return std::nullopt;
        }

        const double std_dev = std::sqrt(variance_) < min_std_ ? min_std_ : std::sqrt(variance_);
        const double z = (x - mean_) / std_dev;

        // Exponential update of mean/variance using x, so the *next* call
        // scores against a mean/variance that includes this observation.
        const double delta = x - mean_;
        const double incr = alpha_ * delta;
        mean_ += incr;
        variance_ = (1.0 - alpha_) * (variance_ + delta * incr);

        return z;
    }

    bool is_warmed_up() const { return count_ >= warmup_samples_; }
    double mean() const { return mean_; }
    double variance() const { return variance_; }

private:
    double alpha_;
    std::size_t warmup_samples_;
    double min_std_;

    std::size_t count_ = 0;
    double mean_ = 0.0;
    double variance_ = 0.0;
    double m2_ = 0.0; // Welford accumulator, warm-up phase only.
};

} // namespace loi
