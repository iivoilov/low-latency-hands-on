#pragma once

#include <arpa/inet.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace low_latency::cli {

inline std::uint64_t integer(std::string_view name, std::string_view value, std::uint64_t maximum) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result > maximum) {
        throw std::invalid_argument(std::string(name) + ": invalid unsigned integer");
    }
    return result;
}

inline double decimal(std::string_view name, std::string_view value) {
    double result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(result)) {
        throw std::invalid_argument(std::string(name) + ": invalid decimal");
    }
    return result;
}

inline std::vector<std::string> symbols(const std::string& value) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    do {
        const auto end = value.find(',', begin);
        result.push_back(value.substr(begin, end - begin));
        if (end == std::string::npos) { return result; }
        begin = end + 1;
    } while (true);
}

inline in_addr address(const std::string& value) {
    in_addr result{};
    if (inet_pton(AF_INET, value.c_str(), &result) != 1) {
        throw std::invalid_argument("Expected an IPv4 address: " + value);
    }
    return result;
}

} // namespace low_latency::cli
