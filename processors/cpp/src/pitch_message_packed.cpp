#include "low_latency/pitch_message.hpp"
#include "pitch_wire_layouts.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace low_latency {
namespace {

using namespace pitch_wire;

template<class Wire> Wire load(std::span<const std::uint8_t> bytes) {
    static_assert(std::is_trivially_copyable_v<Wire> && alignof(Wire) == 1);
    if (bytes.size() < sizeof(Wire)) { throw std::runtime_error("Truncated PITCH message fields"); }
    Wire wire;
    std::memcpy(&wire, bytes.data(), sizeof(wire));
    return wire;
}

// Take packed integers by value, never bind an aligned reference to a member.
template<class T> T from_le(T value) {
    static_assert(std::is_unsigned_v<T>);
    static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big);
    if constexpr (std::endian::native == std::endian::little) { return value; }
    else {
        T swapped = 0;
        for (unsigned i = 0; i < sizeof(T); ++i) {
            swapped = static_cast<T>((swapped << 8) | (value & 0xff));
            value = static_cast<T>(value >> 8);
        }
        return swapped;
    }
}
template<std::size_t N> Symbol symbol_at(const char (&source)[N]) {
    static_assert(N <= 8);
    Symbol symbol{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    bool visible = false;
    for (std::size_t i = 0; i < N; ++i) {
        const auto ch = source[i];
        if (ch < ' ' || ch > '~') { throw std::runtime_error("Invalid PITCH symbol"); }
        symbol[i] = ch;
        visible = visible || ch != ' ';
    }
    if (!visible) { throw std::runtime_error("Empty PITCH symbol"); }
    return symbol;
}
void choice(char value, const char* choices) {
    if (std::string_view(choices).find(value) == std::string_view::npos) {
        throw std::runtime_error("Invalid PITCH indicator");
    }
}
void positive_quantity(std::uint32_t quantity, const char* error) {
    if (quantity == 0) { throw std::runtime_error(error); }
}
template<bool IsTrade, class Fields> void order_fields(PitchMessage& m, const Fields& order) {
    m.time_offset = from_le(order.header.time_offset);
    m.order_id = from_le(order.order_id);
    m.side = order.side;
    choice(m.side, IsTrade ? "B" : "BS");
    m.quantity = from_le(order.quantity);
    m.symbol = symbol_at(order.symbol);
    m.price = from_le(order.price);
    if constexpr (sizeof(order.price) == 2) { m.price *= 100; }
}
template<class Wire> void add_order(PitchMessage& m, std::span<const std::uint8_t> bytes) {
    const auto wire = load<Wire>(bytes);
    order_fields<false>(m, wire.order);
    m.flags = wire.flags;
    if constexpr (std::is_same_v<Wire, AddExpanded>) {
        std::copy_n(wire.participant, 4, m.participant.begin());
        m.customer = wire.customer;
    }
    positive_quantity(m.quantity, "Invalid zero added/traded quantity");
}
template<class Wire> void trade(PitchMessage& m, std::span<const std::uint8_t> bytes) {
    const auto wire = load<Wire>(bytes);
    order_fields<true>(m, wire.order);
    m.execution_id = from_le(wire.execution_id);
    positive_quantity(m.quantity, "Invalid zero added/traded quantity");
}
template<class Wire> void reduce(PitchMessage& m, std::span<const std::uint8_t> bytes) {
    const auto wire = load<Wire>(bytes);
    m.time_offset = from_le(wire.header.time_offset);
    m.order_id = from_le(wire.order_id);
    m.quantity = from_le(wire.quantity);
    positive_quantity(m.quantity, "Invalid zero reduced/executed quantity");
}
template<class Wire> void modify(PitchMessage& m, std::span<const std::uint8_t> bytes) {
    const auto wire = load<Wire>(bytes);
    m.time_offset = from_le(wire.header.time_offset);
    m.order_id = from_le(wire.order_id);
    m.quantity = from_le(wire.quantity);
    m.price = from_le(wire.price);
    if constexpr (sizeof(wire.price) == 2) { m.price *= 100; }
    m.flags = wire.flags;
}

} // namespace

PitchMessage decode_pitch_message_packed(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 2 || bytes[0] != bytes.size()) { throw std::runtime_error("Invalid PITCH message length"); }
    PitchMessage m;
    m.type = bytes[1];
    switch (m.type) {
    case 0x20: {
        const auto wire = load<Time>(bytes);
        m.seconds = from_le(wire.seconds);
        if (bytes.size() > sizeof(Time) && bytes.size() < sizeof(TimeEpoch)) {
            throw std::runtime_error("Truncated PITCH epoch time");
        }
        if (bytes.size() >= sizeof(TimeEpoch)) { m.epoch_seconds = from_le(load<TimeEpoch>(bytes).epoch_seconds); }
        break;
    }
    case 0xb1: {
        const auto wire = load<TimeReference>(bytes);
        m.midnight_reference = from_le(wire.midnight_reference);
        m.seconds = from_le(wire.seconds);
        m.time_offset = from_le(wire.time_offset);
        m.trade_date = from_le(wire.trade_date);
        break;
    }
    case 0x97: case 0x2d: m.time_offset = from_le(load<Header>(bytes).time_offset); break;
    case 0x21: add_order<AddLong>(m, bytes); break;
    case 0x22: add_order<AddShort>(m, bytes); break;
    case 0x2f: add_order<AddExpanded>(m, bytes); break;
    case 0x2a: trade<TradeLong>(m, bytes); break;
    case 0x2b: trade<TradeShort>(m, bytes); break;
    case 0x30: trade<TradeExpanded>(m, bytes); break;
    case 0x23: {
        const auto wire = load<Executed>(bytes);
        m.time_offset = from_le(wire.header.time_offset);
        m.order_id = from_le(wire.order_id);
        m.quantity = from_le(wire.quantity);
        positive_quantity(m.quantity, "Invalid zero reduced/executed quantity");
        m.execution_id = from_le(wire.execution_id);
        break;
    }
    case 0x24: {
        const auto wire = load<ExecutedPrice>(bytes);
        m.time_offset = from_le(wire.header.time_offset);
        m.order_id = from_le(wire.order_id);
        m.quantity = from_le(wire.quantity);
        positive_quantity(m.quantity, "Invalid zero reduced/executed quantity");
        m.remaining_quantity = from_le(wire.remaining_quantity);
        m.execution_id = from_le(wire.execution_id);
        m.price = from_le(wire.price);
        break;
    }
    case 0x25: reduce<ReduceLong>(m, bytes); break;
    case 0x26: reduce<ReduceShort>(m, bytes); break;
    case 0x27: modify<ModifyLong>(m, bytes); break;
    case 0x28: modify<ModifyShort>(m, bytes); break;
    case 0x29: {
        const auto wire = load<Delete>(bytes);
        m.time_offset = from_le(wire.header.time_offset);
        m.order_id = from_le(wire.order_id);
        break;
    }
    case 0x2c: {
        const auto wire = load<TradeBreak>(bytes);
        m.time_offset = from_le(wire.header.time_offset);
        m.execution_id = from_le(wire.execution_id);
        break;
    }
    case 0x31: {
        const auto wire = load<Status>(bytes);
        m.time_offset = from_le(wire.header.time_offset);
        m.symbol = symbol_at(wire.symbol);
        m.indicator = wire.trading_status;
        choice(m.indicator, "AEHLOPQST");
        m.reg_sho = wire.reg_sho;
        choice(m.reg_sho, "01");
        break;
    }
    case 0x95: {
        const auto wire = load<AuctionUpdate>(bytes);
        m.time_offset = from_le(wire.header.time_offset);
        m.symbol = symbol_at(wire.symbol);
        m.auction_type = wire.auction_type;
        choice(m.auction_type, "OCHIMP");
        m.price = from_le(wire.reference_price);
        m.buy_quantity = from_le(wire.buy_quantity);
        m.sell_quantity = from_le(wire.sell_quantity);
        m.indicative_price = from_le(wire.indicative_price);
        m.auction_only_price = from_le(wire.auction_only_price);
        break;
    }
    case 0x96: {
        const auto wire = load<AuctionSummary>(bytes);
        m.time_offset = from_le(wire.header.time_offset);
        m.symbol = symbol_at(wire.symbol);
        m.auction_type = wire.auction_type;
        choice(m.auction_type, "OCHIMP");
        m.price = from_le(wire.price);
        m.quantity = from_le(wire.quantity);
        break;
    }
    case 0x98: {
        const auto wire = load<Retail>(bytes);
        m.time_offset = from_le(wire.header.time_offset);
        m.symbol = symbol_at(wire.symbol);
        m.indicator = wire.indicator;
        choice(m.indicator, "BSAN");
        break;
    }
    default: throw std::runtime_error("Unsupported equities PITCH message type " + std::to_string(m.type));
    }
    return m;
}

} // namespace low_latency
