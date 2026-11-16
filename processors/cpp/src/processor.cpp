#include "low_latency/processor.hpp"

#include <time.h>
#include <sys/random.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace low_latency {
namespace {

class PartialBookError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::uint64_t le(const std::uint8_t* data, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) { value |= std::uint64_t{data[i]} << (8 * i); }
    return value;
}

} // namespace

OrderIdHash::OrderIdHash() {
    // getentropy supplies all requested bytes or fails; never fall back to a
    // timestamp, PID, or fixed seed. No seed is logged or read from the feed.
    if (getentropy(key_.data(), sizeof(key_)) != 0) {
        throw std::system_error(errno, std::generic_category(), "getentropy for order hash");
    }
}

std::uint64_t monotonic_ns() {
    timespec time{};
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        throw std::system_error(errno, std::generic_category(), "clock_gettime");
    }
    return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000 + static_cast<std::uint64_t>(time.tv_nsec);
}

OrderBook::OrderBook(std::size_t max_orders, OrderIdHash hash) : orders_(0, hash), max_orders_(max_orders) {
    if (max_orders == 0 || max_orders > largest_order_limit) {
        throw std::invalid_argument("Need 1..100000000 maximum live orders");
    }
    orders_.max_load_factor(1.0F);
    orders_.reserve(max_orders_); // Reserve before RX to avoid growth during processing.
}
void OrderBook::clear() { orders_.clear(); symbols_.clear(); }

OrderBook::Levels& OrderBook::levels(const Order& order) {
    auto& sides = symbols_[order.symbol];
    return order.side == 'B' ? sides.bids : sides.asks;
}

void OrderBook::add_level(const Order& order) {
    auto& level = levels(order)[order.price];
    level.quantity += order.quantity;
    ++level.orders;
}

void OrderBook::remove_level(const Order& order) {
    auto& side = levels(order);
    const auto found = side.find(order.price);
    found->second.quantity -= order.quantity;
    if (--found->second.orders == 0) { side.erase(found); }
    const auto symbol = symbols_.find(order.symbol);
    if (symbol->second.bids.empty() && symbol->second.asks.empty()) { symbols_.erase(symbol); }
}

void OrderBook::apply(std::span<const std::uint8_t> message) { (void)apply(decode_pitch_message(message)); }

BookEffect OrderBook::apply(const PitchMessage& message) {
    const auto type = message.type;
    const auto id = message.order_id;
    if (type == 0x21 || type == 0x22 || type == 0x2f) {
        Order order{message.symbol, message.side, message.price, message.quantity};
        if (orders_.contains(id)) { throw std::runtime_error("Invalid/duplicate Add Order"); }
        if (orders_.size() == max_orders_) { throw std::runtime_error("PITCH live order limit exceeded"); }
        orders_.emplace(id, order);
        add_level(order);
        return {order.symbol, std::nullopt};
    }
    if (!message.order_update()) { throw std::runtime_error("Unsupported PITCH order message"); }
    const auto found = orders_.find(id);
    if (found == orders_.end()) { throw PartialBookError("PITCH update references a missing order"); }
    auto& order = found->second;
    BookEffect effect{order.symbol, std::nullopt};
    if (type == 0x23 || type == 0x24) {
        effect.trade = LastTrade{message.execution_id, type == 0x23 ? order.price : message.price, message.quantity};
    }
    if (type == 0x27 || type == 0x28 || type == 0x24) {
        // 0x24 supplies authoritative remaining displayed size. Its execution
        // can exceed displayed quantity; do not treat it as a Reduce message.
        const auto quantity = type == 0x24 ? message.remaining_quantity : message.quantity;
        const auto price = type == 0x24 ? order.price : message.price;
        if (quantity != 0 && price == order.price) {
            auto& level = levels(order).at(order.price);
            level.quantity = level.quantity - order.quantity + quantity;
            order.quantity = quantity;
            return effect;
        }
        remove_level(order);
        if (quantity == 0) { orders_.erase(found); return effect; }
        order.quantity = quantity;
        order.price = price;
        add_level(order);
    } else if (type == 0x23 || type == 0x25 || type == 0x26) {
        const auto quantity = message.quantity;
        if (quantity > order.quantity) { throw PartialBookError("Reduced/executed quantity exceeds known order"); }
        levels(order).at(order.price).quantity -= quantity;
        order.quantity -= quantity;
        if (order.quantity == 0) {
            remove_level(order);
            orders_.erase(found);
        }
    } else if (type == 0x29) {
        remove_level(order);
        orders_.erase(found);
    }
    return effect;
}

std::pair<std::optional<Quote>, std::optional<Quote>> OrderBook::top(const Symbol& symbol) const {
    std::pair<std::optional<Quote>, std::optional<Quote>> result;
    const auto found = symbols_.find(symbol);
    if (found == symbols_.end()) { return result; }
    const auto& sides = found->second;
    if (!sides.bids.empty()) {
        const auto& [price, level] = *sides.bids.rbegin();
        result.first = Quote{price, level.quantity};
    }
    if (!sides.asks.empty()) {
        const auto& [price, level] = *sides.asks.begin();
        result.second = Quote{price, level.quantity};
    }
    return result;
}

std::vector<BookLevel> OrderBook::snapshot() const {
    std::vector<BookLevel> result;
    for (const auto& [symbol, sides] : symbols_) {
        std::string name(symbol.begin(), symbol.end());
        name.erase(name.find_last_not_of(' ') + 1);
        for (auto it = sides.bids.rbegin(); it != sides.bids.rend(); ++it) {
            result.push_back({name, 'B', it->first, it->second.quantity, it->second.orders});
        }
        for (const auto& [price, level] : sides.asks) {
            result.push_back({name, 'S', price, level.quantity, level.orders});
        }
    }
    return result;
}

Processor::Processor(std::uint8_t unit, std::uint64_t warmup, std::size_t samples,
                     LossPolicy policy, Clock clock, std::size_t gap_slots, std::size_t max_orders, std::size_t max_trades)
    : unit_(unit), warmup_(warmup), target_(samples), clock_(clock), policy_(policy), gaps_(gap_slots), book_(max_orders), l1_(max_orders, max_trades) {
    if (unit == 0 || samples == 0 || !clock) { throw std::invalid_argument("Need nonzero unit/sample target and clock"); }
    samples_.reserve(target_);
}

void Processor::emit(const L1Update& update) {
    ++stats_.l1_updates;
    if (sink_) { sink_(update); }
}
void Processor::note_loss() {
    dirty_ = true;
    loss_seen_ = true;
    l1_.note_loss();
    l1_.refresh(book_, false, 0, [this](const L1Update& update) { emit(update); });
}

void Processor::begin_recovery() {
    recovering_ = true;
    loss_seen_ = true;
    l1_.refresh(book_, false, 0, [this](const L1Update& update) { emit(update); });
}
void Processor::finish_recovery() {
    recovering_ = false;
    l1_.refresh(book_, book_current(), 0, [this](const L1Update& update) { emit(update); });
}
void Processor::begin_spin() {
    begin_recovery();
    for (const auto& [symbol, state] : l1_.symbols()) {
        auto empty = state.l1;
        empty.bid.reset(); empty.ask.reset(); empty.last.reset();
        empty.trading_status = 'S'; empty.reg_sho = '0'; empty.retail = 'N';
        empty.book_current = false; empty.trade_history_current = false; empty.changed = 63;
        emit(empty);
    }
    book_.clear();
    l1_ = L1Book(book_.max_orders(), l1_.max_trades());
    l1_.note_loss();
    time_ = {}; time_reference_ = {};
    initialized_ = false; dirty_ = false; session_ended_ = false;
}
void Processor::spin_message(std::span<const std::uint8_t> message) {
    const auto decoded = decode_pitch_message(message);
    const auto type = decoded.type;
    if (type == 0x20) { time_ = decoded; }
    else if (type == 0xb1) { time_reference_ = decoded; l1_.set_trade_date(decoded.trade_date); }
    else if (type == 0x21 || type == 0x22 || type == 0x2f || type == 0x31 || type == 0x95 || type == 0x98) {
        const auto effect = decoded.order_update() ? book_.apply(decoded) : BookEffect{};
        (void)l1_.decide(decoded, effect, book_, false);
    } else { throw std::runtime_error("Unexpected equities spin message"); }
}
void Processor::finish_spin(std::uint32_t sequence) {
    expected_ = advance_sequence(sequence, 1);
    sequenced_seen_ = true; initialized_ = true;
    for (const auto& [symbol, state] : l1_.symbols()) {
        auto event = state.l1; event.changed = 63; emit(event);
    }
}

void Processor::process(std::span<std::uint8_t> payload, std::uint64_t rx_ns, bool sample) {
    if (failed_) { throw std::runtime_error("Processor is invalid after a previous protocol error"); }
    try {
        validate_pitch(payload);
        const auto sequence = static_cast<std::uint32_t>(le(payload.data() + 4, 4));
        // Outside-session multicast heartbeats are unsequenced, sometimes unit
        // zero. They neither establish a sequence prefix nor imply a gap.
        if (sequence == 0 && payload[2] == 0 && (payload[3] == unit_ || payload[3] == 0)) {
            ++stats_.packets;
            ++stats_.unsequenced_heartbeats;
            return;
        }
        if (payload[3] != unit_) { ++stats_.other_unit_packets; return; }
        ++stats_.packets;
        if (sequence != expected_) {
            ++stats_.sequence_errors;
            const auto distance = sequence >= expected_ ? std::uint64_t{sequence} - expected_ :
                                  std::uint64_t{UINT32_MAX} - expected_ + sequence;
            const bool joining = !sequenced_seen_ && sequence != 0 && policy_ == LossPolicy::continue_processing;
            if (joining) {
                stats_.joined_midstream = true;
                note_loss(); // No previous observation: the missing prefix length is unknown.
            } else if (sequence != 0 && distance <= UINT32_MAX / 2) {
                stats_.missing_messages += distance;
                const GapRange gap{unit_, expected_, sequence == 1 ? UINT32_MAX : sequence - 1,
                                   static_cast<std::uint32_t>(distance), rx_ns};
                if (gaps_.try_push(gap)) { ++stats_.gap_ranges_enqueued; }
                else { ++stats_.gap_ranges_dropped; }
                note_loss();
            }
            else { ++stats_.out_of_order_packets; }
            if (policy_ == LossPolicy::stop || sequence == 0) {
                throw std::runtime_error("PITCH sequence mismatch: expected " + std::to_string(expected_) +
                                         ", received " + std::to_string(sequence));
            }
            // Stale/duplicate packets must never be applied twice.
            if (!joining && distance > UINT32_MAX / 2) { return; }
        }
        sequenced_seen_ = true;
        std::size_t offset = 8;
        for (unsigned i = 0; i < payload[2]; ++i) {
            const auto message = payload.subspan(offset, payload[offset]);
            const auto decoded = decode_pitch_message_zero_copy(message);
            const auto type = decoded.type;
            if (session_ended_) { throw std::runtime_error("Sequenced message after End of Session"); }
            if (type == 0x20) { time_ = decoded; }
            else if (type == 0xb1) {
                time_reference_ = decoded;
                l1_.set_trade_date(decoded.trade_date);
            }
            else if (type == 0x2d) { session_ended_ = true; }
            else if (type == 0x97) {
                book_.clear();
                initialized_ = true;
                dirty_ = false;
                l1_.refresh(book_, book_current(), type, [this](const L1Update& update) { emit(update); });
            } else if (decoded.market()) {
                if (!initialized_ && decoded.order_update()) {
                    if (policy_ == LossPolicy::stop) { throw std::runtime_error("Need initial Unit Clear before building L2"); }
                    stats_.joined_midstream = true;
                    note_loss();
                }
                bool applied = true;
                BookEffect effect;
                try { if (decoded.order_update()) { effect = book_.apply(decoded); } }
                catch (const PartialBookError&) {
                    if (policy_ != LossPolicy::continue_processing || !dirty_) { throw; }
                    applied = false;
                    ++stats_.skipped_market_messages;
                }
                std::optional<L1Update> update;
                if (applied) { update = l1_.decide(decoded, effect, book_, book_current()); }
                // Endpoint includes decoding, L2, trade history and the L1
                // emit/no-update decision. Dispatch to a consumer is afterward.
                if (sample && applied && stats_.market_messages >= warmup_ && samples_.size() < target_) {
                    const auto end = clock_();
                    if (end < rx_ns) { throw std::runtime_error("RX/end clocks are inconsistent"); }
                    samples_.push_back(end - rx_ns);
                }
                if (applied) {
                    ++stats_.applied_market_messages;
                    ++stats_.l1_decisions;
                    if (update) { emit(*update); }
                    else { ++stats_.l1_no_updates; }
                }
                ++stats_.market_messages;
                if (stats_.warmup_messages < warmup_) { ++stats_.warmup_messages; }
            }
            ++stats_.pitch_messages;
            offset += message.size();
        }
        expected_ = advance_sequence(sequence, payload[2]);
    } catch (...) {
        failed_ = true;
        ++stats_.protocol_errors;
        throw;
    }
}

LatencySummary Processor::latency() const {
    LatencySummary result;
    result.samples = samples_.size();
    if (samples_.empty()) { return result; }
    auto sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](std::size_t numerator, std::size_t denominator) {
        return sorted[(sorted.size() * numerator + denominator - 1) / denominator - 1];
    };
    result.minimum = sorted.front();
    result.p50 = percentile(50, 100);
    result.p99 = percentile(99, 100);
    result.p999 = percentile(999, 1000);
    result.maximum = sorted.back();
    long double sum = 0;
    for (const auto sample : sorted) { sum += sample; }
    result.mean = static_cast<double>(sum / sorted.size());
    return result;
}

} // namespace low_latency
