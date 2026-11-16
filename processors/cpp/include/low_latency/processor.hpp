#pragma once

#include "low_latency/pitch.hpp"
#include "low_latency/pitch_message.hpp"

#include <optional>
#include <functional>
#include "low_latency/gap_queue.hpp"
#include "low_latency/order_id_hash.hpp"

#include <array>
#include <map>
#include <span>
#include <unordered_map>

namespace low_latency {

std::uint64_t monotonic_ns(); // clock_gettime(CLOCK_MONOTONIC), shared RX/end clock.

struct Quote {
    std::uint64_t price = 0, quantity = 0;
    bool operator==(const Quote&) const = default;
};
struct LastTrade {
    std::uint64_t execution_id = 0, price = 0;
    std::uint32_t quantity = 0;
    std::uint32_t trade_date = 0; // Zero when the feed has not supplied a date.
    bool operator==(const LastTrade&) const = default;
};
struct BookEffect {
    Symbol symbol{};
    std::optional<LastTrade> trade;
};

class OrderBook {
public:
    static constexpr std::size_t default_max_orders = 1'000'000;
    static constexpr std::size_t largest_order_limit = 100'000'000;
    explicit OrderBook(std::size_t max_orders = default_max_orders, OrderIdHash hash = OrderIdHash{});
    void clear();
    void apply(std::span<const std::uint8_t> message);
    std::pair<std::optional<Quote>, std::optional<Quote>> top(const Symbol& symbol) const;
    std::vector<BookLevel> snapshot() const; // Reporting only; outside timed updates.
    std::size_t orders() const { return orders_.size(); }
    std::size_t symbols() const { return symbols_.size(); }
    std::size_t max_orders() const { return max_orders_; }
    std::size_t order_bucket(std::uint64_t id) const { return orders_.bucket(id); } // Diagnostics only.

private:
    friend class Processor;
    BookEffect apply(const PitchMessage& message);
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

// Every event is a complete current snapshot. A missing side is explicit,
// never zero priced. Changed: bid=1, ask=2, last=4, status=8, retail=16,
// validity=32. Repeated equal-price trades still change last via execution ID.
struct L1Update {
    Symbol symbol{};
    std::optional<Quote> bid, ask;
    std::optional<LastTrade> last;
    char trading_status = 'S', reg_sho = '0', retail = 'N';
    bool book_current = false, trade_history_current = true;
    std::uint8_t changed = 0, message_type = 0;
};
struct SymbolState {
    L1Update l1;
    std::optional<PitchMessage> auction_update, auction_summary;
    bool metadata = false;
};

class L1Book {
public:
    static constexpr std::size_t default_max_trades = 1'000'000;
    L1Book(std::size_t max_symbols, std::size_t max_trades);
    std::optional<L1Update> decide(const PitchMessage& message, const BookEffect& effect,
                                   const OrderBook& book, bool current);
    // Unit Clear clears orders only, preserving last trades and symbol metadata.
    void refresh(const OrderBook& book, bool current, std::uint8_t type,
                 const std::function<void(const L1Update&)>& emit);
    void note_loss() { history_current_ = false; }
    void set_trade_date(std::uint32_t date) { trade_date_ = date; }
    const std::map<Symbol, SymbolState>& symbols() const { return symbols_; }
    std::size_t trades() const { return trades_.size(); }
    std::size_t max_trades() const { return max_trades_; }
    bool trade_history_current() const { return history_current_; }
private:
    struct ExecutionKey {
        std::uint32_t date;
        std::uint64_t id;
        bool operator==(const ExecutionKey&) const = default;
    };
    struct ExecutionHash {
        OrderIdHash ids, dates; // Independent OS-generated keys for both fields.
        std::size_t operator()(const ExecutionKey& key) const noexcept { return ids(key.id) ^ dates(key.date); }
    };
    struct TradeRecord {
        Symbol symbol;
        LastTrade trade;
        std::optional<ExecutionKey> previous, next;
    };
    SymbolState& state(const Symbol& symbol, bool current);
    std::optional<L1Update> update(SymbolState& state, const L1Update& previous,
                                  const OrderBook& book, bool current, std::uint8_t type);
    std::map<Symbol, SymbolState> symbols_;
    std::unordered_map<ExecutionKey, TradeRecord, ExecutionHash> trades_;
    std::size_t max_symbols_, max_trades_;
    bool history_current_ = true;
    std::uint32_t trade_date_ = 0;
};

struct ProcessorStats {
    bool joined_midstream = false;
    std::uint64_t packets = 0;
    std::uint64_t unsequenced_heartbeats = 0;
    std::uint64_t other_unit_packets = 0;
    std::uint64_t pitch_messages = 0;
    std::uint64_t market_messages = 0;
    std::uint64_t applied_market_messages = 0;
    std::uint64_t skipped_market_messages = 0;
    std::uint64_t warmup_messages = 0;
    std::uint64_t l1_decisions = 0, l1_updates = 0, l1_no_updates = 0;
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
              std::size_t gap_slots = 4096, std::size_t max_orders = OrderBook::default_max_orders,
              std::size_t max_trades = L1Book::default_max_trades);
    // RX timestamp is taken immediately after recvmsg returns, before validation.
    // All market messages in this datagram share that timestamp.
    // Requires exclusively owned writable storage for packed object lifetimes;
    // decoding preserves all bytes and retains no references after this call.
    void process(std::span<std::uint8_t> payload, std::uint64_t rx_ns, bool sample = true);
    bool complete() const { return samples_.size() == target_; }
    bool book_current() const { return initialized_ && !dirty_ && !failed_ && !recovering_; }
    void note_loss();
    void begin_recovery();
    void finish_recovery();
    void begin_spin();
    void spin_message(std::span<const std::uint8_t> message);
    void finish_spin(std::uint32_t sequence);
    // Callback runs synchronously after the measured decision. No I/O in the default sink.
    void set_l1_sink(std::function<void(const L1Update&)> sink) { sink_ = std::move(sink); }
    const L1Book& l1() const { return l1_; }
    const PitchMessage& time() const { return time_; }
    const PitchMessage& time_reference() const { return time_reference_; }
    bool session_ended() const { return session_ended_; }
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
    bool sequenced_seen_ = false;
    bool initialized_ = false;
    bool failed_ = false;
    bool dirty_ = false;
    bool loss_seen_ = false;
    bool recovering_ = false;
    ProcessorStats stats_;
    OrderBook book_;
    L1Book l1_;
    std::function<void(const L1Update&)> sink_;
    PitchMessage time_, time_reference_;
    bool session_ended_ = false;
    void emit(const L1Update& update);
    std::vector<std::uint64_t> samples_;
};

} // namespace low_latency
