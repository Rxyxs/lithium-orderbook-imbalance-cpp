// lithium-orderbook-imbalance-cpp
//
// Streaming order-flow-imbalance (OFI) EWMA z-score engine for lithium
// stocks (SQM, ALB, PLS, MIN, ...). Reads a tick-by-tick CSV file one
// line at a time (constant memory regardless of file size), aggregates
// buy/sell volume into 500ms windows per symbol, and scores each
// window's imbalance against an exponentially-weighted mean/variance to
// flag statistically abnormal order flow in real time.
//
// Usage:
//   loi_engine.exe <ticks.csv> [--window-ms 500] [--alpha 0.05]
//                  [--warmup 30] [--z-alert 3.0] [--out results.csv]

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
#include "tick.hpp"

namespace {

struct Args {
    std::string input_path;
    std::string output_path = "results.csv";
    std::int64_t window_ms = 500;
    double alpha = 0.05;
    std::size_t warmup = 30;
    double z_alert = 3.0;
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
        else if (flag == "--alpha") args.alpha = std::stod(next_value());
        else if (flag == "--warmup") args.warmup = static_cast<std::size_t>(std::stoul(next_value()));
        else if (flag == "--z-alert") args.z_alert = std::stod(next_value());
        else if (flag == "--out") args.output_path = next_value();
        else throw std::runtime_error("unknown flag: " + flag);
    }
    return true;
}

void print_usage(const char* program_name) {
    std::cerr << "Uso: " << program_name << " <ticks.csv> [--window-ms 500] [--alpha 0.05]"
              << " [--warmup 30] [--z-alert 3.0] [--out results.csv]\n";
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

    std::ofstream out(args.output_path);
    if (!out.is_open()) {
        std::cerr << "No se pudo abrir el archivo de salida: " << args.output_path << "\n";
        return 1;
    }
    out << "window_start_ms,symbol,buy_volume,sell_volume,net_imbalance,imbalance_ratio,"
           "trade_count,z_score,alert\n";

    std::size_t windows_emitted = 0;
    std::size_t alerts_raised = 0;

    const auto write_result = [&](const loi::WindowResult& r) {
        const bool alert = r.z_score.has_value() && std::abs(*r.z_score) >= args.z_alert;
        if (alert) ++alerts_raised;
        ++windows_emitted;

        out << r.window_start_ms << ',' << r.symbol << ',' << r.buy_volume << ','
            << r.sell_volume << ',' << r.net_imbalance << ',' << r.imbalance_ratio << ','
            << r.trade_count << ',';
        if (r.z_score.has_value()) {
            out << *r.z_score;
        }
        out << ',' << (alert ? "1" : "0") << '\n';

        if (alert) {
            std::cout << "[ALERTA] " << r.symbol << " t=" << r.window_start_ms
                      << "ms net_imbalance=" << r.net_imbalance
                      << " ratio=" << r.imbalance_ratio << " z=" << *r.z_score << "\n";
        }
    };

    try {
        loi::CsvTickReader reader(args.input_path);
        loi::OrderFlowImbalanceEngine engine(args.window_ms, args.alpha, args.warmup);

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
                   << "Alertas (|z|>=" << args.z_alert << ")  : " << alerts_raised << "\n"
                   << "Tiempo de procesamiento: " << elapsed_ms << " ms ("
                   << (ticks_processed / std::max(elapsed_ms, 1.0) * 1000.0) << " ticks/s)\n"
                   << "Resultados escritos en: " << args.output_path << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
