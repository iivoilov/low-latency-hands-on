#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace low_latency {

// Inclusive message-sequence bounds. A range can cross UINT32_MAX -> 1;
// message_count is explicit because PITCH reserves sequence zero.
struct GapRange {
    std::uint8_t unit = 0;
    std::uint32_t first_sequence = 0;
    std::uint32_t last_sequence = 0;
    std::uint32_t message_count = 0;
    std::uint64_t detected_rx_ns = 0;
    bool operator==(const GapRange&) const = default;
};

// L2 reader produces ranges; an independent recovery component may consume them.
// No recovery implementation is attached in the baseline executable.
class GapQueue {
public:
    explicit GapQueue(std::size_t capacity) : mask_(capacity - 1), entries_(capacity) {
        static_assert(std::atomic<std::size_t>::is_always_lock_free);
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("Gap queue capacity must be a power of two");
        }
    }
    bool try_push(const GapRange& gap) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        if (write - read_.load(std::memory_order_acquire) == entries_.size()) { return false; }
        entries_[write & mask_] = gap;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }
    bool try_pop(GapRange& gap) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) { return false; }
        gap = entries_[read & mask_];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

private:
    std::size_t mask_;
    std::vector<GapRange> entries_;
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

} // namespace low_latency
