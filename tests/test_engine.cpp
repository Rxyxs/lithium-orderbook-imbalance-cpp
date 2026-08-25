// Hand-rolled assertion tests (no external test framework -- zero
// dependency policy applies to tests too). Run the compiled binary
// directly; it exits non-zero and prints the failing check on any
// assertion failure, and prints a pass count on success.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "csv_tick_reader.hpp"
#include "ewma_zscore.hpp"
#include "order_flow_imbalance.hpp"
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
    loi::OrderFlowImbalanceEngine engine(/*window_ms=*/500, /*alpha=*/0.1, /*warmup_windows=*/2);

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
    loi::OrderFlowImbalanceEngine engine(500, 0.1, 2);

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
    loi::OrderFlowImbalanceEngine engine(500, 0.1, 2);

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
    // z-score, mirroring the injected shock in the sample data generator.
    loi::OrderFlowImbalanceEngine engine(/*window_ms=*/500, /*alpha=*/0.1, /*warmup_windows=*/20);

    std::int64_t t = 0;
    bool found_alert = false;
    double max_z = 0.0;

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
            if (r.z_score.has_value() && *r.z_score > max_z) max_z = *r.z_score;
            if (r.z_score.has_value() && *r.z_score >= 3.0) found_alert = true;
        }
        for (auto& r : engine.on_tick(tiny_sell)) {
            if (r.z_score.has_value() && *r.z_score > max_z) max_z = *r.z_score;
            if (r.z_score.has_value() && *r.z_score >= 3.0) found_alert = true;
        }
        t += 500;
    }

    check(found_alert, "injected buy-side imbalance burst triggers a |z|>=3 alert");
    check(max_z > 3.0, "peak z-score during the burst exceeds the alert threshold");
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

    std::printf("OK: %d checks pasaron.\n", g_checks_run);
    return 0;
}
