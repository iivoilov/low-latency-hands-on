#pragma once

#include <cstddef>
#include <cstdint>

namespace low_latency::pitch_wire {

// Wire-only layouts shared by the copying and in-place reference decoders.
// sizeof/offset/align checks describe the protocol prefix, not a native ABI.
#pragma pack(push, 1)
struct MessagePrefix { std::uint8_t length, type; };
struct Header {
    std::uint8_t length, type;
    std::uint32_t time_offset;
};
struct Time {
    std::uint8_t length, type;
    std::uint32_t seconds;
};
struct TimeEpoch { Time time; std::uint32_t epoch_seconds; };
struct TimeReference {
    std::uint8_t length, type;
    std::uint32_t midnight_reference, seconds, time_offset, trade_date;
};
template<class Quantity, std::size_t SymbolWidth, class Price>
struct OrderFields {
    Header header;
    std::uint64_t order_id;
    char side;
    Quantity quantity;
    char symbol[SymbolWidth];
    Price price;
};
using LongFields = OrderFields<std::uint32_t, 6, std::uint64_t>;
using ShortFields = OrderFields<std::uint16_t, 6, std::uint16_t>;
using ExpandedFields = OrderFields<std::uint32_t, 8, std::uint64_t>;
struct AddLong { LongFields order; std::uint8_t flags; };
struct AddShort { ShortFields order; std::uint8_t flags; };
struct AddExpanded {
    ExpandedFields order;
    std::uint8_t flags;
    char participant[4], customer;
};
struct TradeLong { LongFields order; std::uint64_t execution_id; };
struct TradeShort { ShortFields order; std::uint64_t execution_id; };
struct TradeExpanded { ExpandedFields order; std::uint64_t execution_id; };
struct Executed {
    Header header;
    std::uint64_t order_id;
    std::uint32_t quantity;
    std::uint64_t execution_id;
};
struct ExecutedPrice {
    Header header;
    std::uint64_t order_id;
    std::uint32_t quantity, remaining_quantity;
    std::uint64_t execution_id, price;
};
template<class Quantity> struct Reduce {
    Header header;
    std::uint64_t order_id;
    Quantity quantity;
};
template<class Quantity, class Price> struct Modify {
    Header header;
    std::uint64_t order_id;
    Quantity quantity;
    Price price;
    std::uint8_t flags;
};
using ReduceLong = Reduce<std::uint32_t>;
using ReduceShort = Reduce<std::uint16_t>;
using ModifyLong = Modify<std::uint32_t, std::uint64_t>;
using ModifyShort = Modify<std::uint16_t, std::uint16_t>;
struct Delete { Header header; std::uint64_t order_id; };
struct TradeBreak { Header header; std::uint64_t execution_id; };
struct Status {
    Header header;
    char symbol[8], trading_status, reg_sho, reserved[2];
};
struct AuctionUpdate {
    Header header;
    char symbol[8], auction_type;
    std::uint64_t reference_price;
    std::uint32_t buy_quantity, sell_quantity;
    std::uint64_t indicative_price, auction_only_price;
};
struct AuctionSummary {
    Header header;
    char symbol[8], auction_type;
    std::uint64_t price;
    std::uint32_t quantity;
};
struct Retail { Header header; char symbol[8], indicator; };
#pragma pack(pop)

// Check nested layouts as well as complete documented prefix lengths.
static_assert(sizeof(MessagePrefix) == 2 && alignof(MessagePrefix) == 1);
static_assert(sizeof(Header) == 6 && alignof(Header) == 1);
static_assert(sizeof(Time) == 6 && sizeof(TimeEpoch) == 10 && sizeof(TimeReference) == 18);
static_assert(sizeof(LongFields) == 33 && sizeof(ShortFields) == 25 && sizeof(ExpandedFields) == 35);
static_assert(offsetof(LongFields, order_id) == 6 && offsetof(LongFields, quantity) == 15);
static_assert(offsetof(LongFields, symbol) == 19 && offsetof(LongFields, price) == 25);
static_assert(offsetof(ShortFields, symbol) == 17 && offsetof(ShortFields, price) == 23);
static_assert(offsetof(ExpandedFields, symbol) == 19 && offsetof(ExpandedFields, price) == 27);
static_assert(sizeof(AddLong) == 34 && sizeof(AddShort) == 26 && sizeof(AddExpanded) == 41);
static_assert(offsetof(AddExpanded, participant) == 36 && offsetof(AddExpanded, customer) == 40);
static_assert(sizeof(TradeLong) == 41 && sizeof(TradeShort) == 33 && sizeof(TradeExpanded) == 43);
static_assert(sizeof(Executed) == 26 && sizeof(ExecutedPrice) == 38);
static_assert(offsetof(ExecutedPrice, remaining_quantity) == 18 && offsetof(ExecutedPrice, price) == 30);
static_assert(sizeof(ReduceLong) == 18 && sizeof(ReduceShort) == 16);
static_assert(sizeof(ModifyLong) == 27 && sizeof(ModifyShort) == 19);
static_assert(sizeof(Delete) == 14 && sizeof(TradeBreak) == 14);
static_assert(sizeof(Status) == 18 && sizeof(AuctionUpdate) == 47);
static_assert(sizeof(AuctionSummary) == 27 && sizeof(Retail) == 15);

} // namespace low_latency::pitch_wire
