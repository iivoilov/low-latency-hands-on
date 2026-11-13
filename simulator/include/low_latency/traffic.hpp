#pragma once

#include <cstddef>
#include <cstdint>

namespace low_latency {

// Rates count PITCH UDP payload bits. Background is the normal traffic rate;
// bursts replace it with peak rate at the start of each period.
class TrafficSchedule {
public:
    TrafficSchedule(double background_mbps, double peak_mbps,
                    unsigned burst_ms, unsigned period_ms);
    std::uint64_t now_ns() const;
    void advance(std::size_t payload_bytes);
    double background_mbps() const { return static_cast<double>(background_ / 1'000'000); }
    double cycle_average_mbps() const {
        return static_cast<double>((background_ * (period_ns_ - burst_ns_) + peak_ * burst_ns_)
                                   / period_ns_ / 1'000'000);
    }

private:
    long double peak_;
    long double background_;
    long double burst_ns_;
    long double period_ns_;
    long double elapsed_ns_ = 0;
};

} // namespace low_latency
