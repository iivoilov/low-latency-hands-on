#include "low_latency/pcap.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace low_latency {
namespace {

void put_le(std::ostream& stream, std::uint32_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        stream.put(static_cast<char>(value >> (8 * i)));
    }
}

std::uint32_t get32(const std::uint8_t* data, bool little) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data[i]) << (8 * (little ? i : 3 - i));
    }
    return value;
}

std::uint16_t get_be16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(data[0]) << 8) | data[1]);
}

void put_be16(std::uint8_t* data, std::size_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value);
}

void read_exact(std::istream& stream, std::uint8_t* data, std::size_t length) {
    if (!stream.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(length))) {
        throw std::runtime_error("Truncated PCAP file");
    }
}

} // namespace

PcapWriter::PcapWriter(const std::string& path, const std::string& source_ip,
                       const std::string& destination_ip, std::uint16_t port)
    : port_(port) {
    if (inet_pton(AF_INET, source_ip.c_str(), source_.data()) != 1 ||
        inet_pton(AF_INET, destination_ip.c_str(), destination_.data()) != 1) {
        throw std::invalid_argument("PCAP source/destination must be IPv4 addresses");
    }
    stream_.exceptions(std::ios::badbit | std::ios::failbit);
    stream_.open(path, std::ios::binary | std::ios::trunc);
    put_le(stream_, 0xa1b23c4d, 4); // Nanosecond-resolution, little-endian classic PCAP.
    put_le(stream_, 2, 2);
    put_le(stream_, 4, 2);
    put_le(stream_, 0, 4);
    put_le(stream_, 0, 4);
    put_le(stream_, 65535, 4);
    put_le(stream_, 101, 4); // LINKTYPE_RAW: record begins with IPv4 header.
}

void PcapWriter::write(const Packet& packet, std::uint64_t elapsed_ns) {
    validate_pitch(packet.data());
    std::array<std::uint8_t, max_payload + 28> frame{};
    frame[0] = 0x45;
    put_be16(frame.data() + 2, packet.size + 28);
    frame[8] = 1; // TTL
    frame[9] = 17; // UDP
    std::copy(source_.begin(), source_.end(), frame.begin() + 12);
    std::copy(destination_.begin(), destination_.end(), frame.begin() + 16);
    std::uint32_t checksum = 0;
    for (unsigned i = 0; i < 20; i += 2) {
        checksum += get_be16(frame.data() + i);
    }
    while ((checksum >> 16) != 0) {
        checksum = (checksum & 0xffff) + (checksum >> 16);
    }
    put_be16(frame.data() + 10, (~checksum) & 0xffff);
    put_be16(frame.data() + 20, port_);
    put_be16(frame.data() + 22, port_);
    put_be16(frame.data() + 24, packet.size + 8);
    // IPv4 UDP checksum zero means absent, as permitted by UDP.
    std::copy(packet.data().begin(), packet.data().end(), frame.begin() + 28);
    put_le(stream_, static_cast<std::uint32_t>(elapsed_ns / 1'000'000'000), 4);
    put_le(stream_, static_cast<std::uint32_t>(elapsed_ns % 1'000'000'000), 4);
    put_le(stream_, static_cast<std::uint32_t>(packet.size + 28), 4);
    put_le(stream_, static_cast<std::uint32_t>(packet.size + 28), 4);
    stream_.write(reinterpret_cast<const char*>(frame.data()),
                  static_cast<std::streamsize>(packet.size + 28));
}

void PcapWriter::finish() { stream_.close(); }

PcapReader::PcapReader(const std::string& path, std::uint16_t filter_port)
    : stream_(path, std::ios::binary), filter_port_(filter_port) {
    if (!stream_) {
        throw std::runtime_error("Cannot open PCAP input: " + path);
    }
    std::array<std::uint8_t, 24> header{};
    read_exact(stream_, header.data(), header.size());
    const auto magic = get32(header.data(), true);
    nanosecond_ = magic == 0xa1b23c4d || magic == 0x4d3cb2a1;
    if (magic == 0xa1b2c3d4 || magic == 0xa1b23c4d) {
        little_endian_ = true;
    } else if (magic == 0xd4c3b2a1 || magic == 0x4d3cb2a1) {
        little_endian_ = false;
    } else {
        throw std::runtime_error("Expected classic PCAP (PCAPNG is not supported)");
    }
    const auto version = get32(header.data() + 4, little_endian_);
    if (version != (little_endian_ ? 0x00040002U : 0x00020004U)) {
        throw std::runtime_error("Expected PCAP version 2.4");
    }
    snap_length_ = get32(header.data() + 16, little_endian_);
    link_type_ = get32(header.data() + 20, little_endian_);
    if (link_type_ != 1 && link_type_ != 101) {
        throw std::runtime_error("PCAP must use Ethernet or raw IP link type");
    }
    if (snap_length_ == 0 || snap_length_ > 1'048'576) {
        throw std::runtime_error("Invalid or unsupported PCAP snapshot length");
    }
    frame_.resize(snap_length_);
}

bool PcapReader::next(Packet& packet) {
    std::array<std::uint8_t, 16> header{};
    while (true) {
        stream_.read(reinterpret_cast<char*>(header.data()), header.size());
        if (stream_.gcount() == 0 && stream_.eof()) {
            return false;
        }
        if (!stream_) {
            throw std::runtime_error("Truncated PCAP record header");
        }
        const auto seconds = get32(header.data(), little_endian_);
        const auto fraction = get32(header.data() + 4, little_endian_);
        if (fraction >= (nanosecond_ ? 1'000'000'000U : 1'000'000U)) {
            throw std::runtime_error("Invalid PCAP timestamp fraction");
        }
        const auto captured = get32(header.data() + 8, little_endian_);
        const auto original = get32(header.data() + 12, little_endian_);
        if (captured == 0 || captured > snap_length_ || captured != original) {
            throw std::runtime_error("Invalid or truncated PCAP record; capture full packets");
        }
        auto frame = std::span(frame_).first(captured);
        read_exact(stream_, frame.data(), frame.size());
        std::size_t ip = 0;
        if (link_type_ == 1) {
            if (frame.size() < 14) {
                throw std::runtime_error("Truncated Ethernet header");
            }
            auto ether_type = get_be16(frame.data() + 12);
            ip = 14;
            while (ether_type == 0x8100 || ether_type == 0x88a8) {
                if (ip + 4 > frame.size()) {
                    throw std::runtime_error("Truncated VLAN tag");
                }
                ether_type = get_be16(frame.data() + ip + 2);
                ip += 4;
            }
            if (ether_type != 0x0800) {
                continue;
            }
        }
        if (ip >= frame.size()) {
            throw std::runtime_error("Missing IP header");
        }
        if ((frame[ip] >> 4) != 4) {
            continue;
        }
        if (ip + 20 > frame.size()) {
            throw std::runtime_error("Truncated IPv4 header");
        }
        const auto ihl = static_cast<std::size_t>((frame[ip] & 15) * 4);
        const auto total = get_be16(frame.data() + ip + 2);
        if (ihl < 20 || total < ihl || ip + total > frame.size()) {
            throw std::runtime_error("Invalid IPv4 lengths");
        }
        if (frame[ip + 9] != 17) {
            continue;
        }
        if ((get_be16(frame.data() + ip + 6) & 0x3fff) != 0) {
            throw std::runtime_error("Fragmented UDP is unsupported; capture unfragmented feed packets");
        }
        const auto udp = ip + ihl;
        if (ihl + 8 > total) {
            throw std::runtime_error("Truncated UDP header");
        }
        if (filter_port_ != 0 && get_be16(frame.data() + udp + 2) != filter_port_) {
            continue;
        }
        const auto udp_length = get_be16(frame.data() + udp + 4);
        if (udp_length < 8 || ihl + udp_length != total || udp_length - 8U > max_payload) {
            throw std::runtime_error("Invalid UDP length or payload exceeds 1472 bytes");
        }
        packet.size = udp_length - 8U;
        std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(udp + 8), packet.size,
                    packet.bytes.begin());
        validate_pitch(packet.data());
        timestamp_ns_ = std::uint64_t{seconds} * 1'000'000'000 +
                        std::uint64_t{fraction} * (nanosecond_ ? 1 : 1000);
        return true;
    }
}

} // namespace low_latency
