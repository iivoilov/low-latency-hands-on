#include "low_latency/processor.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using Bytes = std::vector<std::uint8_t>;

// Own fixture bytes while the processor establishes packed object lifetimes.
void process_packet(low_latency::Processor& processor, Bytes payload, std::uint64_t rx_ns) {
    const auto before = payload;
    processor.process(payload, rx_ns);
    if (payload != before) { throw std::runtime_error("Processing changed packet bytes"); }
}

void check(bool b, const char* message) { if (!b) { throw std::runtime_error(message); } }
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Expected protocol rejection");
}
void put(Bytes& b, std::size_t o, std::uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) { b.at(o + i) = static_cast<std::uint8_t>(v >> (8 * i)); }
}
Bytes packet(std::uint32_t sequence, const Bytes& m) {
    Bytes b(8 + m.size(), 0); put(b, 0, b.size(), 2); b[2] = 1; b[3] = 1; put(b, 4, sequence, 4);
    std::copy(m.begin(), m.end(), b.begin() + 8); return b;
}
std::map<std::string, Bytes> fixtures(const char* path) {
    std::ifstream input(path);
    check(input.good(), "Missing shared fixture file");
    std::map<std::string, Bytes> result;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') { continue; }
        std::istringstream in(line);
        std::string name, hex, symbol;
        std::uint64_t price, quantity, id, execution, remaining;
        in >> name >> hex >> symbol >> price >> quantity >> id >> execution >> remaining;
        check(!in.fail(), "Invalid fixture row");
        Bytes bytes;
        for (std::size_t i = 0; i < hex.size(); i += 2) { bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16))); }
        const auto m = low_latency::decode_pitch_message(bytes);
        std::string actual(m.symbol.begin(), m.symbol.end());
        actual.erase(actual.find_last_not_of(' ') + 1);
        check(actual == (symbol == "-" ? "" : symbol) && m.price == price && m.quantity == quantity &&
              m.order_id == id && m.execution_id == execution && m.remaining_quantity == remaining,
              "Normalized fixture mismatch");
        for (std::size_t n = 2; n < bytes.size(); ++n) {
            // Time's 6-byte form is a complete alternative prefix.
            if (name == "time_epoch" && n == 6) { continue; }
            auto short_message = Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
            short_message[0] = static_cast<std::uint8_t>(n);
            rejects([&] { low_latency::decode_pitch_message(short_message); });
        }
        auto extended = bytes;
        // Time extensions begin after the optional, complete epoch field.
        if (name != "time") {
            extended.push_back(0xaa); extended.push_back(0xbb); extended[0] = static_cast<std::uint8_t>(extended.size());
            const auto e = low_latency::decode_pitch_message(extended);
            check(e.price == m.price && e.symbol == m.symbol && e.quantity == m.quantity, "Extension changed decoded prefix");
        }
        result.emplace(name, bytes);
    }
    check(result.size() == 23, "All equities layouts must be covered");
    const auto expanded = low_latency::decode_pitch_message(result.at("add_expanded"));
    check(expanded.participant == std::array<char, 4>{'A','B','C','D'} && expanded.flags == 1, "Lost expanded attribution");
    const auto auction = low_latency::decode_pitch_message(result.at("auction_update"));
    check(auction.buy_quantity == 100 && auction.sell_quantity == 200 && auction.indicative_price == 102600 && auction.auction_only_price == 102700, "Lost auction fields");
    const auto time = low_latency::decode_pitch_message(result.at("time_reference"));
    check(time.midnight_reference == 1699966800 && time.seconds == 34200 && time.time_offset == 123456789 && time.trade_date == 20261114, "Lost time reference");
    return result;
}
std::uint64_t ticks = 0;
std::uint64_t tick() { return ++ticks; }
void quantity_updates(const std::map<std::string, Bytes>& f) {
    for (const char side : {'B', 'S'}) {
        low_latency::Processor p(1, 0, 100, low_latency::LossPolicy::stop, tick, 8, 4, 8);
        std::vector<low_latency::L1Update> events;
        p.set_l1_sink([&](const auto& e) { events.push_back(e); });
        std::uint32_t sequence = 1;
        const auto send = [&](Bytes m) { process_packet(p, packet(sequence++, m), 0); };
        const auto level = [&](std::uint64_t price, std::uint64_t quantity, unsigned orders) {
            check(p.book().snapshot() == std::vector<low_latency::BookLevel>{{"TEST", side, price, quantity, orders}},
                  "Quantity update changed price, aggregate quantity or order count");
            const auto& quote = side == 'B' ? events.back().bid : events.back().ask;
            check(quote && quote->price == price && quote->quantity == quantity, "L1 quote differs from aggregate");
        };
        const auto modify = [&](const char* name, std::uint32_t quantity, std::uint64_t price) {
            auto m = f.at(name);
            const bool short_message = m[1] == 0x28;
            put(m, 14, quantity, short_message ? 2 : 4);
            put(m, short_message ? 16 : 18, short_message ? price / 100 : price, short_message ? 2 : 8);
            send(m);
        };
        send(f.at("unit_clear"));
        auto add = f.at("add_long"); add[14] = static_cast<std::uint8_t>(side); send(add);
        put(add, 6, 12, 8); send(add);
        level(102500, 200, 2);
        modify("modify_long", 0xffffffffU, 102500);
        level(102500, std::uint64_t{0xffffffffU} + 100, 2);
        modify("modify_long", 50, 102500); level(102500, 150, 2);
        const auto before = events.size();
        modify("modify_long", 50, 102500);
        check(events.size() == before, "Unchanged quantity must suppress L1 emission");
        modify("modify_short", 65535, 102500); level(102500, 65635, 2);
        modify("modify_short", 7, 102500); level(102500, 107, 2);
        auto execution = f.at("execute_price"); // 200 executed, independently supplied remainder.
        put(execution, 18, 9, 4); send(execution); level(102500, 109, 2);
        check(events.back().last && events.back().last->price == 102700 && events.back().last->quantity == 200,
              "Quantity fast path lost execution price or hidden size");
        put(execution, 22, 113, 8); put(execution, 18, 11, 4); send(execution); level(102500, 111, 2);
        put(execution, 22, 114, 8); send(execution);
        check(events.back().changed == 4 && events.back().last->execution_id == 114,
              "Unchanged remainder must still emit a new execution");
        put(execution, 22, 115, 8); put(execution, 18, 0, 4); send(execution); level(102500, 100, 1);
        auto remove = f.at("delete"); put(remove, 6, 12, 8); send(remove);
        check(p.book().orders() == 0 && p.book().symbols() == 0, "Last order must prune the symbol");
        put(add, 6, 11, 8); send(add);
        modify("modify_long", 25, 102500); level(102500, 25, 1);
        modify("modify_short", 30, 102600); level(102600, 30, 1);
        modify("modify_long", 0, 102600);
        check(p.book().snapshot().empty() && p.book().symbols() == 0 &&
              !events.back().bid && !events.back().ask && events.back().last->execution_id == 115,
              "Zero modification must remove the sole level and preserve last trade");
        check(p.book_current() && !p.loss_seen(), "Quantity updates must preserve integrity");
    }
}
void decisions(std::map<std::string, Bytes> f) {
    ticks = 0;
    low_latency::Processor p(1, 0, 100, low_latency::LossPolicy::stop, tick, 8, 100, 100);
    std::vector<low_latency::L1Update> events;
    p.set_l1_sink([&](const auto& e) {
        if (e.message_type != 0x97) { check(ticks == p.latency().samples, "Dispatch must follow the sampled endpoint"); }
        events.push_back(e);
    });
    std::uint32_t sequence = 1;
    const auto send = [&](Bytes m) {process_packet(p, packet(sequence++, m), 0);};
    send(f.at("unit_clear"));
    send(f.at("add_long"));
    check(events.size() == 1 && events.back().bid && !events.back().ask && events.back().changed == 1, "One-sided add must emit explicit snapshot");
    auto deeper = f.at("add_long"); put(deeper, 6, 12, 8); put(deeper, 25, 102400, 8);
    send(deeper);
    check(events.size() == 1 && p.stats().l1_no_updates == 1 && ticks == 2, "Deep update must decide no emit and still be sampled");
    auto ask = f.at("add_short"); put(ask, 6, 13, 8); ask[14] = 'S'; put(ask, 23, 1030, 2); send(ask);
    check(events.back().changed == 2 && events.back().bid && events.back().ask->price == 103000, "Ask-only change must carry both current sides");
    send(f.at("execute_price"));
    check(events.back().last->price == 102700 && events.back().last->quantity == 200 && events.back().bid->quantity == 7, "Price/size execution must accept hidden size and preserve resting price");
    auto trade = f.at("trade_long"); put(trade, 33, 113, 8); send(trade);
    check(events.back().changed == 4 && events.back().last->execution_id == 113 && events.back().bid->quantity == 7, "Hidden trade must not change L2");
    put(trade, 33, 114, 8); send(trade);
    check(events.back().changed == 4, "Same-price/size execution must still fire");
    auto broken = f.at("trade_break"); put(broken, 6, 113, 8); const auto before = events.size(); send(broken);
    check(events.size() == before, "Breaking an older trade must not change latest sale");
    put(broken, 6, 114, 8); send(broken);
    check(events.back().last->execution_id == 112 && events.back().last->price == 102700, "Break must roll back to latest unbroken execution");
    send(f.at("status")); send(f.at("retail"));
    check(events.back().retail == 'A' && events.back().trading_status == 'T' && events.back().reg_sho == '1', "Lost symbol state");
    const auto n = events.size(); send(f.at("auction_update")); send(f.at("auction_summary"));
    check(events.size() == n && events.back().last->execution_id == 112, "Auction metadata must not double-count last sales");
    send(f.at("unit_clear"));
    check(!events.back().bid && !events.back().ask && events.back().last->execution_id == 112 && events.back().changed == 3, "Unit Clear must clear quotes and preserve trades");
    p.note_loss();
    check(!events.back().book_current && !events.back().trade_history_current && events.back().changed == 32, "Loss must publish invalidity");
    send(f.at("unit_clear"));
    check(events.back().book_current && !events.back().trade_history_current, "Order reset cannot restore missing trade history");
    send(f.at("end_session"));
    check(p.session_ended(), "Missing session end state");
    rejects([&] {send(f.at("add_long"));});
}
void boundaries(const std::map<std::string, Bytes>& f) {
    low_latency::Processor p(1,0,100,low_latency::LossPolicy::stop,tick,8,2,1);
    process_packet(p, packet(1, f.at("unit_clear")),0);
    process_packet(p, packet(2, f.at("trade_long")),0);
    auto t = f.at("trade_long"); put(t,33,112,8);
    rejects([&] {process_packet(p, packet(3,t),0);});
    low_latency::OrderBook book(2);
    book.apply(f.at("add_short"));
    book.apply(f.at("reduce_short"));
    check(book.snapshot()[0].quantity == 80, "Short reduction quantity mismatch");
    book.apply(f.at("modify_short"));
    check(book.snapshot()[0].quantity == 90 && book.snapshot()[0].price == 102800, "Short modify normalization mismatch");
    book.apply(f.at("execute"));
    check(book.snapshot()[0].quantity == 70, "Execution must use current modified order");
    rejects([&] {book.apply(f.at("add_expanded"));});
    book.clear(); book.apply(f.at("add_expanded"));
    check(book.snapshot()[0].symbol == "LONGSYMB", "Expanded symbol must preserve all eight bytes");
}
void heartbeat_and_history_boundaries(const std::map<std::string, Bytes>& f) {
    low_latency::Processor p(1,0,100,low_latency::LossPolicy::stop,tick,8,2,1);
    const Bytes heartbeat{8,0,0,0,0,0,0,0};
    process_packet(p, heartbeat,0);
    process_packet(p, packet(1,f.at("unit_clear")),0);
    process_packet(p, packet(2,f.at("trade_long")),0);
    process_packet(p, packet(3,f.at("trade_break")),0);
    check(p.l1().trades() == 0 && p.l1().symbols().empty(), "Breaking the only trade must clear last and release retained state");
    auto t = f.at("trade_long"); put(t,33,112,8);
    process_packet(p, packet(4,t),0); // Break freed the one available trade slot.
    process_packet(p, packet(5,f.at("end_session")),0); process_packet(p, heartbeat,0);
    check(p.stats().unsequenced_heartbeats == 2 && p.stats().sequence_errors == 0, "Unsequenced heartbeats must not advance sequence");
    low_latency::Processor joined(1,0,100,low_latency::LossPolicy::continue_processing,tick,8,2,1);
    process_packet(joined, heartbeat,0); process_packet(joined, packet(100,f.at("trade_long")),0);
    check(joined.stats().joined_midstream && joined.stats().missing_messages == 0 && !joined.l1().trade_history_current(), "An unsequenced heartbeat cannot establish a known prefix");
    low_latency::Processor missing(1,0,100,low_latency::LossPolicy::stop,tick,8,2,1);
    process_packet(missing, packet(1,f.at("unit_clear")),0);
    rejects([&] {process_packet(missing, packet(2,f.at("trade_break")),0);});
    low_latency::Processor metadata(1,0,100,low_latency::LossPolicy::continue_processing,tick,8,1,1);
    process_packet(metadata, packet(1,f.at("status")),0);
    auto status = f.at("status"); status[6] = 'X'; metadata.note_loss();
    rejects([&] {process_packet(metadata, packet(2,status),0);});
}

void dated_history(const std::map<std::string, Bytes>& f) {
    low_latency::Processor p(1,0,100,low_latency::LossPolicy::stop,tick,8,2,2);
    process_packet(p, packet(1,f.at("unit_clear")),0);
    process_packet(p, packet(2,f.at("time_reference")),0);
    process_packet(p, packet(3,f.at("trade_long")),0);
    auto next_day = f.at("time_reference"); put(next_day,14,20261115,4);
    process_packet(p, packet(4,next_day),0);
    process_packet(p, packet(5,f.at("trade_long")),0); // The same execution ID on a different trade date is valid.
    check(p.l1().trades() == 2 && p.l1().symbols().begin()->second.l1.last->trade_date == 20261115, "Execution IDs are day scoped");
    process_packet(p, packet(6,f.at("trade_break")),0);
    const auto& last = p.l1().symbols().begin()->second.l1.last;
    check(last && last->trade_date == 20261114 && last->execution_id == 111, "Break must address current trade date and preserve prior-day history");
}

}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "Need shared fixture path");
        const auto f = fixtures(argv[1]);
        quantity_updates(f);
        decisions(f);
        boundaries(f);
        heartbeat_and_history_boundaries(f);
        dated_history(f);
        std::cout << "PITCH/L1 tests passed\n";
    } catch (const std::exception& e) {std::cerr << e.what() << '\n'; return 1;}
}
