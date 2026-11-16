#pragma once

#include "low_latency/processor.hpp"
#include <memory>

namespace low_latency {
struct RecoveryOptions {
    std::string grp, spin_server;
    std::size_t buffer_messages = 65536;
    std::uint32_t max_gap = 10000;
    std::uint64_t timeout_ns = 3'000'000'000;
};
struct RecoveryStats {
    std::uint64_t gaps = 0, requests = 0, rejections = 0, duplicates = 0;
    std::uint64_t replay_messages = 0, buffered_high_watermark = 0, spins = 0;
    std::uint64_t timeouts = 0, buffer_overflows = 0;
    std::uint64_t completed = 0, total_ns = 0, max_ns = 0;
};

// Single consumer owns this object and Processor. TCP I/O is nonblocking;
// multicast reception remains on the independent RX thread. Normal contiguous
// datagrams take the original whole-packet processor path without copying.
class Recovery {
public:
    Recovery(const RecoveryOptions& options, std::uint8_t unit);
    ~Recovery();
    Recovery(const Recovery&) = delete;
    Recovery& operator=(const Recovery&) = delete;
    void receive(Processor& processor, std::span<std::uint8_t> payload,
                 std::uint64_t rx_ns, bool replay);
    void tick(Processor& processor, std::uint64_t now);
    void note_loss(Processor& processor, std::uint64_t now);
    bool active() const;
    const RecoveryStats& stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace low_latency
