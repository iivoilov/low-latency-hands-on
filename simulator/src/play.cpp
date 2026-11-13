#include "cli.hpp"
#include "low_latency/pcap.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <system_error>
#include <thread>

namespace {

volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }

struct Options {
    std::string input;
    std::string group = "239.255.0.1";
    std::string interface = "127.0.0.1";
    std::uint16_t port = 30001;
    std::uint16_t filter_port = 0;
    unsigned ttl = 1;
    bool send = true;
    bool spin = false;
};

constexpr std::string_view help = R"(Usage: play_feed FILE.pcap [options]
  --group IPv4          Destination multicast group (239.255.0.1)
  --port N              Destination UDP port (30001)
  --interface IPv4      Local interface IPv4 address (127.0.0.1)
  --ttl N               Multicast TTL, 0..255 (1); loopback enabled
  --filter-port N       Select original UDP destination port in the PCAP
  --spin                Busy-wait throughout; default sleeps for long gaps
  --no-send             Load/validate and report schedule without networking
  --help                Show help

Preloads the entire PCAP, then sends payloads unchanged at recorded relative times.
On scheduling delays, shifts later deadlines to avoid catch-up bursts. Reports
actual load, maximum lateness, and accumulated schedule slip after playback.
)";

Options parse(int argc, char** argv) {
    using namespace low_latency::cli;
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (name == "--help") { std::cout << help; std::exit(0); }
        if (name == "--no-send") { options.send = false; continue; }
        if (name == "--spin") { options.spin = true; continue; }
        if (!name.starts_with("--")) {
            if (!options.input.empty()) { throw std::invalid_argument("Only one play file is allowed"); }
            options.input = name;
            continue;
        }
        if (i + 1 >= argc) { throw std::invalid_argument("Missing value for " + std::string(name)); }
        const std::string value = argv[++i];
        if (name == "--group") { options.group = value; }
        else if (name == "--interface") { options.interface = value; }
        else if (name == "--port") { options.port = static_cast<std::uint16_t>(integer(name, value, 65535)); }
        else if (name == "--filter-port") { options.filter_port = static_cast<std::uint16_t>(integer(name, value, 65535)); }
        else if (name == "--ttl") { options.ttl = static_cast<unsigned>(integer(name, value, 255)); }
        else { throw std::invalid_argument("Unknown option: " + std::string(name)); }
    }
    if (options.input.empty()) { throw std::invalid_argument("A PCAP play file is required"); }
    if (!IN_MULTICAST(ntohl(address(options.group).s_addr)) || options.port == 0) {
        throw std::invalid_argument("Need a multicast --group and a nonzero --port");
    }
    address(options.interface);
    return options;
}

class Sender {
public:
    explicit Sender(const Options& options) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) { fail("socket"); }
        try {
            const auto interface = low_latency::cli::address(options.interface);
            const auto ttl = static_cast<unsigned char>(options.ttl);
            const unsigned char loopback = 1;
            if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &interface, sizeof(interface)) < 0 ||
                setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0 ||
                setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback, sizeof(loopback)) < 0) {
                fail("setsockopt multicast");
            }
            destination_.sin_family = AF_INET;
            destination_.sin_port = htons(options.port);
            destination_.sin_addr = low_latency::cli::address(options.group);
        } catch (...) { close(fd_); throw; }
    }
    ~Sender() { close(fd_); }
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    bool send(const low_latency::Packet& packet) {
        ssize_t sent;
        do {
            sent = sendto(fd_, packet.bytes.data(), packet.size, 0,
                          reinterpret_cast<const sockaddr*>(&destination_), sizeof(destination_));
        } while (sent < 0 && errno == EINTR && !stopped);
        if (sent < 0 && errno == EINTR && stopped) { return false; }
        if (sent < 0) { fail("sendto"); }
        if (static_cast<std::size_t>(sent) != packet.size) { throw std::runtime_error("Partial UDP send"); }
        return true;
    }

private:
    [[noreturn]] static void fail(const char* operation) {
        throw std::system_error(errno, std::generic_category(), operation);
    }
    int fd_ = -1;
    sockaddr_in destination_{};
};

struct TimedPacket {
    std::uint64_t time_ns;
    low_latency::Packet packet;
};

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        low_latency::PcapReader reader(options.input, options.filter_port);
        std::vector<TimedPacket> stream;
        std::uint64_t origin = 0;
        std::uint64_t bytes = 0;
        std::uint64_t messages = 0;
        low_latency::Packet packet;
        while (!stopped && reader.next(packet)) {
            const auto timestamp = reader.timestamp_ns();
            if (stream.empty()) { origin = timestamp; }
            if (timestamp < origin || (!stream.empty() && timestamp - origin < stream.back().time_ns)) {
                throw std::runtime_error("PCAP timestamps must be nondecreasing");
            }
            // Also bounds steady_clock deadline arithmetic and accidental very long runs.
            if (timestamp - origin >= 86'400'000'000'000ULL) {
                throw std::runtime_error("Play-file duration must be shorter than one day");
            }
            stream.push_back({timestamp - origin, packet});
            bytes += packet.size;
            messages += packet.bytes[2];
        }
        if (stopped) { return 0; }
        if (stream.empty()) { throw std::runtime_error("No matching PITCH UDP packets in capture"); }
        const auto duration = stream.back().time_ns;
        std::cout << "loaded_packets=" << stream.size() << " messages=" << messages
                  << " payload_bytes=" << bytes << " scheduled_seconds=" << static_cast<double>(duration) / 1e9
                  << " planned_mbps=" << (duration ? static_cast<double>(bytes) * 8000 / static_cast<double>(duration) : 0)
                  << " network=" << (options.send ? "on" : "off") << '\n';
        if (!options.send) { return 0; }
        Sender sender(options);
        using Clock = std::chrono::steady_clock;
        std::uint64_t sent = 0;
        std::uint64_t sent_bytes = 0;
        std::chrono::nanoseconds slip{};
        std::chrono::nanoseconds maximum_lateness{};
        const auto start = Clock::now();
        for (const auto& item : stream) {
            const auto deadline = start + std::chrono::nanoseconds(item.time_ns) + slip;
            auto now = Clock::now();
            while (!stopped && now < deadline) {
                if (!options.spin && deadline - now > std::chrono::microseconds(200)) {
                    std::this_thread::sleep_until(std::min(deadline - std::chrono::microseconds(100),
                                                          now + std::chrono::milliseconds(10)));
                }
                now = Clock::now();
            }
            if (stopped) { break; }
            const auto lateness = std::chrono::duration_cast<std::chrono::nanoseconds>(now - deadline);
            maximum_lateness = std::max(maximum_lateness, lateness);
            slip += lateness;
            if (!sender.send(item.packet)) { break; }
            ++sent;
            sent_bytes += item.packet.size;
        }
        const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
        std::cout << "sent_packets=" << sent << " elapsed_seconds=" << seconds
                  << " actual_mbps=" << (seconds > 0 ? static_cast<double>(sent_bytes) * 8 / seconds / 1e6 : 0)
                  << " max_lateness_ns=" << maximum_lateness.count() << " schedule_slip_ns=" << slip.count() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "play_feed: " << error.what() << '\n';
        return 1;
    }
}
