// Synthetic tick-tape generator for lithium mining stocks (SQM, ALB, PLS).
//
// There is no free, no-auth source of real tick-level trade/quote data
// for individual equities the way data.binance.vision offers free
// historical crypto trades -- real US/ASX equity tick data requires a
// paid or authenticated vendor (Polygon.io, Databento, LOBSTER, IEX
// Cloud). This generator produces clearly-synthetic sample data instead,
// documented as such, so the engine has something concrete to run
// against out of the box. Swap in a real vendor export that matches the
// `timestamp_ms,symbol,price,size,side` schema to use real data.
//
// Simulates ~2 hours of continuous trading for three lithium miners with
// independent random-walk mid-prices and Poisson-ish tick arrivals, then
// injects one deliberate order-flow shock (a sustained buy-side
// imbalance burst on SQM, modeling the kind of sudden buying pressure a
// real supply-disruption or demand headline produces in lithium names)
// so the detector has a known event to recover.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

struct SymbolConfig {
    std::string ticker;
    double start_price;
    double tick_size;
    double mean_interarrival_ms;
};

constexpr std::int64_t kDurationMs = 2 * 60 * 60 * 1000; // 2 simulated hours
constexpr std::int64_t kShockStartMs = static_cast<std::int64_t>(kDurationMs * 0.60);
constexpr std::int64_t kShockDurationMs = 5000; // 5 second imbalance burst
constexpr unsigned kSeed = 42;

} // namespace

int main(int argc, char** argv) {
    const std::string out_path = argc > 1 ? argv[1] : "data/lithium_ticks_sample.csv";

    std::vector<SymbolConfig> symbols = {
        {"SQM", 52.30, 0.01, 25.0}, // Sociedad Quimica y Minera de Chile (NYSE)
        {"ALB", 118.75, 0.01, 20.0}, // Albemarle Corp (NYSE)
        {"PLS", 3.42, 0.005, 35.0}, // Pilbara Minerals (ASX, quoted here in USD-equiv cents-scale)
    };

    std::ofstream out(out_path);
    if (!out.is_open()) {
        std::cerr << "No se pudo crear el archivo de salida: " << out_path << "\n";
        return 1;
    }
    out << "timestamp_ms,symbol,price,size,side\n";

    std::mt19937 rng(kSeed);
    std::normal_distribution<double> price_step(0.0, 1.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::lognormal_distribution<double> size_dist(4.0, 0.5); // realistic right-skewed trade sizes

    struct RowEvent {
        std::int64_t ts;
        std::string symbol;
        double price;
        double size;
        char side;
    };
    std::vector<RowEvent> events;
    events.reserve(3'000'000);

    for (SymbolConfig& sym : symbols) {
        double price = sym.start_price;
        std::int64_t t = 0;
        std::exponential_distribution<double> interarrival(1.0 / sym.mean_interarrival_ms);
        // During the shock, orders also arrive ~4x faster -- an imbalance
        // burst is a participation event, not just a sign flip.
        std::exponential_distribution<double> shock_interarrival(1.0 / (sym.mean_interarrival_ms / 4.0));

        while (t < kDurationMs) {
            const bool currently_in_shock =
                (sym.ticker == "SQM") && t >= kShockStartMs && t < kShockStartMs + kShockDurationMs;
            const double step = currently_in_shock ? shock_interarrival(rng) : interarrival(rng);
            t += static_cast<std::int64_t>(std::max(1.0, step));
            if (t >= kDurationMs) break;

            const bool in_shock =
                (sym.ticker == "SQM") && t >= kShockStartMs && t < kShockStartMs + kShockDurationMs;

            // Random-walk mid-price; the shock also carries mild upward drift,
            // consistent with sustained one-sided buy pressure moving the tape.
            const double drift = in_shock ? 0.15 : 0.0;
            price += (price_step(rng) + drift) * sym.tick_size;
            if (price < sym.tick_size) price = sym.tick_size;

            const double buy_probability = in_shock ? 0.90 : 0.50;
            const char side = unit(rng) < buy_probability ? 'B' : 'S';

            // During the shock, trades are also larger -- an imbalance
            // burst is a volume event, not just a sign flip.
            const double size = in_shock ? size_dist(rng) * 3.0 : size_dist(rng);

            events.push_back({t, sym.ticker, price, size, side});
        }
    }

    std::sort(events.begin(), events.end(),
              [](const RowEvent& a, const RowEvent& b) { return a.ts < b.ts; });

    for (const RowEvent& e : events) {
        out << e.ts << ',' << e.symbol << ',' << e.price << ',' << e.size << ',' << e.side << '\n';
    }

    std::cout << "Generadas " << events.size() << " ticks sinteticos en " << out_path << "\n"
              << "Shock de orden-flujo inyectado en SQM: t=[" << kShockStartMs << ", "
              << (kShockStartMs + kShockDurationMs) << "] ms\n";
    return 0;
}
