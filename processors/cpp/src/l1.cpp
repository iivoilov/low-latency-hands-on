#include "low_latency/processor.hpp"

#include <stdexcept>

namespace low_latency {
L1Book::L1Book(std::size_t max_symbols, std::size_t max_trades)
    : max_symbols_(max_symbols), max_trades_(max_trades) {
    if (max_trades == 0 || max_trades > OrderBook::largest_order_limit) {
        throw std::invalid_argument("Need 1..100000000 maximum retained trades");
    }
    trades_.reserve(max_trades);
}
SymbolState& L1Book::state(const Symbol& symbol, bool current) {
    auto found = symbols_.find(symbol);
    if (found != symbols_.end()) { return found->second; }
    if (symbols_.size() == max_symbols_) { throw std::runtime_error("PITCH retained symbol limit exceeded"); }
    auto& result = symbols_[symbol];
    result.l1.symbol = symbol;
    result.l1.book_current = current;
    result.l1.trade_history_current = history_current_;
    return result;
}
std::optional<L1Update> L1Book::update(SymbolState& state, const L1Update& previous,
                                     const OrderBook& book, bool current, std::uint8_t type) {
    auto& l1 = state.l1;
    const auto [bid, ask] = book.top(l1.symbol);
    l1.bid = bid;
    l1.ask = ask;
    l1.book_current = current;
    l1.trade_history_current = history_current_;
    l1.message_type = type;
    l1.changed = static_cast<std::uint8_t>((l1.bid != previous.bid ? 1 : 0) |
        (l1.ask != previous.ask ? 2 : 0) | (l1.last != previous.last ? 4 : 0) |
        (l1.trading_status != previous.trading_status || l1.reg_sho != previous.reg_sho ? 8 : 0) |
        (l1.retail != previous.retail ? 16 : 0) |
        (l1.book_current != previous.book_current || l1.trade_history_current != previous.trade_history_current ? 32 : 0));
    return l1.changed ? std::optional<L1Update>{l1} : std::nullopt;
}
std::optional<L1Update> L1Book::decide(const PitchMessage& message, const BookEffect& effect,
                                     const OrderBook& book, bool current) {
    const auto type = message.type;
    if (type == 0x2c) {
        const auto found = trades_.find(ExecutionKey{trade_date_, message.execution_id});
        // A mid-session join/loss may omit a broken execution. Do not invent it.
        if (found == trades_.end()) {
            if (history_current_) { throw std::runtime_error("Trade Break references a missing execution"); }
            return std::nullopt;
        }
        const auto record = found->second;
        auto& symbol = symbols_.at(record.symbol);
        const auto previous = symbol.l1;
        if (record.previous) { trades_.at(*record.previous).next = record.next; }
        if (record.next) { trades_.at(*record.next).previous = record.previous; }
        else { symbol.l1.last = record.previous ? std::optional<LastTrade>{trades_.at(*record.previous).trade} : std::nullopt; }
        trades_.erase(found);
        auto result = update(symbol, previous, book, current, type);
        if (!symbol.l1.bid && !symbol.l1.ask && !symbol.l1.last && !symbol.metadata) { symbols_.erase(record.symbol); }
        return result;
    }
    const auto key = message.order_update() ? effect.symbol : message.symbol;
    auto& symbol = state(key, current);
    const auto previous = symbol.l1;
    auto trade = message.trade() ? std::optional<LastTrade>{LastTrade{message.execution_id, message.price, message.quantity}} : effect.trade;
    if (trade) {
        trade->trade_date = trade_date_;
        const ExecutionKey key_id{trade_date_, trade->execution_id};
        if (trades_.contains(key_id)) { throw std::runtime_error("Duplicate PITCH execution ID"); }
        if (trades_.size() == max_trades_) { throw std::runtime_error("PITCH retained trade limit exceeded"); }
        const auto prev = symbol.l1.last ? std::optional<ExecutionKey>{ExecutionKey{symbol.l1.last->trade_date, symbol.l1.last->execution_id}} : std::nullopt;
        trades_.emplace(key_id, TradeRecord{key, *trade, prev, std::nullopt});
        if (prev) { trades_.at(*prev).next = key_id; }
        symbol.l1.last = trade;
    }
    if (type == 0x31) {
        symbol.l1.trading_status = message.indicator;
        symbol.l1.reg_sho = message.reg_sho;
        symbol.metadata = true;
    } else if (type == 0x98) {
        symbol.l1.retail = message.indicator;
        symbol.metadata = true;
    } else if (type == 0x95) {
        symbol.auction_update = message;
        symbol.metadata = true;
    } else if (type == 0x96) {
        // Auction summaries can repeat/reprice and duplicate individual fills.
        // They are auction metadata, never additional last-sale executions.
        symbol.auction_summary = message;
        symbol.metadata = true;
    }
    auto result = update(symbol, previous, book, current, type);
    if (!symbol.l1.bid && !symbol.l1.ask && !symbol.l1.last && !symbol.metadata) { symbols_.erase(key); }
    return result;
}
void L1Book::refresh(const OrderBook& book, bool current, std::uint8_t type,
                     const std::function<void(const L1Update&)>& emit) {
    for (auto it = symbols_.begin(); it != symbols_.end();) {
        auto& state = it->second;
        const auto previous = state.l1;
        if (const auto event = update(state, previous, book, current, type)) { emit(*event); }
        if (!state.l1.bid && !state.l1.ask && !state.l1.last && !state.metadata) { it = symbols_.erase(it); }
        else { ++it; }
    }
}
} // namespace low_latency
