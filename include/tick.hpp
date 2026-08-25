#pragma once

#include <cstdint>
#include <string>

namespace loi {

enum class Side : std::int8_t { Buy = 1, Sell = -1 };

struct Tick {
    std::int64_t timestamp_ms;
    std::string symbol;
    double price;
    double size;
    Side side;
};

inline Side parse_side(const std::string& token) {
    if (token.empty()) return Side::Buy;
    const char c = token[0];
    if (c == 'B' || c == 'b' || c == '1') return Side::Buy;
    return Side::Sell;
}

} // namespace loi
