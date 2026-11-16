#pragma once
#include "low_latency/pcap.hpp"
#include "low_latency/pitch_message.hpp"
#include <map>
#include <optional>
#include <unordered_map>

namespace low_latency {
// Immutable after construction. Sender and recovery service may read in parallel.
// Sparse full-book checkpoints bound reconstruction to checkpoint_every-1 events.
class RecoveryArchive {
public:
    using Bytes = std::vector<std::uint8_t>;
    struct TimedPacket { std::uint64_t time_ns; Packet packet; std::size_t through; };
    RecoveryArchive(const std::string& path, std::uint8_t unit, std::size_t checkpoint_every = 4096);
    const std::vector<TimedPacket>& packets() const { return packets_; }
    const Bytes& message(std::size_t index) const { return messages_.at(index); }
    std::uint32_t sequence(std::size_t through) const;
    std::optional<std::size_t> find(std::uint32_t sequence) const;
    std::vector<Bytes> snapshot(std::size_t through) const;
    std::size_t through_time(std::uint64_t relative_ns) const;
    std::size_t messages() const { return messages_.size(); }
    std::size_t checkpoints() const { return checkpoints_.size(); }
    std::uint8_t unit() const { return unit_; }
private:
    static constexpr std::size_t absent = static_cast<std::size_t>(-1);
    struct Stored {
        PitchMessage order;
        Bytes metadata;
        std::size_t index = 0, time = absent, reference = absent;
    };
    struct State {
        std::map<std::uint64_t, Stored> orders;
        std::map<std::pair<Symbol,std::uint8_t>, Stored> metadata;
        std::size_t time = absent, reference = absent;
        bool initialized = false;
    };
    void apply(State& state, std::size_t index) const;
    std::uint8_t unit_;
    std::size_t checkpoint_every_;
    std::vector<TimedPacket> packets_;
    std::vector<Bytes> messages_;
    std::vector<std::uint32_t> sequences_;
    std::unordered_map<std::uint32_t,std::size_t> index_;
    std::vector<State> checkpoints_;
};
} // namespace low_latency
