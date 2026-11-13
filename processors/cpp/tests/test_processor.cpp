#include "low_latency/packet_ring.hpp"
#include "low_latency/processor.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
template<class Function> void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Expected rejection");
}

using Bytes = std::vector<std::uint8_t>;
void put(Bytes& bytes, std::size_t offset, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) { bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (8 * i)); }
}

Bytes control(std::uint8_t type) { return {6, type, 0, 0, 0, 0}; }
Bytes event(std::uint8_t type, std::uint64_t id, std::uint32_t quantity = 100,
            std::uint64_t price = 100000, char side = 'B', std::string symbol = "TEST") {
    const unsigned length = type == 0x21 ? 34 : type == 0x27 ? 27 : type == 0x23 ? 26 : type == 0x25 ? 18 : 14;
    Bytes result(length, 0);
    result[0] = static_cast<std::uint8_t>(length);
    result[1] = type;
    put(result, 6, id, 8);
    if (type == 0x21) {
        result[14] = static_cast<std::uint8_t>(side);
        put(result, 15, quantity, 4);
        symbol.resize(6, ' ');
        std::copy(symbol.begin(), symbol.end(), result.begin() + 19);
        put(result, 25, price, 8);
        result[33] = 1;
    } else if (type == 0x27) {
        put(result, 14, quantity, 4);
        put(result, 18, price, 8);
        result[26] = 1;
    } else if (type == 0x23 || type == 0x25) { put(result, 14, quantity, 4); }
    return result;
}

Bytes datagram(std::uint32_t sequence, const std::vector<Bytes>& messages, std::uint8_t unit = 1) {
    Bytes result(8, 0);
    result[2] = static_cast<std::uint8_t>(messages.size());
    result[3] = unit;
    put(result, 4, sequence, 4);
    for (const auto& message : messages) { result.insert(result.end(), message.begin(), message.end()); }
    put(result, 0, result.size(), 2);
    return result;
}

std::uint64_t ticks = 0;
std::uint64_t clock_tick() { return ++ticks; }

void test_book() {
    low_latency::OrderBook book;
    book.apply(event(0x21, 1, 100, 100000));
    book.apply(event(0x21, 2, 50, 100000));
    book.apply(event(0x21, 3, 40, 100200, 'S'));
    auto levels = book.snapshot();
    check(levels == std::vector<low_latency::BookLevel>{{"TEST", 'B', 100000, 150, 2},
                                                       {"TEST", 'S', 100200, 40, 1}}, "Wrong initial L2 aggregation");
    book.apply(event(0x27, 2, 70, 99900));
    levels = book.snapshot();
    check(levels.size() == 3 && levels[0].price == 100000 && levels[1].price == 99900,
          "Modify must move quantity/order count and sort best bid first");
    book.apply(event(0x25, 1, 30));
    check(book.snapshot()[0].quantity == 70, "Wrong reduced level quantity");
    book.apply(event(0x23, 1, 70));
    check(book.orders() == 2 && book.snapshot()[0].price == 99900, "Full execution must remove empty level");
    book.apply(event(0x29, 2));
    book.apply(event(0x27, 3, 0));
    check(book.orders() == 0 && book.snapshot().empty(), "Delete/zero Modify must remove orders/levels");
    rejects([&] { book.apply(event(0x29, 99)); });
    rejects([&] { book.apply(event(0x21, 1, 0)); });
    rejects([&] { book.apply(event(0x21, 1, 10, 100000, 'X')); });
    book.apply(event(0x21, 1, 10));
    rejects([&] { book.apply(event(0x21, 1, 10)); });
    rejects([&] { book.apply(event(0x23, 1, 11)); });
    check(book.snapshot()[0].quantity == 10, "Invalid reduction must not mutate the book");
    book.apply(event(0x25, 1, 10));
    check(book.snapshot().empty(), "Full reduction must remove the order");
    auto extended = event(0x21, 5);
    extended.insert(extended.end(), {0xaa, 0xbb});
    extended[0] = 36;
    book.apply(extended);
    check(book.orders() == 1, "Known message extensions must be accepted");
    book.clear();
    check(book.snapshot().empty(), "Unit clear must erase all levels");
}

void test_timing() {
    ticks = 0;
    low_latency::Processor processor(1, 1, 1000, low_latency::LossPolicy::stop, clock_tick);
    processor.process(datagram(1, {control(0x20), control(0x97), event(0x21, 1)}), 0);
    check(ticks == 0 && processor.stats().warmup_messages == 1, "Controls and warmup must not be sampled");
    std::uint32_t sequence = 4;
    for (unsigned packet = 0; packet < 25; ++packet) {
        std::vector<Bytes> messages;
        for (unsigned i = 0; i < 40; ++i) { messages.push_back(event(0x21, 2 + packet * 40 + i)); }
        processor.process(datagram(sequence, messages), 0);
        sequence += 40;
    }
    const auto summary = processor.latency();
    check(processor.complete() && processor.book_current(), "Valid sample target should complete");
    check(summary.samples == 1000 && summary.minimum == 1 && summary.p50 == 500 && summary.p99 == 990 &&
          summary.p999 == 999 && summary.maximum == 1000, "Wrong RX endpoints or nearest-rank percentiles");
    processor.process(datagram(sequence, {event(0x21, 2000)}), 0);
    check(processor.latency().samples == 1000 && ticks == 1000, "Sample storage must stay bounded");
    low_latency::Processor other(1, 0, 1);
    other.process(datagram(1, {event(0x21, 1)}, 2), 0);
    check(other.stats().other_unit_packets == 1 && other.book().orders() == 0, "Other units must be isolated");
}

void test_loss() {
    low_latency::Processor strict(1, 0, 10);
    strict.process(datagram(1, {control(0x97), event(0x21, 1, 10)}), low_latency::monotonic_ns());
    rejects([&] { strict.process(datagram(4, {event(0x25, 1, 1)}), 0); });
    check(strict.stats().missing_messages == 1 && !strict.book_current(), "Gap must invalidate a benchmark");
    check(strict.book().snapshot()[0].quantity == 10, "Gap packet must not be applied in stop mode");
    low_latency::GapRange gap;
    check(strict.pop_gap(gap) && gap == low_latency::GapRange{1, 3, 3, 1, 0},
          "Strict mode must queue the missing range before stopping");

    low_latency::Processor live(1, 0, 20, low_latency::LossPolicy::continue_processing);
    live.process(datagram(1, {control(0x97), event(0x21, 1, 10)}), 0);
    live.process(datagram(4, {event(0x25, 99, 1)}), 0); // Lost Add for order 99.
    check(!live.book_current() && live.stats().skipped_market_messages == 1, "Missing-order update must be skipped in partial mode");
    live.process(datagram(5, {event(0x25, 1, 1)}), 0);
    live.process(datagram(5, {event(0x25, 1, 1)}), 0); // Duplicate packet.
    check(live.book().snapshot()[0].quantity == 9 && live.stats().out_of_order_packets == 1,
          "Duplicate packet must not be applied twice");
    live.process(datagram(6, {control(0x97), event(0x21, 100, 50, 101000, 'S')}), 0);
    check(live.book_current() && live.loss_seen() && live.book().orders() == 1,
          "Unit Clear must restore current state while preserving loss history");
    live.note_loss(); // RX-ring/kernel loss preceding the next queued datagram.
    check(!live.book_current(), "External packet loss must invalidate the book");
    live.process(datagram(8, {control(0x97)}), 0);
    check(live.book_current() && live.book().snapshot().empty(), "Reset must recover from external loss");
    check(live.pop_gap(gap) && gap == low_latency::GapRange{1, 3, 3, 1, 0} && !live.pop_gap(gap),
          "Reader must queue the gap once, independent of RX diagnostics, duplicates, and resets");
    for (const auto sequence : {50U, 3'000'000'000U}) {
        low_latency::Processor joined(1, 0, 10, low_latency::LossPolicy::continue_processing);
        joined.process(datagram(sequence, {event(0x21, 5)}), 0);
        check(!joined.book_current() && joined.book().orders() == 1 && joined.stats().joined_midstream &&
              joined.stats().missing_messages == 0, "Mid-session join must accept either half of sequence space without inventing loss counts");
        check(!joined.pop_gap(gap), "Unknown startup prefix must not invent a bounded gap");
    }
    low_latency::Processor malformed(1, 0, 1);
    rejects([&] { malformed.process(Bytes{8, 0, 1, 1, 1, 0, 0, 0}, 0); });
    check(malformed.stats().protocol_errors == 1 && !malformed.book_current(), "Bad framing must invalidate state");
    low_latency::Processor unknown(1, 0, 1);
    rejects([&] { unknown.process(datagram(1, {Bytes{2, 0xff}}), 0); });
    check(!unknown.book_current(), "Unknown book semantics must not be reported as current");
}

void test_gap_queue() {
    low_latency::GapQueue queue(2);
    low_latency::GapRange gap;
    const low_latency::GapRange a{1, 10, 15, 6, 100};
    const low_latency::GapRange b{2, 20, 21, 2, 200};
    const low_latency::GapRange c{3, 30, 30, 1, 300};
    check(!queue.try_pop(gap) && queue.try_push(a) && queue.try_push(b), "Gap queue FIFO setup failed");
    check(!queue.try_push(c), "Full gap queue must refuse to overwrite an unread request");
    check(queue.try_pop(gap) && gap == a && queue.try_push(c), "Gap queue wraparound lost the oldest request");
    check(queue.try_pop(gap) && gap == b && queue.try_pop(gap) && gap == c && !queue.try_pop(gap), "Gap queue FIFO order changed");
    rejects([] { low_latency::GapQueue invalid(3); });

    low_latency::Processor overflow(3, 0, 10, low_latency::LossPolicy::continue_processing,
                                    low_latency::monotonic_ns, 1);
    overflow.process(datagram(1, {control(0x97)}, 3), 0);
    overflow.process(datagram(4, {event(0x21, 1)}, 3), 100);
    overflow.process(datagram(7, {event(0x21, 2)}, 3), 200);
    check(overflow.stats().gap_ranges_enqueued == 1 && overflow.stats().gap_ranges_dropped == 1 &&
          overflow.book().orders() == 2, "Full gap queue must count the dropped notification and keep processing input");
    check(overflow.pop_gap(gap) && gap == low_latency::GapRange{3, 2, 3, 2, 100}, "Notification overflow overwrote the first gap");
    overflow.process(datagram(10, {event(0x21, 3)}, 3), 300);
    check(overflow.pop_gap(gap) && gap == low_latency::GapRange{3, 8, 9, 2, 300} &&
          overflow.stats().missing_messages == 6, "Reader must resume enqueuing when space becomes available");

    low_latency::Processor wrap(1, 0, 10, low_latency::LossPolicy::continue_processing);
    wrap.process(datagram(UINT32_MAX - 2, {control(0x97)}), 0);
    wrap.process(datagram(2, {event(0x21, 1)}), 100);
    check(wrap.pop_gap(gap) && gap == low_latency::GapRange{1, UINT32_MAX - 1, 1, 3, 100},
          "Wrapped gap must skip sequence zero and retain exact bounds/count");

    low_latency::Processor heartbeat(1, 0, 1, low_latency::LossPolicy::continue_processing);
    heartbeat.process(datagram(1, {control(0x97)}), 0);
    heartbeat.process(datagram(5, {}), 100);
    heartbeat.process(datagram(5, {}), 200);
    check(heartbeat.pop_gap(gap) && gap == low_latency::GapRange{1, 2, 4, 3, 100} &&
          !heartbeat.pop_gap(gap), "Heartbeat must expose a gap without duplicating its notification");

    low_latency::GapQueue concurrent(64);
    constexpr std::uint32_t count = 100000;
    std::jthread producer([&](std::stop_token token) {
        for (std::uint32_t n = 1; n <= count && !token.stop_requested(); ++n) {
            const low_latency::GapRange item{7, n, n, 1, std::uint64_t{n} * 10};
            while (!concurrent.try_push(item) && !token.stop_requested()) { std::this_thread::yield(); }
        }
    });
    for (std::uint32_t n = 1; n <= count; ++n) {
        while (!concurrent.try_pop(gap)) { std::this_thread::yield(); }
        check(gap == low_latency::GapRange{7, n, n, 1, std::uint64_t{n} * 10},
              "Concurrent recovery handoff corrupted a range");
    }
    producer.join();
}

void test_workload() {
    low_latency::PitchGenerator generator(42, 1, 8, {"AAPL", "MSFT", "SPY"});
    low_latency::Processor processor(1, generator.initial_orders(), 100000);
    std::uint64_t packet_index = 0;
    const auto generate = [&](std::uint64_t messages) {
        while (messages) {
            const auto events = static_cast<unsigned>(std::min<std::uint64_t>(messages, 8));
            const auto packet = generator.next(packet_index++ * 100000, events);
            processor.process(packet.data(), low_latency::monotonic_ns());
            messages -= events;
        }
    };
    generate(generator.initial_orders());
    check(processor.book().snapshot() == generator.book(), "Initial L2 must match the generator oracle");
    generate(100000);
    check(processor.complete() && processor.latency().samples == 100000 && processor.book_current(),
          "Full workload must have exactly 100000 post-initialization samples");
    check(processor.book().snapshot() == generator.book(), "Decoded final L2 differs from workload oracle");
}

void test_ring() {
    low_latency::PacketRing ring(2);
    auto* first = ring.producer_slot();
    first->packet.size = 31;
    first->packet.bytes[0] = 123;
    first->rx_ns = 100;
    ring.publish();
    auto* second = ring.producer_slot();
    second->packet.size = 200;
    second->rx_ns = 200;
    ring.publish();
    check(ring.producer_slot() == nullptr, "Full ring must refuse writes");
    check(ring.consumer_slot()->packet.size == 31 && ring.consumer_slot()->packet.bytes[0] == 123 &&
          ring.consumer_slot()->rx_ns == 100, "Overflow must never overwrite unread data");
    ring.release();
    check(ring.producer_slot() != nullptr && ring.consumer_slot()->packet.size == 200, "Release must preserve the next variable-length packet");
    ring.release();
    check(ring.consumer_slot() == nullptr, "Empty ring must refuse reads");
    rejects([] { low_latency::PacketRing invalid(3); });

    low_latency::PacketRing threaded(64);
    constexpr std::uint64_t count = 100000;
    std::jthread producer([&](std::stop_token token) {
        for (std::uint64_t n = 0; n < count && !token.stop_requested(); ++n) {
            auto* slot = threaded.producer_slot();
            while (!slot && !token.stop_requested()) { std::this_thread::yield(); slot = threaded.producer_slot(); }
            if (!slot) { return; }
            slot->packet.size = static_cast<std::size_t>(8 + n % 1450);
            for (unsigned i = 0; i < 8; ++i) { slot->packet.bytes[i] = static_cast<std::uint8_t>(n >> (i * 8)); }
            std::fill(slot->packet.bytes.begin() + 8,
                      slot->packet.bytes.begin() + static_cast<std::ptrdiff_t>(slot->packet.size),
                      static_cast<std::uint8_t>(n));
            slot->rx_ns = n * 10;
            slot->dropped_before = n / 1000;
            threaded.publish();
        }
    });
    for (std::uint64_t n = 0; n < count; ++n) {
        const auto* slot = threaded.consumer_slot();
        while (!slot) { std::this_thread::yield(); slot = threaded.consumer_slot(); }
        std::uint64_t observed = 0;
        for (unsigned i = 0; i < 8; ++i) { observed |= std::uint64_t{slot->packet.bytes[i]} << (8 * i); }
        check(observed == n && slot->packet.size == 8 + n % 1450 && slot->rx_ns == n * 10 &&
              slot->dropped_before == n / 1000, "Concurrent ring wraparound corrupted packet ownership/metadata");
        for (std::size_t i = 8; i < slot->packet.size; ++i) {
            check(slot->packet.bytes[i] == static_cast<std::uint8_t>(n), "Concurrent ring corrupted variable-length payload");
        }
        threaded.release();
    }
    producer.join();
}

} // namespace

int main() {
    try {
        test_book(); test_timing(); test_loss(); test_gap_queue(); test_workload(); test_ring();
        std::cout << "L2, timing, loss policies, and concurrent packet ring checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
