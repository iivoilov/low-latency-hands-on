#pragma once

#include "low_latency/pitch.hpp"

#include <fstream>
#include <string>

namespace low_latency {

// Classic PCAP only. Writer uses LINKTYPE_RAW (IPv4); reader also accepts Ethernet,
// including single/double VLAN tags. PCAPNG, fragmentation, IPv6 are not supported.
class PcapWriter {
public:
    PcapWriter(const std::string& path, const std::string& source_ip,
               const std::string& destination_ip, std::uint16_t port);
    void write(const Packet& packet, std::uint64_t elapsed_ns);
    void finish();

private:
    std::ofstream stream_;
    std::array<std::uint8_t, 4> source_{};
    std::array<std::uint8_t, 4> destination_{};
    std::uint16_t port_;
};

class PcapReader {
public:
    explicit PcapReader(const std::string& path, std::uint16_t filter_port = 0);
    bool next(Packet& packet); // Skips non-IPv4/UDP traffic and nonmatching ports.
    std::uint64_t timestamp_ns() const { return timestamp_ns_; }

private:
    std::ifstream stream_;
    bool little_endian_ = true;
    bool nanosecond_ = false;
    std::uint64_t timestamp_ns_ = 0;
    std::uint32_t link_type_ = 0;
    std::uint32_t snap_length_ = 0;
    std::uint16_t filter_port_;
    std::vector<std::uint8_t> frame_;
};

} // namespace low_latency
