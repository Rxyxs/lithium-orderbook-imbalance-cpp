#pragma once

// Streaming, single-pass CSV tick reader.
//
// Expected header (order-independent, matched by name):
//   timestamp_ms,symbol,price,size,side
//
// Reads one line at a time from disk via a buffered std::ifstream -- the
// file is never loaded into memory as a whole, so this scales to
// multi-gigabyte tick tapes on constant memory. Malformed lines are
// skipped and counted rather than aborting the stream, since real
// exchange/vendor tick dumps routinely contain a handful of corrupt rows.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "tick.hpp"

namespace loi {

class CsvTickReader {
public:
    explicit CsvTickReader(const std::string& path)
        : file_(path), path_(path) {
        if (!file_.is_open()) {
            throw std::runtime_error("CsvTickReader: cannot open file: " + path);
        }
        parse_header();
    }

    // Returns the next valid tick, or std::nullopt at end of stream.
    std::optional<Tick> next() {
        std::string line;
        while (std::getline(file_, line)) {
            ++lines_read_;
            if (line.empty()) continue;

            Tick tick;
            if (parse_line(line, tick)) {
                return tick;
            }
            ++malformed_lines_;
        }
        return std::nullopt;
    }

    std::size_t lines_read() const { return lines_read_; }
    std::size_t malformed_lines() const { return malformed_lines_; }
    const std::string& path() const { return path_; }

private:
    void parse_header() {
        std::string header_line;
        if (!std::getline(file_, header_line)) {
            throw std::runtime_error("CsvTickReader: empty file: " + path_);
        }
        ++lines_read_;

        const std::vector<std::string> fields = split(header_line);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            column_index_[trim(fields[i])] = i;
        }

        for (const char* required : {"timestamp_ms", "symbol", "price", "size", "side"}) {
            if (column_index_.find(required) == column_index_.end()) {
                throw std::runtime_error(
                    std::string("CsvTickReader: missing required column '") + required +
                    "' in " + path_);
            }
        }
    }

    bool parse_line(const std::string& line, Tick& out) const {
        const std::vector<std::string> fields = split(line);
        const std::size_t max_index = std::max(
            {column_index_.at("timestamp_ms"), column_index_.at("symbol"),
             column_index_.at("price"), column_index_.at("size"), column_index_.at("side")});
        if (fields.size() <= max_index) return false;

        try {
            out.timestamp_ms = std::stoll(fields[column_index_.at("timestamp_ms")]);
            out.symbol = trim(fields[column_index_.at("symbol")]);
            out.price = std::stod(fields[column_index_.at("price")]);
            out.size = std::stod(fields[column_index_.at("size")]);
            out.side = parse_side(trim(fields[column_index_.at("side")]));
        } catch (const std::exception&) {
            return false;
        }

        if (out.symbol.empty() || out.price <= 0.0 || out.size <= 0.0) return false;
        return true;
    }

    static std::vector<std::string> split(const std::string& line) {
        std::vector<std::string> out;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ',')) {
            out.push_back(field);
        }
        return out;
    }

    static std::string trim(const std::string& s) {
        std::size_t start = 0;
        std::size_t end = s.size();
        while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
        return s.substr(start, end - start);
    }

    std::ifstream file_;
    std::string path_;
    std::unordered_map<std::string, std::size_t> column_index_;
    std::size_t lines_read_ = 0;
    std::size_t malformed_lines_ = 0;
};

} // namespace loi
