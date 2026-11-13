#include "low_latency/pcap.hpp"
#include "low_latency/pitch.hpp"
#include "low_latency/traffic.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

template<class Function>
void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Expected invalid input to be rejected");
}

std::uint64_t le(const std::uint8_t* data, unsigned size) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) { value |= std::uint64_t{data[i]} << (i * 8); }
    return value;
}

void test_generator() {
    low_latency::PitchGenerator generator(1, 7, 40, {"AAPL", "MSFT"});
    low_latency::PitchGenerator identical(1, 7, 40, {"AAPL", "MSFT"});
    low_latency::PitchGenerator different(2, 7, 40, {"AAPL", "MSFT"});
    struct Order { std::uint32_t quantity; std::string symbol; char side; std::uint64_t price; };
    std::map<std::uint64_t, Order> book;
    std::uint32_t sequence = 1;
    std::uint64_t last_order = 0;
    std::uint64_t last_execution = 0;
    std::map<unsigned, unsigned> types;
    for (unsigned n = 0; n < 10000; ++n) {
        const auto time = std::uint64_t{n} * 250000;
        const auto packet = generator.next(time);
        const auto copy = identical.next(time);
        const auto other = different.next(time);
        check(packet.size == copy.size && std::equal(packet.data().begin(), packet.data().end(),
              copy.data().begin()), "Same inputs must generate identical bytes");
        if (n == 0) {
            check(!std::equal(packet.data().begin(), packet.data().end(),
                  other.data().begin(), other.data().end()), "Seed must affect stream");
            const std::vector<std::uint8_t> startup{6, 0x20, 0x98, 0x85, 0, 0,
                                                   6, 0x97, 0, 0, 0, 0};
            check(std::equal(startup.begin(), startup.end(), packet.bytes.begin() + 8),
                  "Startup bytes must be Time 09:30:00 then Unit Clear");
        }
        low_latency::validate_pitch(packet.data());
        check(packet.size <= 1472, "Packet must fit 1500-byte IPv4 MTU");
        check(packet.bytes[3] == 7, "Wrong unit");
        check(le(packet.bytes.data() + 4, 4) == sequence, "Noncontiguous message sequence");
        for (unsigned i = 0; i < packet.bytes[2]; ++i) {
            sequence = sequence == UINT32_MAX ? 1 : sequence + 1;
        }
        std::size_t offset = 8;
        for (unsigned i = 0; i < packet.bytes[2]; ++i) {
            const auto* message = packet.bytes.data() + offset;
            const auto type = message[1];
            ++types[type];
            if (type == 0x20) {
                check(message[0] == 6, "Wrong BYX/EDGA Time length");
                check(le(message + 2, 4) == 34200 + time / 1'000'000'000, "Wrong Time value");
            } else {
                check(le(message + 2, 4) == time % 1'000'000'000, "Wrong nanosecond offset");
                if (type == 0x97) {
                    check(n == 0 && message[0] == 6, "Unexpected unit clear");
                    book.clear();
                } else {
                    const auto id = le(message + 6, 8);
                    if (type == 0x21) {
                        check(message[0] == 34 && id == ++last_order, "Wrong Add Order fields");
                        check(message[14] == 'B' || message[14] == 'S', "Wrong order side");
                        const auto quantity = static_cast<std::uint32_t>(le(message + 15, 4));
                        check(quantity > 0 && quantity <= 2000, "Invalid order size");
                        const std::string symbol(reinterpret_cast<const char*>(message + 19), 6);
                        check(symbol == "AAPL  " || symbol == "MSFT  ", "Wrong symbol/padding");
                        const auto mid = symbol == "AAPL  " ? 1'000'000U : 1'100'000U;
                        const auto price = le(message + 25, 8);
                        check(message[14] == 'B' ? price < mid : price > mid, "Crossed synthetic quote");
                        check(message[33] == 1, "Wrong Add flags");
                        check(book.emplace(id, Order{quantity, symbol.substr(0, 4),
                              static_cast<char>(message[14]), price}).second, "Duplicate live order ID");
                    } else {
                        const auto found = book.find(id);
                        check(found != book.end(), "Event references a missing order");
                        if (type == 0x23) {
                            check(message[0] == 26, "Wrong execution length");
                            const auto quantity = static_cast<std::uint32_t>(le(message + 14, 4));
                            check(quantity > 0 && quantity <= found->second.quantity, "Execution exceeds order size");
                            check(le(message + 18, 8) == ++last_execution, "Duplicate execution ID");
                            found->second.quantity -= quantity;
                            if (found->second.quantity == 0) { book.erase(found); }
                        } else if (type == 0x25) {
                            check(message[0] == 18, "Wrong reduction length");
                            const auto quantity = static_cast<std::uint32_t>(le(message + 14, 4));
                            check(quantity > 0 && quantity < found->second.quantity, "Invalid partial reduction");
                            found->second.quantity -= quantity;
                        } else if (type == 0x27) {
                            check(message[0] == 27 && message[26] == 1, "Wrong Modify fields");
                            found->second.quantity = static_cast<std::uint32_t>(le(message + 14, 4));
                            found->second.price = le(message + 18, 8);
                            check(found->second.quantity > 0, "Zero modified size");
                        } else {
                            check(type == 0x29 && message[0] == 14, "Unexpected event type/length");
                            book.erase(found);
                        }
                    }
                }
            }
            offset += message[0];
        }
        check(book.size() <= 120, "Generator book must remain bounded");
        if (n >= 2) { check(book.size() >= 119, "Depleted orders must be replenished"); }
        std::map<std::tuple<std::string, char, std::uint64_t>, std::pair<std::uint64_t, unsigned>> levels;
        for (const auto& [id, order] : book) {
            auto& level = levels[{order.symbol, order.side, order.price}];
            level.first += order.quantity;
            ++level.second;
            const auto mid = order.symbol == "AAPL" ? 1'000'000U : 1'100'000U;
            check(order.side == 'B' ? order.price < mid : order.price > mid, "Crossed modified price");
        }
        const auto expected = generator.book();
        check(expected.size() == levels.size(), "Wrong final L2 depth");
        for (const auto& level : expected) {
            const auto decoded = levels.at({level.symbol, level.side, level.price});
            check(decoded.first == level.quantity && decoded.second == level.orders, "L2 manifest oracle differs from wire events");
            if (n == 2) { check(level.orders == 3, "Initialization needs three orders at every level"); }
        }
        if (n == 2) { check(expected.size() == 40, "Initialization needs ten levels on both sides of both symbols"); }
    }
    check(types[0x20] == 3 && types[0x97] == 1, "Time messages should track second boundaries");
    check(types[0x21] > 1000 && types[0x23] > 1000 && types[0x29] > 1000 && types[0x25] > 1000 && types[0x27] > 1000,
          "Stream must exercise all five market event types");
    check(low_latency::advance_sequence(UINT32_MAX - 1, 4) == 3, "Rollover must skip sequence zero");
    rejects([] { low_latency::PitchGenerator invalid(0, 1, 1, {"A"}); });
    rejects([] { low_latency::PitchGenerator invalid(1, 0, 1, {"A"}); });
    rejects([] { low_latency::PitchGenerator invalid(1, 1, 41, {"A"}); });
    rejects([] { low_latency::PitchGenerator invalid(1, 1, 1, {"TOOLONG"}); });
    rejects([] { low_latency::PitchGenerator invalid(1, 1, 1, {"A B"}); });
    rejects([] { low_latency::PitchGenerator invalid(1, 1, 1, {"A", "A"}); });
    rejects([] { low_latency::PitchGenerator invalid(1, 1, 1, {"A"}, 0); });
    rejects([] { low_latency::PitchGenerator invalid(1, 1, 1, {"A"}, 100, 100); });
    rejects([&] { generator.next(52'200'000'000'000ULL); });
    rejects([&] { generator.next(UINT64_MAX); });
}


void test_traffic() {
    low_latency::TrafficSchedule schedule(8, 20, 10, 100);
    check(schedule.background_mbps() == 8, "Bursts must preserve the configured background rate");
    schedule.advance(12500);
    check(schedule.now_ns() == 5'000'000, "Wrong burst spacing");
    schedule.advance(50000);
    check(std::abs(static_cast<double>(schedule.now_ns()) - 47'500'000) <= 1, "Packet crossing burst boundary has wrong spacing");
    schedule.advance(52500);
    check(std::abs(static_cast<double>(schedule.now_ns()) - 100'000'000) <= 1, "Full cycle must carry 920000 bits at 9.2 Mbps");
    low_latency::TrafficSchedule variable(8, 20, 10, 100);
    std::uint64_t bits = 0;
    for (unsigned i = 0; i < 50000; ++i) {
        const auto bytes = 64 + i % 1400;
        variable.advance(bytes);
        bits += bytes * 8;
        const auto time = variable.now_ns();
        const auto phase = time % 100'000'000;
        const long double capacity = static_cast<long double>(time / 100'000'000) * 920'000 +
            static_cast<long double>(std::min<std::uint64_t>(phase, 10'000'000)) * 0.02L +
            static_cast<long double>(phase > 10'000'000 ? phase - 10'000'000 : 0) * 0.008L;
        check(std::abs(capacity - bits) < 0.1L, "Variable-sized packets violate the traffic budget");
    }
    low_latency::TrafficSchedule constant(8, 20, 0, 100);
    constant.advance(1000);
    check(constant.now_ns() == 1'000'000, "Constant profile rate is wrong");
    check(std::abs(schedule.cycle_average_mbps() - 9.2) < 1e-9, "Wrong derived cycle average");
    low_latency::TrafficSchedule frequent(8, 20, 50, 100);
    check(frequent.background_mbps() == 8 && frequent.cycle_average_mbps() == 14,
          "Longer bursts must raise the average without lowering background");
    frequent.advance(125000);
    check(frequent.now_ns() == 50'000'000, "Wrong long-burst duration");
    frequent.advance(50000);
    check(frequent.now_ns() == 100'000'000, "Background must remain eight Mbps after a long burst");
    rejects([] { low_latency::TrafficSchedule invalid(8, 20, 100, 100); });
    rejects([] { low_latency::TrafficSchedule invalid(0, 20, 10, 100); });
    rejects([] { low_latency::TrafficSchedule invalid(21, 20, 10, 100); });
}

void test_framing() {
    // An unknown future type and a heartbeat are both valid framing.
    std::vector<std::uint8_t> future{10, 0, 1, 1, 1, 0, 0, 0, 2, 0xff};
    low_latency::validate_pitch(future);
    const std::vector<std::uint8_t> heartbeat{8, 0, 0, 1, 1, 0, 0, 0};
    low_latency::validate_pitch(heartbeat);
    future[8] = 0;
    rejects([&] { low_latency::validate_pitch(future); });
    future[8] = 3;
    rejects([&] { low_latency::validate_pitch(future); });
    future[8] = 2;
    future[2] = 2;
    rejects([&] { low_latency::validate_pitch(future); });
    future[2] = 0;
    rejects([&] { low_latency::validate_pitch(future); });
    rejects([] { low_latency::validate_pitch({}); });
}

void test_pcap(const std::filesystem::path& directory) {
    const auto path = (directory / "roundtrip.pcap").string();
    low_latency::PitchGenerator generator(3, 1, 8, {"TEST"});
    const auto first = generator.next(0);
    const auto second = generator.next(1'000'000'123);
    low_latency::PcapWriter writer(path, "127.0.0.1", "239.255.0.1", 30001);
    writer.write(first, 0);
    writer.write(second, 1'000'000'123);
    writer.finish();
    low_latency::PcapReader reader(path, 30001);
    low_latency::Packet actual;
    unsigned record = 0;
    for (const auto& expected : {first, second}) {
        check(reader.next(actual), "Missing PCAP packet");
        check(reader.timestamp_ns() == (record++ == 0 ? 0 : 1'000'000'123), "PCAP timestamp changed");
        check(actual.size == expected.size && std::equal(actual.data().begin(), actual.data().end(),
              expected.data().begin()), "PCAP changed PITCH payload");
    }
    check(!reader.next(actual), "Extra PCAP packet");
    low_latency::PcapReader filtered(path, 12345);
    check(!filtered.next(actual), "Destination port filter failed");
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
    rejects([&] {
        low_latency::PcapReader truncated(path);
        while (truncated.next(actual)) {}
    });
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "bad";
    rejects([&] { low_latency::PcapReader invalid(path); });
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("low-latency-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(directory);
        test_generator();
        test_framing();
        test_traffic();
        test_pcap(directory);
        std::filesystem::remove_all(directory);
        std::cout << "Generator, framing, sequencing, and PCAP checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
