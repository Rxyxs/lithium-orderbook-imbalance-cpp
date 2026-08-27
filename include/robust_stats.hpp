#pragma once

// Robust-statistics primitives: median, Median Absolute Deviation (MAD),
// and the Student-t distribution's CDF/critical-value machinery used to
// calibrate an alert threshold against heavy-tailed (fat-tailed) data
// instead of assuming Normality. See robust_zscore.hpp for how these are
// combined into a streaming tracker, and README.md ("Why MAD + Student-t,
// not a Gaussian EWMA") for the statistical justification.
//
// Zero external dependencies, consistent with the rest of this project:
// the Student-t CDF is derived from the regularized incomplete beta
// function, implemented here via the standard continued-fraction method
// (the same approach as the classic Numerical Recipes `betai`/`betacf`),
// using only <cmath>'s std::lgamma for the log-gamma terms.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace loi {

// Consistency constant: for data that truly is Gaussian, MAD * kMadToSigma
// is an unbiased estimator of the standard deviation (1 / Phi^-1(0.75)).
// This is what lets a MAD-based scale substitute directly for a
// variance-based one without changing the interpretation of "how many
// sigma away" a point is.
inline constexpr double kMadToSigma = 1.4826;

// Median of `values` (must be non-empty). Reorders `values` in place
// (nth_element) -- callers that need the original order preserved must
// pass a copy, which is exactly what RobustZScore does with its window
// snapshot. Takes a std::span (C++20) rather than a concrete container
// type: the caller can hand in a std::vector, a std::deque copied out to
// a contiguous buffer, or a plain array, without this function caring.
inline double median_inplace(std::span<double> values) {
    if (values.empty()) {
        throw std::invalid_argument("median_inplace: empty input");
    }
    const std::size_t n = values.size();
    const std::size_t mid = n / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    const double upper = values[mid];
    if (n % 2 == 1) {
        return upper;
    }
    // Even count: also need the element just below the midpoint. A second
    // nth_element on the (already partitioned) lower half is still O(n)
    // amortized and keeps this allocation-free beyond the one buffer.
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid - 1), values.begin() + static_cast<std::ptrdiff_t>(mid));
    const double lower = values[mid - 1];
    return 0.5 * (lower + upper);
}

// Median Absolute Deviation of `values` around `center`: median(|x_i -
// center|). Consumes `values` (reordered in place) the same way
// median_inplace does.
inline double mad_inplace(std::span<double> values, double center) {
    for (double& v : values) {
        v = std::fabs(v - center);
    }
    return median_inplace(values);
}

namespace detail {

// Continued-fraction evaluation of the regularized incomplete beta
// function I_x(a, b), via the modified Lentz algorithm. Standard
// numerical technique (Press et al., "Numerical Recipes", `betacf`) for
// evaluating this integral to double precision without a symbolic CDF.
inline double incomplete_beta_continued_fraction(double x, double a, double b) {
    constexpr int kMaxIterations = 200;
    constexpr double kEpsilon = 1e-14;
    constexpr double kTiny = 1e-300;

    const double qab = a + b;
    const double qap = a + 1.0;
    const double qam = a - 1.0;

    double c = 1.0;
    double d = 1.0 - qab * x / qap;
    if (std::fabs(d) < kTiny) d = kTiny;
    d = 1.0 / d;
    double h = d;

    for (int m = 1; m <= kMaxIterations; ++m) {
        const double m2 = 2.0 * m;

        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (std::fabs(d) < kTiny) d = kTiny;
        c = 1.0 + aa / c;
        if (std::fabs(c) < kTiny) c = kTiny;
        d = 1.0 / d;
        h *= d * c;

        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (std::fabs(d) < kTiny) d = kTiny;
        c = 1.0 + aa / c;
        if (std::fabs(c) < kTiny) c = kTiny;
        d = 1.0 / d;
        const double delta = d * c;
        h *= delta;

        if (std::fabs(delta - 1.0) < kEpsilon) break;
    }
    return h;
}

// Regularized incomplete beta function I_x(a, b), for x in [0, 1], a, b > 0.
inline double regularized_incomplete_beta(double x, double a, double b) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;

    const double log_beta_front =
        std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) + a * std::log(x) + b * std::log(1.0 - x);
    const double front = std::exp(log_beta_front);

    // The continued fraction converges faster on one side than the
    // other; the standard symmetry relation I_x(a,b) = 1 - I_{1-x}(b,a)
    // picks whichever side is well-conditioned for this (x, a, b).
    if (x < (a + 1.0) / (a + b + 2.0)) {
        return front * incomplete_beta_continued_fraction(x, a, b) / a;
    }
    return 1.0 - front * incomplete_beta_continued_fraction(1.0 - x, b, a) / b;
}

} // namespace detail

// CDF of the Student-t distribution with `dof` degrees of freedom (dof >
// 0; need not be an integer -- the incomplete-beta formulation is valid
// for any real dof > 0).
inline double student_t_cdf(double t, double dof) {
    if (dof <= 0.0) {
        throw std::invalid_argument("student_t_cdf: dof must be > 0");
    }
    const double x = dof / (dof + t * t);
    const double ibeta = detail::regularized_incomplete_beta(x, dof / 2.0, 0.5);
    // P(T <= t) = 1 - 0.5*I_x(dof/2, 1/2) for t >= 0, mirrored for t < 0
    // by the distribution's symmetry around zero.
    return t >= 0.0 ? 1.0 - 0.5 * ibeta : 0.5 * ibeta;
}

// Two-sided tail probability P(|T| >= |t|) for a Student-t with `dof`
// degrees of freedom.
inline double student_t_two_sided_pvalue(double t, double dof) {
    return 2.0 * (1.0 - student_t_cdf(std::fabs(t), dof));
}

// Two-sided tail probability P(|Z| >= |z|) for a standard Normal --
// used to convert a familiar "nominal Gaussian sigma" threshold (e.g.
// the conventional z-alert=3.0) into the target tail probability that
// student_t_critical_value calibrates against.
inline double gaussian_two_sided_pvalue(double z) {
    return std::erfc(std::fabs(z) / std::sqrt(2.0));
}

// The |t| such that the Student-t(dof) two-sided tail probability equals
// `alpha`, found by bisection: student_t_two_sided_pvalue(t, dof) is
// continuous and strictly decreasing in t for t >= 0, so bisection is
// exact (to tolerance) and does not require a closed-form quantile
// function or an iterative root-finder that could fail to converge.
inline double student_t_critical_value(double alpha, double dof) {
    if (alpha <= 0.0 || alpha >= 1.0) {
        throw std::invalid_argument("student_t_critical_value: alpha must be in (0, 1)");
    }

    double lo = 0.0;
    double hi = 1.0;
    // Expand hi until it overshoots the target (p-value at hi is below
    // alpha), so the bracket [lo, hi] is guaranteed to contain the root.
    while (student_t_two_sided_pvalue(hi, dof) > alpha) {
        hi *= 2.0;
        if (hi > 1e6) break; // pathological alpha/dof; avoid an infinite loop
    }

    constexpr int kMaxIterations = 200;
    for (int i = 0; i < kMaxIterations; ++i) {
        const double mid = 0.5 * (lo + hi);
        const double p = student_t_two_sided_pvalue(mid, dof);
        if (p > alpha) {
            lo = mid;
        } else {
            hi = mid;
        }
        if (hi - lo < 1e-10) break;
    }
    return 0.5 * (lo + hi);
}

} // namespace loi
