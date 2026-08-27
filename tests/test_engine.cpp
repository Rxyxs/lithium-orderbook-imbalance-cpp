// Hand-rolled assertion tests (no external test framework -- zero
// dependency policy applies to tests too). Run the compiled binary
// directly; it exits non-zero and prints the failing check on any
// assertion failure, and prints a pass count on success.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "csv_tick_reader.hpp"
#include "ewma_zscore.hpp"
#include "order_flow_imbalance.hpp"
#include "robust_stats.hpp"
#include "robust_zscore.hpp"
#include "tick.hpp"

namespace {

int g_checks_run = 0;

void check(bool condition, const char* description) {
    ++g_checks_run;
    if (!condition) {
        std::fprintf(stderr, "FALLO: %s\n", description);
        std::exit(1);
    }
}

void check_near(double actual, double expected, double tol, const char* description) {
    ++g_checks_run;
    if (std::fabs(actual - expected) > tol) {
        std::fprintf(stderr, "FALLO: %s (actual=%.6f esperado=%.6f tol=%.6f)\n", description,
                     actual, expected, tol);
        std::exit(1);
    }
}

void test_side_parsing() {
    check(loi::parse_side("B") == loi::Side::Buy, "parse_side('B') == Buy");
    check(loi::parse_side("b") == loi::Side::Buy, "parse_side('b') == Buy");
    check(loi::parse_side("1") == loi::Side::Buy, "parse_side('1') == Buy");
    check(loi::parse_side("S") == loi::Side::Sell, "parse_side('S') == Sell");
    check(loi::parse_side("s") == loi::Side::Sell, "parse_side('s') == Sell");
    check(loi::parse_side("-1") == loi::Side::Sell, "parse_side('-1') == Sell");
}

void test_csv_reader_basic_parsing() {
    const std::string path = "test_tmp_basic.csv";
    {
        std::ofstream f(path);
        f << "timestamp_ms,symbol,price,size,side\n";
        f << "1000,SQM,52.30,10.5,B\n";
        f << "1010,SQM,52.31,5.0,S\n";
    }

    loi::CsvTickReader reader(path);
    auto t1 = reader.next();
    check(t1.has_value(), "first tick parses");
    check(t1->timestamp_ms == 1000, "tick1 timestamp_ms == 1000");
    check(t1->symbol == "SQM", "tick1 symbol == SQM");
    check_near(t1->price, 52.30, 1e-9, "tick1 price == 52.30");
    check(t1->side == loi::Side::Buy, "tick1 side == Buy");

    auto t2 = reader.next();
    check(t2.has_value(), "second tick parses");
    check(t2->side == loi::Side::Sell, "tick2 side == Sell");

    auto t3 = reader.next();
    check(!t3.has_value(), "stream exhausted after 2 ticks");
    check(reader.malformed_lines() == 0, "no malformed lines in clean file");

    std::remove(path.c_str());
}

void test_csv_reader_skips_malformed_lines() {
    const std::string path = "test_tmp_malformed.csv";
    {
        std::ofstream f(path);
        f << "timestamp_ms,symbol,price,size,side\n";
        f << "1000,SQM,52.30,10.5,B\n";
        f << "not,a,valid,row\n";
        f << "1020,ALB,,5.0,S\n"; // missing price
        f << "1030,ALB,118.5,5.0,S\n";
    }

    loi::CsvTickReader reader(path);
    int valid_count = 0;
    while (reader.next()) ++valid_count;

    check(valid_count == 2, "2 valid ticks survive 2 malformed lines");
    check(reader.malformed_lines() == 2, "malformed_lines() counts both bad rows");

    std::remove(path.c_str());
}

void test_ewma_zscore_warmup_then_scores() {
    loi::EwmaZScore tracker(/*alpha=*/0.1, /*warmup_samples=*/5, /*min_std=*/1e-6);

    for (int i = 0; i < 5; ++i) {
        auto z = tracker.update(0.0);
        check(!z.has_value(), "no z-score during warm-up");
    }
    check(tracker.is_warmed_up(), "tracker reports warmed up after 5 samples");

    // Constant series (all zeros) => variance stays at floor, next zero
    // observation should score as z == 0 (no deviation from a mean of 0).
    auto z_flat = tracker.update(0.0);
    check(z_flat.has_value(), "z-score returned post warm-up");
    check_near(*z_flat, 0.0, 1e-6, "z-score of 0 against constant-0 history is 0");

    // A sharp deviation should register as a large positive z-score.
    auto z_spike = tracker.update(10.0);
    check(z_spike.has_value(), "z-score returned for spike");
    check(*z_spike > 5.0, "large deviation produces a large z-score");
}

void test_ewma_zscore_never_blows_up_on_early_variance() {
    // Regression test for the exact bug hit in the sibling C++ repo:
    // near-zero warm-up variance must not send later z-scores to
    // absurd magnitudes (it previously reached -184,855 there).
    loi::EwmaZScore tracker(0.05, 3, 1e-6);
    tracker.update(0.5);
    tracker.update(0.5);
    auto z = tracker.update(0.5); // warm-up completes on a perfectly flat series
    check(!z.has_value(), "still warming up on the 3rd sample");

    auto z2 = tracker.update(0.51); // tiny move after a flat warm-up
    check(z2.has_value(), "z-score available after warm-up");
    check(std::fabs(*z2) < 100000.0, "small deviation does not explode the z-score");
}

void test_order_flow_engine_basic_window() {
    loi::OrderFlowImbalanceEngine engine(/*window_ms=*/500, /*mad_window=*/10, /*warmup_windows=*/2);

    loi::Tick t1{100, "SQM", 52.0, 10.0, loi::Side::Buy};
    loi::Tick t2{200, "SQM", 52.0, 5.0, loi::Side::Sell};
    loi::Tick t3{600, "SQM", 52.0, 3.0, loi::Side::Buy}; // crosses into next 500ms window

    auto r1 = engine.on_tick(t1);
    check(r1.empty(), "no window finalized on first tick");
    auto r2 = engine.on_tick(t2);
    check(r2.empty(), "no window finalized on second tick, same window");
    auto r3 = engine.on_tick(t3);
    check(r3.size() == 1, "window [0,500) finalized when a tick lands in [500,1000)");

    const loi::WindowResult& w = r3[0];
    check(w.symbol == "SQM", "finalized window symbol == SQM");
    check(w.window_start_ms == 0, "finalized window starts at t=0");
    check_near(w.buy_volume, 10.0, 1e-9, "buy_volume == 10.0");
    check_near(w.sell_volume, 5.0, 1e-9, "sell_volume == 5.0");
    check_near(w.net_imbalance, 5.0, 1e-9, "net_imbalance == 10-5");
    check_near(w.imbalance_ratio, 5.0 / 15.0, 1e-9, "imbalance_ratio == (10-5)/(10+5)");
    check(w.trade_count == 2, "trade_count == 2");
}

void test_order_flow_engine_fills_empty_gap_windows() {
    loi::OrderFlowImbalanceEngine engine(500, 10, 2);

    loi::Tick t1{0, "ALB", 100.0, 1.0, loi::Side::Buy};
    loi::Tick t2{1700, "ALB", 100.0, 1.0, loi::Side::Buy}; // 3 windows later (0,500,1000,1500->1700)

    engine.on_tick(t1);
    auto results = engine.on_tick(t2);

    // Windows [0,500) had the trade; [500,1000) and [1000,1500) are empty
    // gap windows that must still be emitted with zero volume/ofi.
    check(results.size() == 3, "3 windows finalized crossing a 2-window gap");
    check_near(results[0].buy_volume, 1.0, 1e-9, "first window keeps the real trade");
    check_near(results[1].buy_volume, 0.0, 1e-9, "gap window has zero buy_volume");
    check_near(results[1].imbalance_ratio, 0.0, 1e-9, "gap window has zero ratio, not NaN");
    check_near(results[2].buy_volume, 0.0, 1e-9, "second gap window has zero buy_volume");
}

void test_order_flow_engine_tracks_symbols_independently() {
    loi::OrderFlowImbalanceEngine engine(500, 10, 2);

    loi::Tick sqm{100, "SQM", 52.0, 10.0, loi::Side::Buy};
    loi::Tick alb{100, "ALB", 118.0, 2.0, loi::Side::Sell};
    loi::Tick sqm_next{600, "SQM", 52.0, 1.0, loi::Side::Buy};

    engine.on_tick(sqm);
    engine.on_tick(alb);
    auto results = engine.on_tick(sqm_next);

    check(results.size() == 1, "only SQM's window finalizes; ALB's window is untouched");
    check(results[0].symbol == "SQM", "finalized window belongs to SQM only");
}

void test_order_flow_engine_detects_injected_imbalance() {
    // End-to-end sanity check: a sustained one-sided burst after a quiet
    // balanced warm-up period must produce a strongly positive alerting
    // statistic, mirroring the injected shock in the sample data
    // generator. Robustness to noise must not come at the cost of
    // blindness to a genuine, sustained event.
    loi::OrderFlowImbalanceEngine engine(/*window_ms=*/500, /*mad_window=*/60, /*warmup_windows=*/20);

    std::int64_t t = 0;
    bool found_alert = false;
    double max_t_stat = 0.0;

    // 20 warm-up windows of balanced flow.
    for (int w = 0; w < 20; ++w) {
        loi::Tick buy{t, "SQM", 52.0, 5.0, loi::Side::Buy};
        loi::Tick sell{t + 100, "SQM", 52.0, 5.0, loi::Side::Sell};
        for (auto& r : engine.on_tick(buy)) (void)r;
        for (auto& r : engine.on_tick(sell)) (void)r;
        t += 500;
    }

    // 10 windows of heavy one-sided buy imbalance.
    for (int w = 0; w < 10; ++w) {
        loi::Tick buy{t, "SQM", 52.0, 50.0, loi::Side::Buy};
        loi::Tick tiny_sell{t + 100, "SQM", 52.0, 1.0, loi::Side::Sell};
        for (auto& r : engine.on_tick(buy)) {
            if (r.robust_t_stat.has_value() && *r.robust_t_stat > max_t_stat) max_t_stat = *r.robust_t_stat;
            if (r.robust_t_stat.has_value() && *r.robust_t_stat >= 3.0) found_alert = true;
        }
        for (auto& r : engine.on_tick(tiny_sell)) {
            if (r.robust_t_stat.has_value() && *r.robust_t_stat > max_t_stat) max_t_stat = *r.robust_t_stat;
            if (r.robust_t_stat.has_value() && *r.robust_t_stat >= 3.0) found_alert = true;
        }
        t += 500;
    }

    check(found_alert, "injected buy-side imbalance burst triggers a |robust_t_stat|>=3 alert");
    check(max_t_stat > 3.0, "peak robust statistic during the burst exceeds the alert threshold");
}

// --- robust_stats.hpp: median / MAD / Student-t machinery -----------------

void test_median_inplace_odd_and_even_counts() {
    std::vector<double> odd = {5.0, 1.0, 3.0};
    check_near(loi::median_inplace(odd), 3.0, 1e-9, "median of {5,1,3} == 3");

    std::vector<double> even = {1.0, 2.0, 3.0, 4.0};
    check_near(loi::median_inplace(even), 2.5, 1e-9, "median of {1,2,3,4} == 2.5");
}

void test_mad_inplace_hand_computed() {
    // median = 2; deviations = {1,1,0,0,2,4,7}; median(deviations) = 1.
    std::vector<double> for_median = {1, 1, 2, 2, 4, 6, 9};
    const double med = loi::median_inplace(for_median);
    check_near(med, 2.0, 1e-9, "median of {1,1,2,2,4,6,9} == 2");

    std::vector<double> for_mad = {1, 1, 2, 2, 4, 6, 9};
    check_near(loi::mad_inplace(for_mad, med), 1.0, 1e-9, "MAD of {1,1,2,2,4,6,9} around median 2 == 1");
}

void test_median_mad_resist_a_single_extreme_outlier() {
    // The defining property of a robust estimator (breakdown point): one
    // absurd value must not drag the location/scale estimate away from
    // the bulk of otherwise-tame data, the way a mean/variance would.
    std::vector<double> with_outlier = {1, 2, 2, 3, 3, 3, 4, 4, 5, 1'000'000.0};
    const double med = loi::median_inplace(with_outlier);
    check(med < 10.0, "median stays near the bulk of the data despite one extreme outlier");

    double sum = 1 + 2 + 2 + 3 + 3 + 3 + 4 + 4 + 5 + 1'000'000.0;
    check(sum / 10.0 > 100000.0, "sanity: the MEAN of the same data is dragged near the outlier");
}

void test_student_t_critical_value_matches_known_table_values() {
    // Standard two-tailed Student-t table values (any statistics
    // reference, e.g. df=4/10/30 at alpha=0.05).
    check_near(loi::student_t_critical_value(0.05, 4.0), 2.776, 0.01, "t(4), alpha=0.05 == 2.776");
    check_near(loi::student_t_critical_value(0.05, 10.0), 2.228, 0.01, "t(10), alpha=0.05 == 2.228");
    check_near(loi::student_t_critical_value(0.05, 30.0), 2.042, 0.01, "t(30), alpha=0.05 == 2.042");
}

void test_student_t_converges_to_gaussian_as_dof_grows() {
    // As degrees of freedom -> infinity, the Student-t distribution
    // converges to the standard Normal -- the critical value at a huge
    // dof should match the familiar Gaussian value (1.95996 for
    // alpha=0.05, 3.0 for alpha≈0.0027).
    check_near(loi::student_t_critical_value(0.05, 1'000'000.0), 1.95996, 0.001,
               "t(dof->inf), alpha=0.05 converges to the Gaussian 1.95996");
    check_near(loi::student_t_critical_value(loi::gaussian_two_sided_pvalue(3.0), 1'000'000.0), 3.0, 0.001,
               "t(dof->inf) at the Gaussian z=3.0 tail probability converges back to 3.0");
}

void test_student_t_threshold_is_always_at_least_as_strict_as_gaussian() {
    // The whole point of the fix: for any finite dof, matching the same
    // nominal tail probability against a heavier-tailed reference
    // distribution requires a LARGER magnitude before something counts
    // as "equally rare" -- this is what mechanically reduces the
    // false-alarm rate relative to a raw Gaussian threshold.
    const double alpha = loi::gaussian_two_sided_pvalue(3.0); // ~0.27%
    for (double dof : {2.0, 3.0, 4.0, 5.0, 10.0, 30.0}) {
        const double t_crit = loi::student_t_critical_value(alpha, dof);
        check(t_crit >= 3.0, "Student-t critical value is never below the Gaussian one at the same alpha");
    }
    // And it should be *substantially* larger at a low, finance-typical
    // dof (this project defaults to dof=4) -- not just marginally.
    check(loi::student_t_critical_value(alpha, 4.0) > 6.0,
          "at dof=4, the calibrated threshold is well above the naive Gaussian 3.0");
}

// --- robust_zscore.hpp: RobustZScore ---------------------------------------

void test_robust_zscore_warmup_then_scores() {
    loi::RobustZScore tracker(/*window_size=*/20, /*warmup_samples=*/5, /*min_mad=*/1e-6);

    for (int i = 0; i < 5; ++i) {
        auto t_stat = tracker.update(0.0);
        check(!t_stat.has_value(), "no statistic during warm-up");
    }
    check(tracker.is_warmed_up(), "tracker reports warmed up after 5 samples");

    auto t_flat = tracker.update(0.0);
    check(t_flat.has_value(), "statistic returned post warm-up");
    check_near(*t_flat, 0.0, 1e-6, "statistic of 0 against constant-0 history is 0");

    auto t_spike = tracker.update(10.0);
    check(t_spike.has_value(), "statistic returned for spike");
    check(*t_spike > 5.0, "large deviation produces a large robust statistic");
}

void test_robust_zscore_warmup_clamped_to_window_size() {
    // A warm-up requirement larger than the window itself could never be
    // satisfied (the window would have evicted the earliest samples
    // before warm-up finished) -- the constructor must clamp it, not
    // silently hang.
    loi::RobustZScore tracker(/*window_size=*/5, /*warmup_samples=*/50, /*min_mad=*/1e-6);
    for (int i = 0; i < 5; ++i) tracker.update(1.0);
    check(tracker.is_warmed_up(), "warmup_samples > window_size is clamped down to window_size");
}

void test_robust_zscore_does_not_leak_the_current_observation() {
    // Same anti-lookahead discipline as EwmaZScore: x must be scored
    // against the median/MAD of PRIOR observations only. A huge spike
    // must not fold itself into its own denominator and understate its
    // own statistic.
    loi::RobustZScore tracker(/*window_size=*/30, /*warmup_samples=*/10, /*min_mad=*/1e-6);
    for (int i = 0; i < 10; ++i) tracker.update(0.0);

    auto t_spike = tracker.update(1000.0);
    check(t_spike.has_value(), "statistic returned for the spike itself");
    check(*t_spike > 100.0, "the spike is scored against history that does not yet include itself");
}

void test_robust_zscore_resists_a_single_outlier_that_would_blow_up_ewma() {
    // Direct comparison on the exact failure pattern documented in
    // ewma_zscore.hpp's own header comment: a near-constant warm-up
    // window followed by ordinary variation must not blow up.
    loi::RobustZScore robust(30, 3, 1e-6);
    robust.update(0.5);
    robust.update(0.5);
    auto t = robust.update(0.5); // warm-up completes on a flat series
    check(!t.has_value(), "still warming up on the 3rd sample");

    auto t2 = robust.update(0.51); // tiny move after a flat warm-up
    check(t2.has_value(), "statistic available after warm-up");
    check(std::fabs(*t2) < 100000.0, "small deviation does not explode the robust statistic either");
}

// --- The actual point of this work: fewer false alarms on fat-tailed data -

namespace comparison {

// Simulates `n_windows` of order-flow-imbalance noise with NO injected
// directional shock -- just realistic, right-skewed (log-normal) trade
// sizes randomly split buy/sell, the same kind of "high volatility but
// not actually a signal" activity documented in README.md as the source
// of the Gaussian EWMA tracker's excess false-alarm rate. Returns the
// sequence of net_imbalance values so both trackers can be compared on
// *exactly* the same series.
std::vector<double> simulate_noisy_net_imbalance(std::size_t n_windows, unsigned seed) {
    std::mt19937 rng(seed);
    std::lognormal_distribution<double> size_dist(4.0, 0.8); // heavier tail than the sample generator's 0.5
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::poisson_distribution<int> trades_per_window(8);

    std::vector<double> net_imbalances;
    net_imbalances.reserve(n_windows);

    for (std::size_t w = 0; w < n_windows; ++w) {
        const int n_trades = std::max(1, trades_per_window(rng));
        double buy = 0.0, sell = 0.0;
        for (int i = 0; i < n_trades; ++i) {
            const double size = size_dist(rng);
            if (unit(rng) < 0.5) buy += size;
            else sell += size;
        }
        net_imbalances.push_back(buy - sell);
    }
    return net_imbalances;
}

} // namespace comparison

void test_robust_estimator_raises_fewer_false_alarms_than_gaussian_ewma() {
    const std::vector<double> series = comparison::simulate_noisy_net_imbalance(/*n_windows=*/3000, /*seed=*/7);

    // Both calibrated to the same nominal "as rare as a Gaussian 3-sigma
    // event" target -- the Gaussian tracker compares directly against
    // that z, the robust tracker against the equivalent Student-t(dof=4)
    // critical value (see main.cpp for the same calibration used live).
    const double alert_sigma = 3.0;
    const double alpha = loi::gaussian_two_sided_pvalue(alert_sigma);
    const double t_critical = loi::student_t_critical_value(alpha, /*dof=*/4.0);

    loi::EwmaZScore ewma(/*alpha=*/0.05, /*warmup_samples=*/30, /*min_std=*/1e-6);
    loi::RobustZScore robust(/*window_size=*/60, /*warmup_samples=*/30, /*min_mad=*/1e-6);

    std::size_t ewma_alerts = 0;
    std::size_t robust_alerts = 0;

    for (double x : series) {
        auto z = ewma.update(x);
        if (z.has_value() && std::fabs(*z) >= alert_sigma) ++ewma_alerts;

        auto t_stat = robust.update(x);
        if (t_stat.has_value() && std::fabs(*t_stat) >= t_critical) ++robust_alerts;
    }

    std::fprintf(stderr,
                 "[info] alertas EWMA gaussiana=%zu, alertas robustas MAD+Student-t=%zu (sobre %zu ventanas, sin shock inyectado)\n",
                 ewma_alerts, robust_alerts, series.size());

    check(ewma_alerts > 0, "sanity: the fat-tailed noise series does trip the naive Gaussian threshold at least once");
    check(robust_alerts < ewma_alerts,
          "the MAD + Student-t estimator raises strictly fewer false alerts than the Gaussian EWMA on the same fat-tailed, shock-free series");
}

} // namespace

int main() {
    test_side_parsing();
    test_csv_reader_basic_parsing();
    test_csv_reader_skips_malformed_lines();
    test_ewma_zscore_warmup_then_scores();
    test_ewma_zscore_never_blows_up_on_early_variance();
    test_order_flow_engine_basic_window();
    test_order_flow_engine_fills_empty_gap_windows();
    test_order_flow_engine_tracks_symbols_independently();
    test_order_flow_engine_detects_injected_imbalance();
    test_median_inplace_odd_and_even_counts();
    test_mad_inplace_hand_computed();
    test_median_mad_resist_a_single_extreme_outlier();
    test_student_t_critical_value_matches_known_table_values();
    test_student_t_converges_to_gaussian_as_dof_grows();
    test_student_t_threshold_is_always_at_least_as_strict_as_gaussian();
    test_robust_zscore_warmup_then_scores();
    test_robust_zscore_warmup_clamped_to_window_size();
    test_robust_zscore_does_not_leak_the_current_observation();
    test_robust_zscore_resists_a_single_outlier_that_would_blow_up_ewma();
    test_robust_estimator_raises_fewer_false_alarms_than_gaussian_ewma();

    std::printf("OK: %d checks pasaron.\n", g_checks_run);
    return 0;
}
