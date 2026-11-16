#include "low_latency/pitch_message.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

namespace low_latency {
namespace {
std::uint64_t le(std::span<const std::uint8_t> bytes, std::size_t offset, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) { value |= std::uint64_t{bytes[offset + i]} << (8 * i); }
    return value;
}
Symbol symbol_at(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t width) {
    Symbol symbol{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    bool visible = false;
    for (std::size_t i = 0; i < width; ++i) {
        const auto ch = bytes[offset + i];
        if (ch < ' ' || ch > '~') { throw std::runtime_error("Invalid PITCH symbol"); }
        symbol[i] = static_cast<char>(ch);
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
} // namespace

bool PitchMessage::order_update() const {
    return (type >= 0x21 && type <= 0x29) || type == 0x2f;
}
bool PitchMessage::trade() const { return type == 0x2a || type == 0x2b || type == 0x30; }

PitchMessage decode_pitch_message(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 2 || bytes[0] != bytes.size()) { throw std::runtime_error("Invalid PITCH message length"); }
    PitchMessage m;
    m.type = bytes[1];
    std::size_t size = 0;
    switch (m.type) {
    case 0x20: case 0x97: case 0x2d: size = 6; break;
    case 0xb1: case 0x31: size = 18; break;
    case 0x21: size = 34; break;
    case 0x22: case 0x23: size = 26; break;
    case 0x2f: case 0x2a: size = 41; break;
    case 0x24: size = 38; break;
    case 0x25: size = 18; break;
    case 0x26: size = 16; break;
    case 0x27: case 0x96: size = 27; break;
    case 0x28: size = 19; break;
    case 0x29: case 0x2c: size = 14; break;
    case 0x2b: size = 33; break;
    case 0x30: size = 43; break;
    case 0x95: size = 47; break;
    case 0x98: size = 15; break;
    default: throw std::runtime_error("Unsupported equities PITCH message type " + std::to_string(m.type));
    }
    if (bytes.size() < size) { throw std::runtime_error("Truncated PITCH message fields"); }
    const auto read = [&](std::size_t offset, unsigned width) { return le(bytes, offset, width); };
    if (m.type == 0x20) {
        m.seconds = static_cast<std::uint32_t>(read(2, 4));
        // The optional epoch field is present on BZX/EDGX. Reject partial fields.
        if (bytes.size() > 6 && bytes.size() < 10) { throw std::runtime_error("Truncated PITCH epoch time"); }
        if (bytes.size() >= 10) { m.epoch_seconds = static_cast<std::uint32_t>(read(6, 4)); }
        return m;
    }
    m.time_offset = static_cast<std::uint32_t>(read(2, 4));
    if (m.type == 0xb1) {
        m.midnight_reference = m.time_offset;
        m.seconds = static_cast<std::uint32_t>(read(6, 4));
        m.time_offset = static_cast<std::uint32_t>(read(10, 4));
        m.trade_date = static_cast<std::uint32_t>(read(14, 4));
    } else if (m.type == 0x21 || m.type == 0x22 || m.type == 0x2f || m.trade()) {
        const bool short_form = m.type == 0x22 || m.type == 0x2b;
        const bool expanded = m.type == 0x2f || m.type == 0x30;
        const std::size_t symbol_offset = short_form ? 17 : 19;
        const std::size_t price_offset = short_form ? 23 : expanded ? 27 : 25;
        m.order_id = read(6, 8);
        m.side = static_cast<char>(bytes[14]);
        choice(m.side, m.trade() ? "B" : "BS");
        m.quantity = static_cast<std::uint32_t>(read(15, short_form ? 2 : 4));
        m.symbol = symbol_at(bytes, symbol_offset, expanded ? 8 : 6);
        m.price = read(price_offset, short_form ? 2 : 8) * (short_form ? 100 : 1);
        if (m.trade()) { m.execution_id = read(price_offset + (short_form ? 2 : 8), 8); }
        else {
            const auto flag_offset = price_offset + (short_form ? 2 : 8);
            m.flags = bytes[flag_offset];
            if (expanded) {
                std::copy_n(bytes.begin() + 36, 4, m.participant.begin());
                m.customer = static_cast<char>(bytes[40]);
            }
        }
        if (m.quantity == 0) { throw std::runtime_error("Invalid zero added/traded quantity"); }
    } else if (m.type >= 0x23 && m.type <= 0x29) {
        m.order_id = read(6, 8);
        if (m.type != 0x29) {
            m.quantity = static_cast<std::uint32_t>(read(14, m.type == 0x26 || m.type == 0x28 ? 2 : 4));
            if (m.type != 0x27 && m.type != 0x28 && m.quantity == 0) {
                throw std::runtime_error("Invalid zero reduced/executed quantity");
            }
        }
        if (m.type == 0x23 || m.type == 0x24) {
            m.execution_id = read(m.type == 0x23 ? 18 : 22, 8);
            if (m.type == 0x24) {
                m.remaining_quantity = static_cast<std::uint32_t>(read(18, 4));
                m.price = read(30, 8);
            }
        } else if (m.type == 0x27 || m.type == 0x28) {
            m.price = read(m.type == 0x27 ? 18 : 16, m.type == 0x27 ? 8 : 2) * (m.type == 0x27 ? 1 : 100);
            m.flags = bytes[m.type == 0x27 ? 26 : 18];
        }
    } else if (m.type == 0x2c) { m.execution_id = read(6, 8); }
    else if (m.type == 0x31 || m.type == 0x95 || m.type == 0x96 || m.type == 0x98) {
        m.symbol = symbol_at(bytes, 6, 8);
        if (m.type == 0x31) {
            m.indicator = static_cast<char>(bytes[14]);
            choice(m.indicator, "AEHLOPQST");
            m.reg_sho = static_cast<char>(bytes[15]);
            choice(m.reg_sho, "01");
        } else if (m.type == 0x98) {
            m.indicator = static_cast<char>(bytes[14]);
            choice(m.indicator, "BSAN");
        } else {
            m.auction_type = static_cast<char>(bytes[14]);
            choice(m.auction_type, "OCHIMP");
            m.price = read(15, 8);
            if (m.type == 0x95) {
                m.buy_quantity = static_cast<std::uint32_t>(read(23, 4));
                m.sell_quantity = static_cast<std::uint32_t>(read(27, 4));
                m.indicative_price = read(31, 8);
                m.auction_only_price = read(39, 8);
            } else { m.quantity = static_cast<std::uint32_t>(read(23, 4)); }
        }
    }
    return m;
}
} // namespace low_latency
