#pragma once

// Optional human-facing telemetry. Called only by the book-owning thread,
// outside process(); never enabled for the default benchmark path.
#include "low_latency/processor.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <system_error>

namespace low_latency {
class Monitor {
public:
    explicit Monitor(std::uint16_t port) {
        destination_.sin_family = AF_INET;
        destination_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        destination_.sin_port = htons(port);
        fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) { throw std::system_error(errno, std::generic_category(), "monitor socket"); }
        session_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
    ~Monitor() { close(fd_); }
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    void heartbeat(const Processor& processor) {
        const auto now = monotonic_ns();
        if (now - heartbeat_ >= 1'000'000'000) {
            heartbeat_ = now;
            publish(processor, false, true);
        }
    }
    void publish(const Processor& processor, bool failed = false, bool force = false) {
        std::map<std::string, std::pair<std::string, std::string>> levels;
        for (const auto& level : processor.book().snapshot()) {
            auto& side = level.side == 'B' ? levels[level.symbol].first : levels[level.symbol].second;
            if (!side.empty()) { side += ','; }
            side += "{\"price_1e4\":\"" + std::to_string(level.price) +
                "\",\"quantity\":\"" + std::to_string(level.quantity) +
                "\",\"orders\":" + std::to_string(level.orders) + '}';
        }
        std::map<std::string, std::string> current;
        for (const auto& [symbol, state] : processor.l1().symbols()) {
            std::string name(symbol.begin(), symbol.end());
            name.erase(name.find_last_not_of(' ') + 1);
            const auto& l = state.l1;
            std::ostringstream out;
            out << std::boolalpha << "{\"symbol\":" << quoted(name)
                << ",\"book_current\":" << (processor.book_current() && !failed)
                << ",\"trade_history_current\":" << processor.l1().trade_history_current()
                << ",\"loss_seen\":" << processor.loss_seen() << ",\"l1\":{\"bid\":";
            quote(out, l.bid); out << ",\"ask\":"; quote(out, l.ask);
            out << ",\"last\":";
            if (l.last) {
                out << "{\"execution_id\":\"" << l.last->execution_id << "\",\"price_1e4\":\""
                    << l.last->price << "\",\"quantity\":\"" << l.last->quantity
                    << "\",\"trade_date\":" << l.last->trade_date << '}';
            } else { out << "null"; }
            out << ",\"trading_status\":" << quoted(std::string(1, l.trading_status))
                << ",\"reg_sho\":" << quoted(std::string(1, l.reg_sho))
                << ",\"retail\":" << quoted(std::string(1, l.retail))
                << "},\"l2\":{\"bids\":[" << levels[name].first << "],\"asks\":[" << levels[name].second << "]}}";
            current[name] = out.str();
            const auto previous = previous_.find(name);
            if (force || previous == previous_.end() || previous->second != current[name]) { send(current[name]); }
        }
        for (const auto& [name, unused] : previous_) {
            (void)unused;
            if (!current.contains(name)) { send("{\"symbol\":" + quoted(name) + ",\"removed\":true}"); }
        }
        previous_ = std::move(current);
    }
    std::uint64_t dropped() const { return dropped_; }
private:
    static std::string quoted(const std::string& value) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string result = "\"";
        for (const unsigned char c : value) {
            if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
            else if (c < 32 || c >= 127) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
            else { result += static_cast<char>(c); }
        }
        return result + '"';
    }
    static void quote(std::ostream& out, const std::optional<Quote>& value) {
        if (value) { out << "{\"price_1e4\":\"" << value->price << "\",\"quantity\":\"" << value->quantity << "\"}"; }
        else { out << "null"; }
    }
    static void integer(std::uint8_t* out, std::uint64_t value, unsigned bytes) {
        for (unsigned i = bytes; i != 0; --i) { out[i - 1] = static_cast<std::uint8_t>(value); value >>= 8; }
    }
    void send(const std::string& json) {
        constexpr std::size_t chunk = 1400, header = 28;
        if (json.size() > 8 * 1024 * 1024) { throw std::runtime_error("Monitor symbol snapshot exceeds 8 MiB"); }
        const auto parts = (json.size() + chunk - 1) / chunk;
        const auto generation = ++generation_;
        for (std::size_t part = 0; part < parts; ++part) {
            std::array<std::uint8_t, header + chunk> bytes{};
            bytes[0] = 'L'; bytes[1] = 'L'; bytes[2] = 'M'; bytes[3] = '1';
            integer(bytes.data() + 4, session_, 8); integer(bytes.data() + 12, generation, 8);
            integer(bytes.data() + 20, part, 4); integer(bytes.data() + 24, parts, 4);
            const auto length = std::min(chunk, json.size() - part * chunk);
            std::copy_n(json.data() + part * chunk, length, bytes.data() + header);
            if (sendto(fd_, bytes.data(), header + length, MSG_DONTWAIT,
                       reinterpret_cast<const sockaddr*>(&destination_), sizeof(destination_)) < 0) { ++dropped_; }
        }
    }
    int fd_ = -1;
    sockaddr_in destination_{};
    std::uint64_t session_ = 0, generation_ = 0, heartbeat_ = 0, dropped_ = 0;
    std::map<std::string, std::string> previous_;
};
} // namespace low_latency
