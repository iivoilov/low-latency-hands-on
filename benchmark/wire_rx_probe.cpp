// Linux UDP timestamp probe. Hardware clocks are read-only and never adjusted.
#include "cli.hpp"
#include "low_latency/processor.hpp"
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <linux/ptp_clock.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> interrupted{false};
void signal_stop(int) { interrupted.store(true, std::memory_order_relaxed); }
std::uint64_t now(clockid_t clock) {
    timespec t{};
    if (clock_gettime(clock, &t)) { throw std::system_error(errno, std::generic_category(), "clock_gettime"); }
    return static_cast<std::uint64_t>(t.tv_sec) * 1'000'000'000 + static_cast<std::uint64_t>(t.tv_nsec);
}
void pin(unsigned cpu) {
    if (cpu >= CPU_SETSIZE) { throw std::runtime_error("CPU outside cpu_set_t"); }
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    const int result = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (result) { throw std::system_error(result, std::generic_category(), "pthread_setaffinity_np"); }
}
struct Fd {
    int value;
    explicit Fd(int value_) : value(value_) { if (value < 0) { throw std::system_error(errno, std::generic_category(), "open/socket"); } }
    ~Fd() { close(value); }
    Fd(const Fd&) = delete;
};
struct Options {
    std::string interface = "127.0.0.1", group = "239.255.0.1", phc, bond, prefix;
    unsigned port = 0, packets = 125024, rx_cpu = 2, calibration_cpu = 15;
    bool software_only = false;
};
Options options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--help") {
            std::cout << "wire_rx_probe --output-prefix PATH [--interface IPv4] [--group IPv4] [--port N] [--packets N] [--phc /dev/ptpN | --software-only] [--bond bond0] [--rx-cpu N] [--calibration-cpu N]\n";
            std::exit(0);
        }
        if (key == "--software-only") { o.software_only = true; continue; }
        if (++i == argc) { throw std::runtime_error("Missing option value"); }
        const std::string v = argv[i];
        if (key == "--interface") { o.interface = v; }
        else if (key == "--group") { o.group = v; }
        else if (key == "--phc") { o.phc = v; }
        else if (key == "--bond") { o.bond = v; }
        else if (key == "--output-prefix") { o.prefix = v; }
        else if (key == "--port") { o.port = static_cast<unsigned>(low_latency::cli::integer(key, v, 65535)); }
        else if (key == "--packets") { o.packets = static_cast<unsigned>(low_latency::cli::integer(key, v, 20'000'000)); }
        else if (key == "--rx-cpu" || key == "--calibration-cpu") {
            auto cpu = static_cast<unsigned>(low_latency::cli::integer(key, v, CPU_SETSIZE - 1));
            if (key == "--rx-cpu") { o.rx_cpu = cpu; } else { o.calibration_cpu = cpu; }
        } else { throw std::runtime_error("Unknown option " + key); }
    }
    if (o.prefix.empty() || o.packets == 0 || (!o.software_only && o.phc.empty()) || (o.software_only && !o.phc.empty())) { throw std::runtime_error("Need output, packet count, and exactly one PHC/software mode"); }
    if (!IN_MULTICAST(ntohl(low_latency::cli::address(o.group).s_addr))) { throw std::runtime_error("Need multicast group"); }
    low_latency::cli::address(o.interface);
    for (const auto* suffix : {"-packets.csv", "-clocks.csv", "-capture.json"}) {
        if (std::filesystem::exists(o.prefix + suffix)) { throw std::runtime_error("Output already exists"); }
    }
    return o;
}
std::string active_slave(const std::string& bond) {
    if (bond.empty()) { return {}; }
    if (bond.find('/') != std::string::npos) { throw std::runtime_error("Invalid bond name"); }
    std::ifstream input("/sys/class/net/" + bond + "/bonding/active_slave");
    std::string value;
    if (!(input >> value)) { throw std::runtime_error("Cannot read bond active slave"); }
    return value;
}
struct Anchor { std::uint64_t hardware = 0, mono_before = 0, mono_after = 0; };
Anchor sample_clock(clockid_t phc) {
    Anchor best;
    auto width = std::numeric_limits<std::uint64_t>::max();
    for (unsigned i = 0; i < 25; ++i) {
        Anchor a;
        a.mono_before = now(CLOCK_MONOTONIC);
        a.hardware = now(phc);
        a.mono_after = now(CLOCK_MONOTONIC);
        if (a.mono_after - a.mono_before < width) { best = a; width = a.mono_after - a.mono_before; }
    }
    return best;
}
struct PacketStamp {
    std::uint64_t user_mono, user_real, mono_after_real, hardware, software;
    std::uint32_t sequence;
    unsigned count, length;
};
std::uint64_t timestamp(const __kernel_timespec& t) {
    return static_cast<std::uint64_t>(t.tv_sec) * 1'000'000'000 + static_cast<std::uint64_t>(t.tv_nsec);
}
int run(const Options& o) {
    std::signal(SIGINT, signal_stop); std::signal(SIGTERM, signal_stop);
    Fd socket(::socket(AF_INET, SOCK_DGRAM, 0));
    auto set = [&](int level, int name, const auto& v) {
        if (setsockopt(socket.value, level, name, &v, sizeof(v))) { throw std::system_error(errno, std::generic_category(), "setsockopt"); }
    };
    set(SOL_SOCKET, SO_REUSEADDR, 1);
    set(SOL_SOCKET, SO_RCVBUF, 4 * 1024 * 1024);
    set(SOL_SOCKET, SO_RCVTIMEO, timeval{1, 0});
    set(SOL_SOCKET, SO_RXQ_OVFL, 1);
    set(IPPROTO_IP, IP_MULTICAST_ALL, 0);
    const int flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE |
                      (o.software_only ? 0 : SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE);
    set(SOL_SOCKET, SO_TIMESTAMPING_NEW, flags);
    sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(static_cast<std::uint16_t>(o.port));
    if (bind(socket.value, reinterpret_cast<sockaddr*>(&local), sizeof(local))) { throw std::system_error(errno, std::generic_category(), "bind"); }
    ip_mreq membership{low_latency::cli::address(o.group), low_latency::cli::address(o.interface)};
    set(IPPROTO_IP, IP_ADD_MEMBERSHIP, membership);
    socklen_t length = sizeof(local);
    if (getsockname(socket.value, reinterpret_cast<sockaddr*>(&local), &length)) { throw std::runtime_error("getsockname failed"); }
    int actual_buffer = 0; length = sizeof(actual_buffer);
    if (getsockopt(socket.value, SOL_SOCKET, SO_RCVBUF, &actual_buffer, &length)) { throw std::runtime_error("getsockopt failed"); }
    std::vector<PacketStamp> stamps; stamps.reserve(o.packets);
    std::vector<Anchor> anchors; anchors.reserve(10000);
    const std::string initial_slave = active_slave(o.bond);
    std::string calibration_error;
    std::atomic<bool> stop{false};
    std::promise<void> ready;
    auto initial = ready.get_future();
    // Open the PHC read-only. Nothing in this process adjusts either clock.
    std::unique_ptr<Fd> phc;
    std::jthread calibrator;
    if (!o.software_only) {
        phc = std::make_unique<Fd>(open(o.phc.c_str(), O_RDONLY));
        const clockid_t clock = static_cast<clockid_t>((~static_cast<unsigned>(phc->value) << 3) | 3);
        calibrator = std::jthread([&, clock](std::stop_token token) {
            bool announced = false;
            try {
                pin(o.calibration_cpu);
                anchors.push_back(sample_clock(clock));
                ready.set_value(); announced = true;
                while (!token.stop_requested()) {
                    for (unsigned i = 0; i < 10 && !stop.load(std::memory_order_relaxed) && !token.stop_requested(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
                    if (active_slave(o.bond) != initial_slave) { throw std::runtime_error("Bond active slave changed during capture"); }
                    anchors.push_back(sample_clock(clock));
                    if (stop.load(std::memory_order_relaxed)) { break; }
                }
            } catch (const std::exception& failure) {
                if (!announced) { ready.set_exception(std::current_exception()); }
                else { calibration_error = failure.what(); }
            }
        });
        initial.get();
    }
    pin(o.rx_cpu);
    std::cout << "READY port=" << ntohs(local.sin_port) << " mode=" << (o.software_only ? "software" : "hardware") << std::endl;
    std::uint64_t sequence_errors = 0, missing_messages = 0, pitch_messages = 0;
    std::uint64_t hardware_count = 0, software_count = 0;
    std::uint32_t expected = 1, dropped = 0;
    std::string error;
    low_latency::Packet payload;
    try {
        while (stamps.size() < o.packets && !interrupted.load(std::memory_order_relaxed)) {
            iovec io{payload.bytes.data(), payload.bytes.size()};
            alignas(cmsghdr) std::array<char, 256> controls{};
            msghdr message{}; message.msg_iov = &io; message.msg_iovlen = 1;
            message.msg_control = controls.data(); message.msg_controllen = controls.size();
            auto bytes = recvmsg(socket.value, &message, MSG_TRUNC);
            if (bytes < 0) {
                if (errno == EINTR) { continue; }
                if ((errno == EAGAIN || errno == EWOULDBLOCK) && stamps.empty()) { continue; }
                throw std::system_error(errno, std::generic_category(), "recvmsg/idle timeout");
            }
            const auto user_mono = now(CLOCK_MONOTONIC); // Measurement endpoint, before ancillary parsing.
            const auto user_real = now(CLOCK_REALTIME);
            const auto mono_after_real = now(CLOCK_MONOTONIC);
            if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) { throw std::runtime_error("Truncated packet or ancillary data"); }
            payload.size = static_cast<std::size_t>(bytes);
            low_latency::validate_pitch(payload.data());
            std::uint64_t hw = 0, sw = 0;
            for (auto* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
                if (c->cmsg_level != SOL_SOCKET) { continue; }
                if (c->cmsg_type == SO_TIMESTAMPING_NEW && c->cmsg_len >= CMSG_LEN(sizeof(scm_timestamping64))) {
                    scm_timestamping64 ts{}; std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
                    sw = timestamp(ts.ts[0]); hw = timestamp(ts.ts[2]);
                } else if (c->cmsg_type == SO_RXQ_OVFL && c->cmsg_len >= CMSG_LEN(sizeof(dropped))) {
                    std::memcpy(&dropped, CMSG_DATA(c), sizeof(dropped));
                }
            }
            std::uint32_t sequence = 0;
            for (unsigned i = 0; i < 4; ++i) {
                sequence |= static_cast<std::uint32_t>(payload.bytes[4 + i]) << (8 * i);
            }
            const unsigned count = payload.bytes[2];
            if (sequence != expected) {
                ++sequence_errors;
                const auto distance = sequence >= expected ? std::uint64_t{sequence} - expected : std::uint64_t{UINT32_MAX} - expected + sequence;
                if (sequence != 0 && distance <= UINT32_MAX / 2) { missing_messages += distance; }
            }
            if (sequence == 0) { throw std::runtime_error("Invalid sequence zero"); }
            expected = low_latency::advance_sequence(sequence, static_cast<std::uint8_t>(count));
            pitch_messages += count;
            hardware_count += hw != 0; software_count += sw != 0;
            stamps.push_back({user_mono, user_real, mono_after_real, hw, sw, sequence, count, static_cast<unsigned>(bytes)});
            if (count == 0) { break; } // Generated play files end with a heartbeat.
        }
    } catch (const std::exception& e) { error = e.what(); }
    stop.store(true, std::memory_order_relaxed);
    if (calibrator.joinable()) { calibrator.join(); }
    if (error.empty() && !calibration_error.empty()) { error = calibration_error; }
    const auto final_slave = active_slave(o.bond);
    if (initial_slave != final_slave) { error = "Bond active slave changed"; }
    bool valid = error.empty() && !interrupted.load(std::memory_order_relaxed) && stamps.size() == o.packets && dropped == 0 && sequence_errors == 0 && software_count == stamps.size() && (o.software_only || hardware_count == stamps.size());
    std::ofstream packets(o.prefix + "-packets.csv");
    packets << "user_monotonic_ns,user_realtime_ns,monotonic_after_realtime_ns,hardware_phc_ns,software_realtime_ns,sequence,messages,payload_bytes\n";
    for (const auto& s : stamps) { packets << s.user_mono << ',' << s.user_real << ',' << s.mono_after_real << ',' << s.hardware << ',' << s.software << ',' << s.sequence << ',' << s.count << ',' << s.length << '\n'; }
    std::ofstream clocks(o.prefix + "-clocks.csv"); clocks << "hardware_phc_ns,monotonic_before_ns,monotonic_after_ns\n";
    for (const auto& a : anchors) { clocks << a.hardware << ',' << a.mono_before << ',' << a.mono_after << '\n'; }
    std::ofstream report(o.prefix + "-capture.json");
    report << std::boolalpha << "{\n\"capture_valid\":" << valid << ",\n\"mode\":" << std::quoted(o.software_only ? "software" : "hardware")
           << ",\n\"packets\":" << stamps.size() << ",\n\"expected_packets\":" << o.packets << ",\n\"pitch_messages\":" << pitch_messages
           << ",\n\"hardware_timestamps\":" << hardware_count << ",\n\"software_timestamps\":" << software_count << ",\n\"kernel_drops\":" << dropped
           << ",\n\"sequence_errors\":" << sequence_errors << ",\n\"missing_messages\":" << missing_messages
           << ",\n\"receive_buffer_actual\":" << actual_buffer << ",\n\"rx_cpu\":" << o.rx_cpu << ",\n\"calibration_cpu\":" << o.calibration_cpu
           << ",\n\"phc\":" << std::quoted(o.phc) << ",\n\"active_slave_before\":" << std::quoted(initial_slave) << ",\n\"active_slave_after\":" << std::quoted(final_slave)
           << ",\n\"calibration_anchors\":" << anchors.size() << ",\n\"error\":" << std::quoted(error) << "\n}\n";
    packets.close(); clocks.close(); report.close();
    if (!packets || !clocks || !report) { throw std::runtime_error("Writing capture failed"); }
    std::cout << "packets=" << stamps.size() << " hardware_timestamps=" << hardware_count << " software_timestamps=" << software_count << " valid=" << valid << '\n';
    return valid ? 0 : 1;
}
}
int main(int argc, char** argv) { try { return run(options(argc, argv)); } catch (const std::exception& e) { std::cerr << "wire_rx_probe: " << e.what() << '\n'; return 1; } }
