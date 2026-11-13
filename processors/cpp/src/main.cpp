#include "cli.hpp"
#include "low_latency/packet_ring.hpp"
#include "low_latency/processor.hpp"

#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>
#include <thread>

namespace {

std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void stop(int) { interrupted.store(true, std::memory_order_relaxed); }

struct Options {
    std::string group = "239.255.0.1";
    std::string interface = "127.0.0.1";
    std::uint16_t port = 30001;
    std::uint8_t unit = 1;
    std::size_t samples = 1000000;
    std::uint64_t warmup = 180;
    std::size_t ring_slots = 4096;
    std::size_t gap_slots = 4096;
    unsigned idle_ms = 3000;
    int receive_buffer = 4 * 1024 * 1024;
    low_latency::LossPolicy loss = low_latency::LossPolicy::stop;
    bool continuous = false;
    std::string output;
};

constexpr std::string_view help = R"(Usage: processor_cpp [options]
  --group IPv4           Multicast group (239.255.0.1)
  --interface IPv4       Receiving interface IPv4 address (127.0.0.1)
  --port N               UDP port (30001); 0 chooses an ephemeral port
  --unit N               PITCH unit to process, 1..255 (1)
  --messages N           Latency sample target after warmup (1000000)
  --warmup-messages N    Initial market messages to apply without sampling (180)
  --ring-slots N         Whole-packet SPSC ring capacity, power of two (4096)
  --gap-slots N          Reader-to-recovery gap queue capacity, power of two (4096)
  --receive-buffer N     Requested OS socket buffer bytes (4194304)
  --idle-timeout-ms N    Stop on idle after first packet, 1..60000 (3000)
  --on-loss stop|continue  Stop/invalidate benchmark or continue with partial L2 (stop)
  --continuous           Run until interrupted; keep only the first N samples
  --output PATH          Write JSON report; refuse to overwrite an existing file
  --help                 Show help

Two threads: recvmsg -> timestamped packet ring -> PITCH/L2 updates.
CLOCK_MONOTONIC timestamps RX immediately after successful recvmsg and the end
of every sampled message's L2 update. Samples include ring wait and earlier
messages in that datagram. Controls are excluded. Waits indefinitely for startup.
Continue mode drops whole packets on overflow; L2 stays incomplete until Unit
Clear resets the book. Continuous mode normally accompanies --on-loss continue.
)";

Options parse(int argc, char** argv) {
    using namespace low_latency::cli;
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (name == "--help") { std::cout << help; std::exit(0); }
        if (name == "--continuous") { options.continuous = true; continue; }
        if (i + 1 >= argc) { throw std::invalid_argument("Missing value for " + std::string(name)); }
        const std::string value = argv[++i];
        if (name == "--group") { options.group = value; }
        else if (name == "--interface") { options.interface = value; }
        else if (name == "--port") { options.port = static_cast<std::uint16_t>(integer(name, value, 65535)); }
        else if (name == "--unit") { options.unit = static_cast<std::uint8_t>(integer(name, value, 255)); }
        else if (name == "--messages") { options.samples = static_cast<std::size_t>(integer(name, value, 100'000'000)); }
        else if (name == "--warmup-messages") { options.warmup = integer(name, value, 100'000'000); }
        else if (name == "--ring-slots") { options.ring_slots = static_cast<std::size_t>(integer(name, value, 65536)); }
        else if (name == "--gap-slots") { options.gap_slots = static_cast<std::size_t>(integer(name, value, 65536)); }
        else if (name == "--receive-buffer") { options.receive_buffer = static_cast<int>(integer(name, value, 256 * 1024 * 1024)); }
        else if (name == "--idle-timeout-ms") { options.idle_ms = static_cast<unsigned>(integer(name, value, 60000)); }
        else if (name == "--output") { options.output = value; }
        else if (name == "--on-loss") {
            if (value == "stop") { options.loss = low_latency::LossPolicy::stop; }
            else if (value == "continue") { options.loss = low_latency::LossPolicy::continue_processing; }
            else { throw std::invalid_argument("--on-loss must be stop or continue"); }
        }
        else { throw std::invalid_argument("Unknown option: " + std::string(name)); }
    }
    if (!IN_MULTICAST(ntohl(address(options.group).s_addr))) { throw std::invalid_argument("Need a multicast --group"); }
    address(options.interface);
    if (options.unit == 0 || options.samples == 0 || options.idle_ms == 0 || options.receive_buffer == 0 ||
        options.ring_slots == 0 || (options.ring_slots & (options.ring_slots - 1)) != 0 ||
        options.gap_slots == 0 || (options.gap_slots & (options.gap_slots - 1)) != 0) {
        throw std::invalid_argument("Need nonzero unit/messages/timeout/buffer and power-of-two packet/gap queues");
    }
    if (!options.output.empty() && std::filesystem::exists(options.output)) {
        throw std::invalid_argument("Report already exists: " + options.output);
    }
    return options;
}

struct RxStats {
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;
    std::uint64_t first_rx = 0;
    std::uint64_t last_rx = 0;
    std::uint64_t kernel_drops = 0;
    std::uint64_t ring_drops = 0;
    std::uint64_t truncated = 0;
    std::size_t high_watermark = 0;
    std::string error; // Producer owns these fields; main reads only after join.
};

class Receiver {
public:
    enum class Status { idle, packet, truncated };
    explicit Receiver(const Options& options) {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) { fail("socket"); }
        try {
            const int enabled = 1;
            set(SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
            set(SOL_SOCKET, SO_RCVBUF, &options.receive_buffer, sizeof(options.receive_buffer));
            const auto tick = std::min(options.idle_ms, 50U);
            const timeval timeout{0, static_cast<suseconds_t>(tick) * 1000};
            set(SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#ifdef SO_RXQ_OVFL
            set(SOL_SOCKET, SO_RXQ_OVFL, &enabled, sizeof(enabled));
            drop_counter_ = true;
#endif
#ifdef IP_MULTICAST_ALL
            const int disabled = 0;
            set(IPPROTO_IP, IP_MULTICAST_ALL, &disabled, sizeof(disabled));
#endif
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = htons(options.port);
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(fd_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) { fail("bind"); }
            ip_mreq membership{};
            membership.imr_multiaddr = low_latency::cli::address(options.group);
            membership.imr_interface = low_latency::cli::address(options.interface);
            set(IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership));
            socklen_t size = sizeof(local);
            if (getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &size) < 0) { fail("getsockname"); }
            port_ = ntohs(local.sin_port);
            size = sizeof(buffer_);
            if (getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buffer_, &size) < 0) { fail("getsockopt"); }
        } catch (...) { close(fd_); throw; }
    }
    ~Receiver() { close(fd_); }
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    std::uint16_t port() const { return port_; }
    int buffer() const { return buffer_; }
    bool drop_counter() const { return drop_counter_; }

    Status receive(low_latency::ReceivedPacket& packet, RxStats& stats) {
        iovec io{packet.packet.bytes.data(), packet.packet.bytes.size()};
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(std::uint32_t))> control{};
        msghdr message{};
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        const auto received = recvmsg(fd_, &message, MSG_TRUNC);
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) { return Status::idle; }
            fail("recvmsg");
        }
        packet.rx_ns = low_latency::monotonic_ns(); // RX measurement boundary.
        packet.packet.size = static_cast<std::size_t>(received);
        if (stats.packets++ == 0) { stats.first_rx = packet.rx_ns; }
        stats.last_rx = packet.rx_ns;
        stats.bytes += static_cast<std::uint64_t>(received);
        const bool truncated = (message.msg_flags & MSG_TRUNC) != 0;
        if (truncated) { ++stats.truncated; }
        if ((message.msg_flags & MSG_CTRUNC) != 0) { throw std::runtime_error("Truncated socket control data"); }
#ifdef SO_RXQ_OVFL
        for (auto* item = CMSG_FIRSTHDR(&message); item; item = CMSG_NXTHDR(&message, item)) {
            if (item->cmsg_level == SOL_SOCKET && item->cmsg_type == SO_RXQ_OVFL &&
                item->cmsg_len >= CMSG_LEN(sizeof(std::uint32_t))) {
                std::uint32_t dropped = 0;
                std::memcpy(&dropped, CMSG_DATA(item), sizeof(dropped));
                stats.kernel_drops += static_cast<std::uint32_t>(dropped - last_dropped_);
                last_dropped_ = dropped;
            }
        }
#endif
        if (truncated) { return Status::truncated; }
        return Status::packet;
    }

private:
    [[noreturn]] static void fail(const char* operation) {
        throw std::system_error(errno, std::generic_category(), operation);
    }
    void set(int level, int option, const void* data, socklen_t size) {
        if (setsockopt(fd_, level, option, data, size) < 0) { fail("setsockopt"); }
    }
    int fd_ = -1;
    std::uint16_t port_ = 0;
    int buffer_ = 0;
    bool drop_counter_ = false;
    std::uint32_t last_dropped_ = 0;
};

struct QueueWait {
    std::uint64_t packets = 0;
    std::uint64_t maximum = 0;
    long double sum = 0;
    void record(std::uint64_t value) { ++packets; sum += value; maximum = std::max(maximum, value); }
};

std::string report(const Options& options, const low_latency::Processor& processor,
                   const RxStats& rx, const QueueWait& queue, int buffer, bool drop_counter,
                   std::uint64_t resolution, bool valid, const std::string& error,
                   const std::vector<low_latency::GapRange>& gaps) {
    const auto& stats = processor.stats();
    const auto latency = processor.latency();
    const auto book = processor.book().snapshot();
    std::ostringstream out;
    out << std::boolalpha << std::setprecision(15)
        << "{\n  \"implementation\": \"cpp-os\",\n  \"compiler\": " << std::quoted(__VERSION__)
        << ",\n  \"build_type\": " << std::quoted(PROCESSOR_BUILD_TYPE)
        << ",\n  \"receive_api\": \"recvmsg\",\n  \"consumer_wait\": \"busy_spin\""
        << ",\n  \"order_container\": \"std::unordered_map\",\n  \"level_container\": \"std::map\""
        << ",\n  \"threads\": 2,\n  \"continuous\": " << options.continuous
        << ",\n  \"loss_policy\": " << std::quoted(options.loss == low_latency::LossPolicy::stop ? "stop" : "continue")
        << ",\n  \"benchmark_valid\": " << valid << ",\n  \"complete\": " << processor.complete()
        << ",\n  \"book_current\": " << (processor.book_current() && error.empty())
        << ",\n  \"error\": " << std::quoted(error)
        << ",\n  \"clock\": \"CLOCK_MONOTONIC\",\n  \"clock_resolution_ns\": " << resolution
        << ",\n  \"timing_boundary\": \"recvmsg_return_to_message_l2_update\""
        << ",\n  \"group\": " << std::quoted(options.group) << ",\n  \"interface\": " << std::quoted(options.interface)
        << ",\n  \"unit\": " << unsigned{options.unit}
        << ",\n  \"target_samples\": " << options.samples << ",\n  \"warmup_target\": " << options.warmup
        << ",\n  \"warmup_messages\": " << stats.warmup_messages
        << ",\n  \"received_packets\": " << rx.packets << ",\n  \"processed_packets\": " << stats.packets
        << ",\n  \"other_unit_packets\": " << stats.other_unit_packets
        << ",\n  \"pitch_messages\": " << stats.pitch_messages << ",\n  \"market_messages\": " << stats.market_messages
        << ",\n  \"applied_market_messages\": " << stats.applied_market_messages
        << ",\n  \"skipped_market_messages\": " << stats.skipped_market_messages
        << ",\n  \"received_payload_bytes\": " << rx.bytes
        << ",\n  \"receive_span_ns\": " << (rx.last_rx - rx.first_rx)
        << ",\n  \"receive_buffer_requested\": " << options.receive_buffer
        << ",\n  \"receive_buffer_actual\": " << buffer
        << ",\n  \"kernel_drop_counter_available\": " << drop_counter
        << ",\n  \"kernel_dropped_packets\": " << rx.kernel_drops
        << ",\n  \"ring_slots\": " << options.ring_slots << ",\n  \"ring_high_watermark\": " << rx.high_watermark
        << ",\n  \"ring_dropped_packets\": " << rx.ring_drops << ",\n  \"truncated_packets\": " << rx.truncated
        << ",\n  \"sequence_errors\": " << stats.sequence_errors << ",\n  \"missing_messages\": " << stats.missing_messages
        << ",\n  \"joined_midstream\": " << stats.joined_midstream
        << ",\n  \"out_of_order_packets\": " << stats.out_of_order_packets
        << ",\n  \"protocol_errors\": " << stats.protocol_errors
        << ",\n  \"gap_queue_capacity\": " << options.gap_slots
        << ",\n  \"gap_ranges_enqueued\": " << stats.gap_ranges_enqueued
        << ",\n  \"gap_ranges_dropped\": " << stats.gap_ranges_dropped
        << ",\n  \"queue_wait\": {\"packets\": " << queue.packets << ", \"mean_ns\": "
        << (queue.packets ? static_cast<double>(queue.sum / queue.packets) : 0)
        << ", \"max_ns\": " << queue.maximum << "},\n  \"rx_to_l2\": {\"samples\": " << latency.samples;
    const auto field = [&](const char* name, auto value) {
        out << ", \"" << name << "\": ";
        if (latency.samples) { out << value; } else { out << "null"; }
    };
    field("min_ns", latency.minimum);
    field("p50_ns", latency.p50);
    field("p99_ns", latency.p99);
    field("p99_9_ns", latency.p999);
    field("max_ns", latency.maximum);
    field("mean_ns", latency.mean);
    out << "},\n  \"live_orders\": " << processor.book().orders() << ",\n  \"final_book\": [\n";
    for (std::size_t i = 0; i < book.size(); ++i) {
        const auto& level = book[i];
        out << "    {\"symbol\": " << std::quoted(level.symbol) << ", \"side\": \"" << level.side
            << "\", \"price_1e4\": " << level.price << ", \"quantity\": " << level.quantity
            << ", \"orders\": " << level.orders << '}' << (i + 1 == book.size() ? "\n" : ",\n");
    }
    out << "  ],\n  \"pending_gap_ranges\": [\n";
    for (std::size_t i = 0; i < gaps.size(); ++i) {
        const auto& gap = gaps[i];
        out << "    {\"unit\": " << unsigned{gap.unit} << ", \"first_sequence\": " << gap.first_sequence
            << ", \"last_sequence\": " << gap.last_sequence << ", \"message_count\": " << gap.message_count
            << ", \"detected_rx_ns\": " << gap.detected_rx_ns << '}' << (i + 1 == gaps.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        low_latency::Processor processor(options.unit, options.warmup, options.samples, options.loss,
                                          low_latency::monotonic_ns, options.gap_slots);
        low_latency::PacketRing ring(options.ring_slots);
        Receiver receiver(options);
        timespec clock_resolution{};
        if (clock_getres(CLOCK_MONOTONIC, &clock_resolution) != 0) {
            throw std::system_error(errno, std::generic_category(), "clock_getres");
        }
        const auto resolution = static_cast<std::uint64_t>(clock_resolution.tv_sec) * 1'000'000'000 +
                                static_cast<std::uint64_t>(clock_resolution.tv_nsec);
        RxStats rx;
        std::atomic<bool> finished{false};
        std::jthread producer([&](std::stop_token token) {
            try {
                low_latency::ReceivedPacket scratch;
                while (!token.stop_requested() && !interrupted.load(std::memory_order_relaxed)) {
                    auto* slot = ring.producer_slot();
                    auto& destination = slot ? *slot : scratch;
                    const auto status = receiver.receive(destination, rx);
                    if (status == Receiver::Status::idle) {
                        if (token.stop_requested() || interrupted.load(std::memory_order_relaxed)) { break; }
                        if (!options.continuous && rx.packets &&
                            low_latency::monotonic_ns() - rx.last_rx >= std::uint64_t{options.idle_ms} * 1'000'000) {
                            throw std::runtime_error("Receive idle timeout before run completion");
                        }
                        continue;
                    }
                    if (status == Receiver::Status::truncated) {
                        if (options.loss == low_latency::LossPolicy::stop) {
                            throw std::runtime_error("UDP datagram exceeds the 1472-byte feed limit");
                        }
                        continue;
                    }
                    if (rx.kernel_drops && options.loss == low_latency::LossPolicy::stop) {
                        throw std::runtime_error("Kernel socket queue reported packet loss");
                    }
                    if (!slot) {
                        slot = ring.producer_slot(); // Consumer may have freed space during recvmsg.
                        if (!slot) {
                            ++rx.ring_drops; // Drop the entire new datagram; never overwrite unread data.
                            if (options.loss == low_latency::LossPolicy::stop) {
                                throw std::runtime_error("Packet ring overflow");
                            }
                            continue;
                        }
                        *slot = scratch;
                    }
                    slot->dropped_before = rx.kernel_drops + rx.ring_drops + rx.truncated;
                    rx.high_watermark = std::max(rx.high_watermark, ring.publish());
                }
            } catch (const std::exception& error) { rx.error = error.what(); }
            finished.store(true, std::memory_order_release);
        });
        std::cout << "READY port=" << receiver.port() << " group=" << options.group
                  << " interface=" << options.interface << std::endl;
        QueueWait queue;
        std::uint64_t observed_drops = 0;
        std::string error;
        try {
            while (!interrupted.load(std::memory_order_relaxed) && (options.continuous || !processor.complete())) {
                auto* packet = ring.consumer_slot();
                if (!packet) {
                    if (finished.load(std::memory_order_acquire)) {
                        packet = ring.consumer_slot(); // Recheck after observing the producer's final publish.
                        if (!packet) { break; }
                    } else { continue; } // Busy-wait consumer: no sleep/wake jitter in the baseline.
                }
                const auto dequeue = low_latency::monotonic_ns();
                queue.record(dequeue - packet->rx_ns);
                if (packet->dropped_before != observed_drops) {
                    processor.note_loss();
                    observed_drops = packet->dropped_before;
                }
                try { processor.process(packet->packet.data(), packet->rx_ns); }
                catch (...) { ring.release(); throw; }
                ring.release();
            }
        } catch (const std::exception& failure) { error = failure.what(); }
        producer.request_stop();
        producer.join();
        if (observed_drops != rx.kernel_drops + rx.ring_drops + rx.truncated) { processor.note_loss(); }
        if (error.empty()) { error = rx.error; }
        const bool cancelled = interrupted.load(std::memory_order_relaxed);
        if (!options.continuous && cancelled && error.empty()) { error = "Interrupted before benchmark completion"; }
        const bool valid = !options.continuous && error.empty() && processor.complete() &&
                           processor.book_current() && !processor.loss_seen() &&
                           processor.stats().sequence_errors == 0;
        // The baseline has no recovery worker. Drain only at shutdown for reporting.
        std::vector<low_latency::GapRange> gaps;
        low_latency::GapRange gap;
        while (processor.pop_gap(gap)) { gaps.push_back(gap); }
        const auto json = report(options, processor, rx, queue, receiver.buffer(), receiver.drop_counter(), resolution, valid, error, gaps);
        if (!options.output.empty()) {
            std::ofstream file;
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file.open(options.output);
            file << json;
            file.close();
        } else { std::cout << json; }
        const auto latency = processor.latency();
        std::cout << "samples=" << latency.samples << " p50_ns=" << latency.p50 << " p99_ns=" << latency.p99
                  << " p99.9_ns=" << latency.p999 << " benchmark_valid=" << (valid ? "true" : "false")
                  << " book_current=" << (processor.book_current() && error.empty() ? "true" : "false") << '\n';
        std::cout << "received_packets=" << rx.packets << " kernel_drops=" << rx.kernel_drops
                  << " ring_drops=" << rx.ring_drops << " missing_messages=" << processor.stats().missing_messages
                  << " skipped_messages=" << processor.stats().skipped_market_messages << '\n';
        std::cout << "gap_ranges_enqueued=" << processor.stats().gap_ranges_enqueued
                  << " gap_ranges_dropped=" << processor.stats().gap_ranges_dropped << '\n';
        std::cout << "L2 snapshot (" << (processor.book_current() && error.empty() ? "current" : "incomplete")
                  << ")\nSYMBOL SIDE PRICE QUANTITY ORDERS\n";
        for (const auto& level : processor.book().snapshot()) {
            std::cout << level.symbol << ' ' << level.side << ' ' << level.price / 10000 << '.'
                      << std::setw(4) << std::setfill('0') << level.price % 10000 << std::setfill(' ')
                      << ' ' << level.quantity << ' ' << level.orders << '\n';
        }
        if (!error.empty()) { std::cerr << "processor_cpp: " << error << '\n'; }
        return (valid || (options.continuous && cancelled && error.empty())) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "processor_cpp: " << error.what() << '\n';
        return 1;
    }
}
