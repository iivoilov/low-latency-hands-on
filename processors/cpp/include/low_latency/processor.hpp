#pragma once

#include "low_latency/pitch.hpp"
#include "low_latency/gap_queue.hpp"
#include "low_latency/order_id_hash.hpp"

#include <array>
#include <map>
#include <span>
#include <unordered_map>

namespace low_latency {

std::uint64_t monotonic_ns(); // clock_gettime(CLOCK_MONOTONIC), shared RX/end clock.

class OrderBook {
public:
    static constexpr std::size_t default_max_orders = 1'000'000;
    static constexpr std::size_t largest_order_limit = 100'000'000;
    explicit OrderBook(std::size_t max_orders = default_max_orders, OrderIdHash hash = OrderIdHash{});
    void clear();
    void apply(std::span<const std::uint8_t> message);
    std::vector<BookLevel> snapshot() const; // Reporting only; outside timed updates.
    std::size_t orders() const { return orders_.size(); }
    std::size_t symbols() const { return symbols_.size(); }
    std::size_t max_orders() const { return max_orders_; }
    std::size_t order_bucket(std::uint64_t id) const { return orders_.bucket(id); } // Diagnostics only.

private:
    using Symbol = std::array<char, 6>;
    struct Order { Symbol symbol; char side; std::uint64_t price; std::uint32_t quantity; };
    struct Level { std::uint64_t quantity = 0; unsigned orders = 0; };
    using Levels = std::map<std::uint64_t, Level>;
    struct Sides { Levels bids; Levels asks; };
    Levels& levels(const Order& order);
    void add_level(const Order& order);
    void remove_level(const Order& order);
    std::unordered_map<std::uint64_t, Order, OrderIdHash> orders_;
    std::map<Symbol, Sides> symbols_;
    std::size_t max_orders_;
};

struct ProcessorStats {
    bool joined_midstream = false;
    std::uint64_t packets = 0;
    std::uint64_t other_unit_packets = 0;
    std::uint64_t pitch_messages = 0;
    std::uint64_t market_messages = 0;
    std::uint64_t applied_market_messages = 0;
    std::uint64_t skipped_market_messages = 0;
    std::uint64_t warmup_messages = 0;
    std::uint64_t sequence_errors = 0;
    std::uint64_t missing_messages = 0;
    std::uint64_t out_of_order_packets = 0;
    std::uint64_t protocol_errors = 0;
    std::uint64_t gap_ranges_enqueued = 0;
    std::uint64_t gap_ranges_dropped = 0;
};

struct LatencySummary {
    std::size_t samples = 0;
    std::uint64_t minimum = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t p999 = 0;
    std::uint64_t maximum = 0;
    double mean = 0;
};

enum class LossPolicy { stop, continue_processing };

class Processor {
public:
    using Clock = std::uint64_t (*)();
    Processor(std::uint8_t unit, std::uint64_t warmup, std::size_t samples,
              LossPolicy policy = LossPolicy::stop, Clock clock = monotonic_ns,
              std::size_t gap_slots = 4096, std::size_t max_orders = OrderBook::default_max_orders);
    // RX timestamp is taken immediately after recvmsg returns, before validation.
    // All market messages in this datagram share that timestamp.
    void process(std::span<const std::uint8_t> payload, std::uint64_t rx_ns);
    bool complete() const { return samples_.size() == target_; }
    bool book_current() const { return initialized_ && !dirty_ && !failed_; }
    void note_loss() { dirty_ = true; loss_seen_ = true; }
    bool loss_seen() const { return loss_seen_; }
    const ProcessorStats& stats() const { return stats_; }
    const OrderBook& book() const { return book_; }
    LatencySummary latency() const;
    // Single recovery consumer only. Other state/stats remain reader-owned.
    bool pop_gap(GapRange& gap) noexcept { return gaps_.try_pop(gap); }

private:
    std::uint8_t unit_;
    std::uint64_t warmup_;
    std::size_t target_;
    Clock clock_;
    LossPolicy policy_;
    GapQueue gaps_;
    std::uint32_t expected_ = 1;
    bool initialized_ = false;
    bool failed_ = false;
    bool dirty_ = false;
    bool loss_seen_ = false;
    ProcessorStats stats_;
    OrderBook book_;
    std::vector<std::uint64_t> samples_;
};

} // namespace low_latency
