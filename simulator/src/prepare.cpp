#include "cli.hpp"
#include "low_latency/pcap.hpp"
#include "low_latency/traffic.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

struct Options {
    std::filesystem::path directory = "simulator/play_files";
    std::uint64_t messages = 1000000;
    double background = 8;
    double peak = 20;
    unsigned burst_ms = 10;
    unsigned period_ms = 100;
    unsigned levels = 10;
    unsigned orders = 3;
    unsigned events = 8;
    std::uint64_t seed = 42;
    std::uint8_t unit = 1;
    std::vector<std::string> symbols{"AAPL", "MSFT", "SPY"};
};

constexpr std::string_view help = R"(Usage: prepare_feed [options]
  --directory PATH      Play-file directory (simulator/play_files)
  --messages N          Market updates AFTER book initialization (1000000)
  --background-mbps N      Normal traffic PITCH UDP payload Mbps (8)
  --burst-mbps N        Peak payload Mbps during bursts (20)
  --burst-ms N          Burst duration in milliseconds; 0 = constant load (10)
  --burst-every-ms N    Burst start-to-start period in milliseconds (100)
  --levels N            Initial price levels per side/symbol, 1..100 (10)
  --orders-per-level N  Initial orders at each level, 1..100 (3)
  --events N            Maximum market events per UDP datagram, 1..40 (8)
  --symbols A,B,C        Unique symbols, 1..6 ASCII bytes (AAPL,MSFT,SPY)
  --seed N              Nonzero deterministic seed (42)
  --unit N              PITCH unit, 1..255 (1)
  --help                Show help

Writes a parameter-named PITCH PCAP and JSON manifest; never sends packets.
Existing files are not overwritten. Bursts begin at time zero each period.
Background stays fixed between bursts. Cycle and finite-file averages are derived.
)";

Options parse(int argc, char** argv) {
    using namespace low_latency::cli;
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (name == "--help") { std::cout << help; std::exit(0); }
        if (i + 1 >= argc) { throw std::invalid_argument("Missing value for " + std::string(name)); }
        const std::string value = argv[++i];
        if (name == "--directory") { options.directory = value; }
        else if (name == "--messages") { options.messages = integer(name, value, 100'000'000); }
        else if (name == "--background-mbps") { options.background = decimal(name, value); }
        else if (name == "--burst-mbps") { options.peak = decimal(name, value); }
        else if (name == "--burst-ms") { options.burst_ms = static_cast<unsigned>(integer(name, value, 60000)); }
        else if (name == "--burst-every-ms") { options.period_ms = static_cast<unsigned>(integer(name, value, 60000)); }
        else if (name == "--levels") { options.levels = static_cast<unsigned>(integer(name, value, 100)); }
        else if (name == "--orders-per-level") { options.orders = static_cast<unsigned>(integer(name, value, 100)); }
        else if (name == "--events") { options.events = static_cast<unsigned>(integer(name, value, 40)); }
        else if (name == "--symbols") { options.symbols = symbols(value); }
        else if (name == "--seed") { options.seed = integer(name, value, UINT64_MAX); }
        else if (name == "--unit") { options.unit = static_cast<std::uint8_t>(integer(name, value, 255)); }
        else { throw std::invalid_argument("Unknown option: " + std::string(name)); }
    }
    if (options.messages == 0 || options.directory.empty()) {
        throw std::invalid_argument("Need nonzero --messages and a nonempty --directory");
    }
    return options;
}

std::string basename(const Options& options) {
    const auto rate = [](double value) {
        char buffer[64];
        const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (error != std::errc{}) { throw std::runtime_error("Cannot format rate"); }
        return std::string(buffer, end);
    };
    std::ostringstream name;
    name << "pitch-v2-";
    if (options.messages % 1'000'000 == 0) { name << options.messages / 1'000'000 << 'm'; }
    else if (options.messages % 1000 == 0) { name << options.messages / 1000 << 'k'; }
    else { name << options.messages; }
    name << "-base" << rate(options.background) << "m-peak" << rate(options.peak)
         << "m-b" << options.burst_ms << "ms-e" << options.period_ms
         << "ms_sym" << options.symbols.size() << "_seed" << options.seed;
    return name.str();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        low_latency::TrafficSchedule schedule(options.background, options.peak, options.burst_ms, options.period_ms);
        low_latency::PitchGenerator generator(options.seed, options.unit, options.events, options.symbols,
                                              options.levels, options.orders);
        const auto name = basename(options);
        std::filesystem::create_directories(options.directory);
        const auto capture = options.directory / (name + ".pcap");
        const auto manifest = options.directory / (name + ".json");
        const auto partial = options.directory / (name + ".pcap.partial");
        if (std::filesystem::exists(capture) || std::filesystem::exists(manifest) || std::filesystem::exists(partial)) {
            throw std::runtime_error("Play file already exists: " + capture.string());
        }
        std::uint64_t packets = 0;
        std::uint64_t messages = 0;
        std::uint64_t bytes = 0;
        std::uint64_t initialization_packets = 0;
        low_latency::Packet last;
        try {
            low_latency::PcapWriter writer(partial.string(), "127.0.0.1", "239.255.0.1", 30001);
            auto generate = [&](std::uint64_t remaining) {
                while (remaining != 0) {
                    const auto events = static_cast<unsigned>(std::min<std::uint64_t>(remaining, options.events));
                    last = generator.next(schedule.now_ns(), events);
                    writer.write(last, schedule.now_ns());
                    messages += last.bytes[2];
                    bytes += last.size;
                    ++packets;
                    schedule.advance(last.size);
                    remaining -= events;
                }
            };
            generate(generator.initial_orders());
            initialization_packets = packets;
            generate(options.messages);
            // Final heartbeat marks the end of the last packet's rate budget.
            const auto sequence = std::uint32_t{last.bytes[4]} | (std::uint32_t{last.bytes[5]} << 8) |
                                  (std::uint32_t{last.bytes[6]} << 16) | (std::uint32_t{last.bytes[7]} << 24);
            const auto next = low_latency::advance_sequence(sequence, last.bytes[2]);
            low_latency::Packet heartbeat;
            heartbeat.size = 8;
            heartbeat.bytes[0] = 8;
            heartbeat.bytes[3] = options.unit;
            for (unsigned i = 0; i < 4; ++i) { heartbeat.bytes[4 + i] = static_cast<std::uint8_t>(next >> (8 * i)); }
            schedule.advance(8);
            writer.write(heartbeat, schedule.now_ns());
            ++packets;
            bytes += 8;
            writer.finish();
            std::ofstream json;
            json.exceptions(std::ios::badbit | std::ios::failbit);
            json.open(manifest);
            json << std::setprecision(15)
                 << "{\n  \"format_version\": 2,\n  \"source\": \"synthetic_l2\",\n  \"pcap\": "
                 << std::quoted(capture.filename().string())
                 << ",\n  \"rate_basis\": \"pitch_udp_payload_bits\",\n  \"background_mbps\": " << options.background
                 << ",\n  \"burst_mbps\": " << options.peak
                 << ",\n  \"cycle_average_mbps\": " << schedule.cycle_average_mbps()
                 << ",\n  \"burst_ms\": " << options.burst_ms
                 << ",\n  \"burst_every_ms\": " << options.period_ms
                 << ",\n  \"updates\": " << options.messages
                 << ",\n  \"initialization_messages\": " << generator.initial_orders()
                 << ",\n  \"initialization_packets\": " << initialization_packets
                 << ",\n  \"market_messages\": " << options.messages + generator.initial_orders()
                 << ",\n  \"pitch_messages\": " << messages
                 << ",\n  \"packets\": " << packets << ",\n  \"payload_bytes\": " << bytes
                 << ",\n  \"duration_ns\": " << schedule.now_ns()
                 << ",\n  \"finite_average_mbps\": " << static_cast<double>(bytes) * 8000 / static_cast<double>(schedule.now_ns())
                 << ",\n  \"levels_per_side\": " << options.levels
                 << ",\n  \"orders_per_level\": " << options.orders
                 << ",\n  \"events_per_packet\": " << options.events
                 << ",\n  \"unit\": " << unsigned{options.unit}
                 << ",\n  \"seed\": " << options.seed << ",\n  \"symbols\": [";
            for (std::size_t i = 0; i < options.symbols.size(); ++i) {
                if (i != 0) { json << ", "; }
                json << std::quoted(options.symbols[i]);
            }
            json << "],\n  \"final_book\": [\n";
            const auto book = generator.book();
            for (std::size_t i = 0; i < book.size(); ++i) {
                const auto& level = book[i];
                json << "    {\"symbol\": " << std::quoted(level.symbol) << ", \"side\": \"" << level.side
                     << "\", \"price_1e4\": " << level.price << ", \"quantity\": " << level.quantity
                     << ", \"orders\": " << level.orders << '}' << (i + 1 == book.size() ? "\n" : ",\n");
            }
            json << "  ]\n}\n";
            json.close();
            std::filesystem::rename(partial, capture);
        } catch (...) {
            std::filesystem::remove(partial);
            std::filesystem::remove(manifest);
            throw;
        }
        std::cout << "pcap=" << capture.string() << "\nmanifest=" << manifest.string()
                  << "\nupdates=" << options.messages << " initialization_messages=" << generator.initial_orders()
                  << " pitch_messages=" << messages << " packets=" << packets << " duration_seconds="
                  << static_cast<double>(schedule.now_ns()) / 1e9 << " finite_average_mbps="
                  << static_cast<double>(bytes) * 8000 / static_cast<double>(schedule.now_ns()) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "prepare_feed: " << error.what() << '\n';
        return 1;
    }
}
