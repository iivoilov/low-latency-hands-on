#include "low_latency/processor.hpp"

#include <time.h>
#include <sys/random.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
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

void require_size(std::span<const std::uint8_t> message, std::size_t minimum) {
    if (message.size() < minimum) { throw std::runtime_error("Truncated PITCH message fields"); }
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

void OrderBook::apply(std::span<const std::uint8_t> message) {
    require_size(message, 2);
    const auto type = message[1];
    const std::size_t minimum = type == 0x21 ? 34 : type == 0x23 ? 26 : type == 0x25 ? 18 :
                                type == 0x27 ? 27 : type == 0x29 ? 14 : 0;
    if (minimum == 0) { throw std::runtime_error("Unsupported PITCH market message"); }
    require_size(message, minimum); // Validate even when the referenced order is absent.
    if ((type == 0x23 || type == 0x25) && le(message.data() + 14, 4) == 0) {
        throw std::runtime_error("Invalid zero reduced/executed quantity");
    }
    const auto id = le(message.data() + 6, 8);
    if (type == 0x21) {
        Order order{};
        std::memcpy(order.symbol.data(), message.data() + 19, 6);
        order.side = static_cast<char>(message[14]);
        order.quantity = static_cast<std::uint32_t>(le(message.data() + 15, 4));
        order.price = le(message.data() + 25, 8);
        if ((order.side != 'B' && order.side != 'S') || order.quantity == 0 || orders_.contains(id)) {
            throw std::runtime_error("Invalid/duplicate Add Order");
        }
        bool visible_symbol = false;
        for (const auto ch : order.symbol) {
            if (ch < ' ' || ch > '~') {
                throw std::runtime_error("Invalid PITCH symbol");
            }
            if (ch != ' ') { visible_symbol = true; }
        }
        if (!visible_symbol) { throw std::runtime_error("Empty PITCH symbol"); }
        if (orders_.size() == max_orders_) { throw std::runtime_error("PITCH live order limit exceeded"); }
        orders_.emplace(id, order);
        add_level(order);
        return;
    }
    const auto found = orders_.find(id);
    if (found == orders_.end()) { throw PartialBookError("PITCH update references a missing order"); }
    auto& order = found->second;
    if (type == 0x27) {
        const auto quantity = static_cast<std::uint32_t>(le(message.data() + 14, 4));
        remove_level(order);
        if (quantity == 0) { orders_.erase(found); return; }
        order.quantity = quantity;
        order.price = le(message.data() + 18, 8);
        add_level(order);
    } else if (type == 0x23 || type == 0x25) {
        const auto quantity = static_cast<std::uint32_t>(le(message.data() + 14, 4));
        if (quantity > order.quantity) { throw PartialBookError("Reduced/executed quantity exceeds known order"); }
        levels(order).at(order.price).quantity -= quantity;
        order.quantity -= quantity;
        if (order.quantity == 0) {
            remove_level(order); // Quantity is already zero; remove the order count.
            orders_.erase(found);
        }
    } else if (type == 0x29) {
        remove_level(order);
        orders_.erase(found);
    } else {
        throw std::runtime_error("Unsupported PITCH market message");
    }
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
                     LossPolicy policy, Clock clock, std::size_t gap_slots, std::size_t max_orders)
    : unit_(unit), warmup_(warmup), target_(samples), clock_(clock), policy_(policy), gaps_(gap_slots), book_(max_orders) {
    if (unit == 0 || samples == 0 || !clock) { throw std::invalid_argument("Need nonzero unit/sample target and clock"); }
    samples_.reserve(target_);
}

void Processor::process(std::span<const std::uint8_t> payload, std::uint64_t rx_ns) {
    if (failed_) { throw std::runtime_error("Processor is invalid after a previous protocol error"); }
    try {
        validate_pitch(payload);
        if (payload[3] != unit_) { ++stats_.other_unit_packets; return; }
        ++stats_.packets;
        const auto sequence = static_cast<std::uint32_t>(le(payload.data() + 4, 4));
        if (sequence != expected_) {
            ++stats_.sequence_errors;
            const auto distance = sequence >= expected_ ? std::uint64_t{sequence} - expected_ :
                                  std::uint64_t{UINT32_MAX} - expected_ + sequence;
            const bool joining = stats_.packets == 1 && sequence != 0 && policy_ == LossPolicy::continue_processing;
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
        std::size_t offset = 8;
        for (unsigned i = 0; i < payload[2]; ++i) {
            const auto message = payload.subspan(offset, payload[offset]);
            const auto type = message[1];
            if (type == 0x20) {
                require_size(message, 6);
            } else if (type == 0x97) {
                require_size(message, 6);
                book_.clear();
                initialized_ = true;
                dirty_ = false;
            } else if (type == 0x21 || type == 0x23 || type == 0x25 || type == 0x27 || type == 0x29) {
                if (!initialized_) {
                    if (policy_ == LossPolicy::stop) { throw std::runtime_error("Need initial Unit Clear before building L2"); }
                    stats_.joined_midstream = true;
                    note_loss(); // Joining mid-session produces only a partial book.
                }
                bool applied = true;
                try { book_.apply(message); }
                catch (const PartialBookError&) {
                    if (policy_ != LossPolicy::continue_processing || !dirty_) { throw; }
                    applied = false;
                    ++stats_.skipped_market_messages;
                }
                // Endpoint is directly after the internal L2 update, before counters/sampling.
                if (applied && stats_.market_messages >= warmup_ && samples_.size() < target_) {
                    const auto end = clock_();
                    if (end < rx_ns) { throw std::runtime_error("RX/end clocks are inconsistent"); }
                    samples_.push_back(end - rx_ns);
                }
                if (applied) { ++stats_.applied_market_messages; }
                ++stats_.market_messages;
                if (stats_.warmup_messages < warmup_) { ++stats_.warmup_messages; }
            } else {
                throw std::runtime_error("Unsupported PITCH message type " + std::to_string(type));
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
