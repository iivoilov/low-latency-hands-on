#include "low_latency/traffic.hpp"

#include <cmath>
#include <stdexcept>

namespace low_latency {

TrafficSchedule::TrafficSchedule(double background_mbps, double peak_mbps,
                                 unsigned burst_ms, unsigned period_ms)
    : peak_(static_cast<long double>(peak_mbps) * 1'000'000),
      background_(static_cast<long double>(background_mbps) * 1'000'000),
      burst_ns_(static_cast<long double>(burst_ms) * 1'000'000),
      period_ns_(static_cast<long double>(period_ms) * 1'000'000) {
    if (!std::isfinite(background_mbps) || !std::isfinite(peak_mbps) ||
        background_mbps <= 0 || peak_mbps < background_mbps || peak_mbps > 1000 ||
        period_ms == 0 || burst_ms >= period_ms) {
        throw std::invalid_argument("Need 0 < background <= peak <= 1000 Mbps and 0 <= burst < period");
    }
}

std::uint64_t TrafficSchedule::now_ns() const {
    return static_cast<std::uint64_t>(std::ceil(elapsed_ns_));
}

void TrafficSchedule::advance(std::size_t payload_bytes) {
    long double bits = static_cast<long double>(payload_bytes) * 8;
    while (bits > 0) {
        const auto phase = std::fmod(elapsed_ns_, period_ns_);
        const bool burst = phase < burst_ns_;
        const auto rate = burst ? peak_ : background_;
        const auto remaining_ns = (burst ? burst_ns_ : period_ns_) - phase;
        const auto available = remaining_ns * rate / 1'000'000'000;
        if (bits < available) {
            elapsed_ns_ += bits * 1'000'000'000 / rate;
            bits = 0;
        } else {
            bits -= available;
            elapsed_ns_ += remaining_ns;
        }
        if (elapsed_ns_ >= 52'200'000'000'000.0L) {
            throw std::runtime_error("Workload schedule reaches synthetic midnight; increase rate or reduce messages");
        }
    }
}

} // namespace low_latency
