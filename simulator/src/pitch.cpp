#include "low_latency/pitch.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace low_latency {
namespace {

void put(Packet& packet, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        packet.bytes[packet.size++] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

void message(Packet& packet, std::uint8_t length, std::uint8_t type, std::uint32_t offset) {
    put(packet, length, 1);
    put(packet, type, 1);
    put(packet, offset, 4);
}

} // namespace

std::uint32_t advance_sequence(std::uint32_t sequence, std::uint8_t count) {
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(sequence) - 1 + count)
                                      % maximum + 1);
}

void validate_pitch(std::span<const std::uint8_t> payload) {
    if (payload.size() < 8 || payload.size() > max_payload) {
        throw std::runtime_error("PITCH payload must contain 8..1472 bytes");
    }
    const auto length = static_cast<std::size_t>(payload[0]) |
                        (static_cast<std::size_t>(payload[1]) << 8);
    if (length != payload.size()) {
        throw std::runtime_error("PITCH header length does not match UDP payload");
    }
    std::size_t offset = 8;
    for (unsigned i = 0; i < payload[2]; ++i) {
        if (offset + 2 > payload.size() || payload[offset] < 2 ||
            offset + payload[offset] > payload.size()) {
            throw std::runtime_error("Truncated or invalid PITCH message");
        }
        offset += payload[offset];
    }
    if (offset != payload.size()) {
        throw std::runtime_error("PITCH message count does not match UDP payload");
    }
}

PitchGenerator::PitchGenerator(std::uint64_t seed, std::uint8_t unit,
                               unsigned events_per_packet, std::vector<std::string> symbols,
                               unsigned levels, unsigned orders_per_level)
    : random_state_(seed), unit_(unit), events_per_packet_(events_per_packet),
      symbols_(std::move(symbols)), levels_(levels) {
    if (seed == 0 || unit == 0 || events_per_packet < 1 || events_per_packet > 40 ||
        symbols_.empty() || levels < 1 || levels > 100 ||
        orders_per_level < 1 || orders_per_level > 100 ||
        symbols_.size() > 4096 / (2 * levels * orders_per_level)) {
        throw std::invalid_argument("Need nonzero seed/unit, 1..40 events, 1..100 levels/orders, and <=4096 book orders");
    }
    std::unordered_set<std::string> seen;
    for (const auto& symbol : symbols_) {
        if (!seen.insert(symbol).second) {
            throw std::invalid_argument("Symbols must be unique");
        }
        if (symbol.empty() || symbol.size() > 6) {
            throw std::invalid_argument("Symbols must contain 1..6 printable non-space ASCII characters");
        }
        for (const auto ch : symbol) {
            if (ch < '!' || ch > '~') {
                throw std::invalid_argument("Symbols must contain 1..6 printable non-space ASCII characters");
            }
        }
    }
    for (std::size_t symbol = 0; symbol < symbols_.size(); ++symbol) {
        const auto mid = 1'000'000 + symbol * 100'000;
        for (const auto side : {'B', 'S'}) {
            for (unsigned level = 1; level <= levels; ++level) {
                const auto ticks = std::uint64_t{level} * 100;
                for (unsigned order = 0; order < orders_per_level; ++order) {
                    orders_.push_back({0, 0, symbol, side, side == 'B' ? mid - ticks : mid + ticks});
                }
            }
        }
    }
    refill_ = orders_.size();
}

std::uint64_t PitchGenerator::random() {
    // xorshift64*: reproducible across standard library implementations.
    random_state_ ^= random_state_ >> 12;
    random_state_ ^= random_state_ << 25;
    random_state_ ^= random_state_ >> 27;
    return random_state_ * 2685821657736338717ULL;
}

void PitchGenerator::add(Packet& packet, Order& order, std::uint32_t offset) {
    order.id = ++order_id_;
    order.quantity = static_cast<std::uint32_t>((random() % 20 + 1) * 100);
    message(packet, 34, 0x21, offset);
    put(packet, order.id, 8);
    put(packet, static_cast<std::uint8_t>(order.side), 1);
    put(packet, order.quantity, 4);
    const auto& symbol = symbols_[order.symbol];
    for (unsigned i = 0; i < 6; ++i) {
        put(packet, static_cast<std::uint8_t>(i < symbol.size() ? symbol[i] : ' '), 1);
    }
    put(packet, order.price, 8);
    put(packet, 1, 1);
}

Packet PitchGenerator::next(std::uint64_t elapsed_ns, unsigned event_limit) {
    if (elapsed_ns >= 52'200'000'000'000ULL) {
        throw std::runtime_error("Synthetic session has reached midnight; start a new run");
    }
    const auto events = event_limit == 0 ? events_per_packet_ : std::min(event_limit, events_per_packet_);
    Packet packet;
    packet.size = 8;
    const auto second = static_cast<std::uint32_t>(34200 + elapsed_ns / 1'000'000'000);
    const auto offset = static_cast<std::uint32_t>(elapsed_ns % 1'000'000'000);
    unsigned count = 0;
    if (!started_ || second != last_second_) {
        message(packet, 6, 0x20, second);
        ++count;
        last_second_ = second;
    }
    if (!started_) {
        message(packet, 6, 0x97, offset);
        ++count;
        started_ = true;
    }
    for (unsigned i = 0; i < events; ++i) {
        if (initialized_ < orders_.size()) {
            add(packet, orders_[initialized_++], offset);
        } else if (refill_ < orders_.size()) {
            add(packet, orders_[refill_], offset);
            refill_ = orders_.size();
        } else {
            const auto index = static_cast<std::size_t>(random() % orders_.size());
            auto& order = orders_[index];
            const auto action = random() % 100;
            if (action < 30) {
                const auto mid = 1'000'000 + order.symbol * 100'000;
                const auto ticks = (random() % levels_ + 1) * 100;
                order.price = order.side == 'B' ? mid - ticks : mid + ticks;
                order.quantity = static_cast<std::uint32_t>((random() % 20 + 1) * 100);
                message(packet, 27, 0x27, offset);
                put(packet, order.id, 8);
                put(packet, order.quantity, 4);
                put(packet, order.price, 8);
                put(packet, 1, 1); // Displayed; reset priority on price/quantity changes.
            } else if (action < 55 && order.quantity > 1) {
                const auto canceled = static_cast<std::uint32_t>(random() % (order.quantity - 1) + 1);
                message(packet, 18, 0x25, offset);
                put(packet, order.id, 8);
                put(packet, canceled, 4);
                order.quantity -= canceled;
            } else if (action < 85) {
                auto quantity = order.quantity;
                if (order.quantity > 1 && random() % 4 != 0) {
                    quantity = static_cast<std::uint32_t>(random() % (order.quantity - 1) + 1);
                }
                message(packet, 26, 0x23, offset);
                put(packet, order.id, 8);
                put(packet, quantity, 4);
                put(packet, ++execution_id_, 8);
                order.quantity -= quantity;
            } else {
                message(packet, 14, 0x29, offset);
                put(packet, order.id, 8);
                order.quantity = 0;
            }
            if (order.quantity == 0) { refill_ = index; }
        }
        ++count;
    }
    packet.bytes[0] = static_cast<std::uint8_t>(packet.size);
    packet.bytes[1] = static_cast<std::uint8_t>(packet.size >> 8);
    packet.bytes[2] = static_cast<std::uint8_t>(count);
    packet.bytes[3] = unit_;
    for (unsigned i = 0; i < 4; ++i) {
        packet.bytes[4 + i] = static_cast<std::uint8_t>(sequence_ >> (8 * i));
    }
    sequence_ = advance_sequence(sequence_, packet.bytes[2]);
    return packet;
}

std::vector<BookLevel> PitchGenerator::book() const {
    using Key = std::tuple<std::string, char, std::uint64_t>;
    std::map<Key, BookLevel> levels;
    for (const auto& order : orders_) {
        if (order.quantity == 0) { continue; }
        const auto key = Key{symbols_[order.symbol], order.side, order.price};
        auto [it, inserted] = levels.try_emplace(key,
            BookLevel{symbols_[order.symbol], order.side, order.price, 0, 0});
        (void)inserted;
        it->second.quantity += order.quantity;
        ++it->second.orders;
    }
    std::vector<BookLevel> result;
    for (const auto& [key, level] : levels) { result.push_back(level); }
    // Best price first on both sides, grouped by symbol and side.
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.symbol != b.symbol) { return a.symbol < b.symbol; }
        if (a.side != b.side) { return a.side < b.side; }
        return a.side == 'B' ? a.price > b.price : a.price < b.price;
    });
    return result;
}

} // namespace low_latency
