#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace low_latency {

using Symbol = std::array<char, 8>;

// Normalized U.S. equities multicast PITCH message. Prices use 0.0001 units.
// Fields not present in a message are zero/space initialized. Original type is
// retained, including short/long/expanded encodings. Trailing extensions survive
// in the caller's input; the decoder reads only the documented prefix.
struct PitchMessage {
    std::uint8_t type = 0;
    std::uint32_t time_offset = 0;
    std::uint64_t order_id = 0, execution_id = 0, price = 0;
    std::uint32_t quantity = 0, remaining_quantity = 0;
    Symbol symbol{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    char side = ' ', indicator = ' ', reg_sho = ' ', auction_type = ' ';
    std::uint8_t flags = 0;
    std::array<char, 4> participant{' ', ' ', ' ', ' '};
    char customer = ' ';
    std::uint32_t seconds = 0, epoch_seconds = 0, midnight_reference = 0, trade_date = 0;
    std::uint64_t indicative_price = 0, auction_only_price = 0;
    std::uint32_t buy_quantity = 0, sell_quantity = 0;
    bool operator==(const PitchMessage&) const = default;
    bool order_update() const;
    bool trade() const;
    bool market() const { return order_update() || trade() || type == 0x2c || type == 0x31 || type == 0x95 || type == 0x96 || type == 0x98; }
};

PitchMessage decode_pitch_message(std::span<const std::uint8_t> bytes);
// Alternative decoder: copy validated wire prefixes into packed, named layouts.
PitchMessage decode_pitch_message_packed(std::span<const std::uint8_t> bytes);
// Processor default: packed references into writable receive storage; no wire copy.
// Starts implicit object lifetimes in place, preserving every input byte. No
// wire reference escapes this call. The normalized result is still returned
// by value, just as in the other two decoders.
PitchMessage decode_pitch_message_zero_copy(std::span<std::uint8_t> bytes);

} // namespace low_latency
