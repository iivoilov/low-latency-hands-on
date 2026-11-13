#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace low_latency {

constexpr std::size_t max_payload = 1472; // IPv4/UDP within a 1500-byte MTU.

struct Packet {
    std::array<std::uint8_t, max_payload> bytes{};
    std::size_t size = 0;
    std::span<const std::uint8_t> data() const { return {bytes.data(), size}; }
};

std::uint32_t advance_sequence(std::uint32_t sequence, std::uint8_t count);
// Validate framing, allowing unknown message types and future message extensions.
void validate_pitch(std::span<const std::uint8_t> payload);

struct BookLevel {
    std::string symbol;
    char side;
    std::uint64_t price; // Units of 0.0001.
    std::uint64_t quantity;
    unsigned orders;
    bool operator==(const BookLevel&) const = default;
};

class PitchGenerator {
public:
    PitchGenerator(std::uint64_t seed, std::uint8_t unit, unsigned events_per_packet,
                   std::vector<std::string> symbols, unsigned levels = 10,
                   unsigned orders_per_level = 3);
    // Deterministic logical time since a synthetic 09:30:00 Eastern session start.
    Packet next(std::uint64_t elapsed_ns, unsigned event_limit = 0);
    std::size_t initial_orders() const { return orders_.size(); }
    std::vector<BookLevel> book() const;

private:
    struct Order {
        std::uint64_t id = 0;
        std::uint32_t quantity = 0;
        std::size_t symbol;
        char side;
        std::uint64_t price;
    };
    std::uint64_t random();
    void add(Packet& packet, Order& order, std::uint32_t offset);

    std::uint64_t random_state_;
    std::uint8_t unit_;
    unsigned events_per_packet_;
    std::vector<std::string> symbols_;
    std::vector<Order> orders_;
    unsigned levels_;
    std::size_t initialized_ = 0;
    std::size_t refill_ = 0;
    std::uint32_t sequence_ = 1;
    std::uint64_t order_id_ = 0;
    std::uint64_t execution_id_ = 0;
    std::uint32_t last_second_ = 0;
    bool started_ = false;
};

} // namespace low_latency
