#pragma once

#include "low_latency/pitch.hpp"

#include <atomic>
#include <stdexcept>

namespace low_latency {

struct ReceivedPacket {
    Packet packet;
    bool replay = false;
    std::uint64_t rx_ns = 0;
    std::uint64_t dropped_before = 0; // Cumulative RX losses before this packet.
};

// Exactly one producer and one consumer. A consumer keeps its slot until all
// messages have been applied; the producer never overwrites an occupied slot.
class PacketRing {
public:
    explicit PacketRing(std::size_t capacity) : mask_(capacity - 1), slots_(capacity) {
        static_assert(std::atomic<std::size_t>::is_always_lock_free);
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Ring capacity must be a power of two");
        }
    }

    ReceivedPacket* producer_slot() {
        const auto write = write_.load(std::memory_order_relaxed);
        if (write - cached_read_ == slots_.size()) {
            cached_read_ = read_.load(std::memory_order_acquire);
            if (write - cached_read_ == slots_.size()) { return nullptr; }
        }
        return &slots_[write & mask_];
    }
    std::size_t publish() {
        const auto next = write_.load(std::memory_order_relaxed) + 1;
        // Keep the existing depth observation; a cached read would overstate it.
        const auto depth = next - read_.load(std::memory_order_acquire);
        write_.store(next, std::memory_order_release);
        return depth;
    }
    // The consumer owns writable packet storage until release().
    ReceivedPacket* consumer_slot() {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == cached_write_) {
            cached_write_ = write_.load(std::memory_order_acquire);
            if (read == cached_write_) { return nullptr; }
        }
        return &slots_[read & mask_];
    }
    void release() { read_.store(read_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

private:
    std::size_t mask_;
    std::vector<ReceivedPacket> slots_;
    alignas(64) std::atomic<std::size_t> write_{0};
    std::size_t cached_read_ = 0; // Producer only; refresh when apparently full.
    alignas(64) std::atomic<std::size_t> read_{0};
    std::size_t cached_write_ = 0; // Consumer only; refresh when apparently empty.
};

} // namespace low_latency
