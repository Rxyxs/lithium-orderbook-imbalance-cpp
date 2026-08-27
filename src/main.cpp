// lithium-orderbook-imbalance-cpp
//
// Streaming order-flow-imbalance (OFI) engine for lithium stocks (SQM,
// ALB, PLS, MIN, ...). Reads a tick-by-tick CSV file one line at a time
// (constant memory regardless of file size), aggregates buy/sell volume
// into 500ms windows per symbol, and scores each window's imbalance
// against a robust median/MAD estimator (RobustZScore) to flag
// statistically abnormal order flow in real time -- calibrated against
// a Student-t distribution rather than a Gaussian one, since signed
// volume imbalance is fat-tailed. See README.md, "Why MAD + Student-t,
// not a Gaussian EWMA", for the full justification.
//
// Usage:
//   loi_engine.exe <ticks.csv> [--window-ms 500] [--mad-window 60]
//                  [--warmup 30] [--alert-sigma 3.0] [--dof 4.0]
//                  [--out results.csv]
//
// --alert-sigma is a *nominal Gaussian-equivalent* rarity target (e.g.
// 3.0 means "as rare as a 3-sigma Gaussian event"), not compared to the
// robust statistic directly: it is converted at startup into the
// Student-t(--dof) critical value with the same two-sided tail
// probability, and *that* larger, heavy-tail-aware value is what each
// window's |robust_t_stat| is actually compared against.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "csv_tick_reader.hpp"
#include "order_flow_imbalance.hpp"
#include "robust_stats.hpp"
#include "tick.hpp"

namespace {

struct Args {
    std::string input_path;
    std::string output_path = "results.csv";
    std::int64_t window_ms = 500;
    std::size_t mad_window = 60;
    std::size_t warmup = 30;
    double alert_sigma = 3.0;
    double dof = 4.0;
};

bool parse_args(int argc, char** argv, Args& args) {
    if (argc < 2) return false;
    args.input_path = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        auto next_value = [&](void) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + flag);
            return argv[++i];
        };

        if (flag == "--window-ms") args.window_ms = std::stoll(next_value());
        else if (flag == "--mad-window") args.mad_window = static_cast<std::size_t>(std::stoul(next_value()));
        else if (flag == "--warmup") args.warmup = static_cast<std::size_t>(std::stoul(next_value()));
        else if (flag == "--alert-sigma") args.alert_sigma = std::stod(next_value());
        else if (flag == "--dof") args.dof = std::stod(next_value());
        else if (flag == "--out") args.output_path = next_value();
        else throw std::runtime_error("unknown flag: " + flag);
    }
    return true;
}

void print_usage(const char* program_name) {
    std::cerr << "Uso: " << program_name << " <ticks.csv> [--window-ms 500] [--mad-window 60]"
              << " [--warmup 30] [--alert-sigma 3.0] [--dof 4.0] [--out results.csv]\n";
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    try {
        if (!parse_args(argc, argv, args)) {
            print_usage(argv[0]);
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Argumento invalido: " << e.what() << "\n";
        print_usage(argv[0]);
        return 1;
    }

    // Convert the nominal Gaussian-equivalent rarity target into the
    // Student-t(dof) critical value with the same two-sided tail
    // probability -- this is the actual number each window's
    // |robust_t_stat| is compared against, and it is computed once
    // (not per window: it doesn't depend on the data, only on the
    // chosen alert-sigma/dof).
    const double target_alpha = loi::gaussian_two_sided_pvalue(args.alert_sigma);
    const double t_critical = loi::student_t_critical_value(target_alpha, args.dof);

    std::ofstream out(args.output_path);
    if (!out.is_open()) {
        std::cerr << "No se pudo abrir el archivo de salida: " << args.output_path << "\n";
        return 1;
    }
    out << "window_start_ms,symbol,buy_volume,sell_volume,net_imbalance,imbalance_ratio,"
           "trade_count,robust_t_stat,alert\n";

    std::size_t windows_emitted = 0;
    std::size_t alerts_raised = 0;

    const auto write_result = [&](const loi::WindowResult& r) {
        const bool alert = r.robust_t_stat.has_value() && std::abs(*r.robust_t_stat) >= t_critical;
        if (alert) ++alerts_raised;
        ++windows_emitted;

        out << r.window_start_ms << ',' << r.symbol << ',' << r.buy_volume << ','
            << r.sell_volume << ',' << r.net_imbalance << ',' << r.imbalance_ratio << ','
            << r.trade_count << ',';
        if (r.robust_t_stat.has_value()) {
            out << *r.robust_t_stat;
        }
        out << ',' << (alert ? "1" : "0") << '\n';

        if (alert) {
            std::cout << "[ALERTA] " << r.symbol << " t=" << r.window_start_ms
                      << "ms net_imbalance=" << r.net_imbalance
                      << " ratio=" << r.imbalance_ratio << " t_stat=" << *r.robust_t_stat << "\n";
        }
    };

    try {
        loi::CsvTickReader reader(args.input_path);
        loi::OrderFlowImbalanceEngine engine(args.window_ms, args.mad_window, args.warmup);

        const auto start_time = std::chrono::steady_clock::now();
        std::size_t ticks_processed = 0;

        while (const std::optional<loi::Tick> tick = reader.next()) {
            for (const loi::WindowResult& result : engine.on_tick(*tick)) {
                write_result(result);
            }
            ++ticks_processed;
        }
        for (const loi::WindowResult& result : engine.flush()) {
            write_result(result);
        }

        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(elapsed).count();

        std::cout << "\n--- lithium-orderbook-imbalance-cpp ---\n"
                   << "Archivo de entrada : " << args.input_path << "\n"
                   << "Ticks procesados   : " << ticks_processed << "\n"
                   << "Lineas malformadas : " << reader.malformed_lines() << "\n"
                   << "Ventanas emitidas  : " << windows_emitted << " (" << args.window_ms
                   << "ms c/u)\n"
                   << "Umbral efectivo    : alert-sigma=" << args.alert_sigma << " (Gaussiano) -> dof="
                   << args.dof << " Student-t -> |t|>=" << t_critical << "\n"
                   << "Alertas (|robust_t_stat|>=" << t_critical << ")  : " << alerts_raised << "\n"
                   << "Tiempo de procesamiento: " << elapsed_ms << " ms ("
                   << (ticks_processed / std::max(elapsed_ms, 1.0) * 1000.0) << " ticks/s)\n"
                   << "Resultados escritos en: " << args.output_path << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
