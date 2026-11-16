#include "low_latency/pitch_message.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
using Bytes = std::vector<std::uint8_t>;
using Decoder = low_latency::PitchMessage (*)(std::span<const std::uint8_t>);
struct Result {
    low_latency::PitchMessage message;
    std::string error;
};
Result decode(Decoder decoder, std::span<const std::uint8_t> bytes) {
    try { return {decoder(bytes), {}}; }
    catch (const std::runtime_error& e) { return {{}, e.what()}; }
}
std::size_t checked = 0;
void compare(std::span<std::uint8_t> bytes) {
    const Bytes before(bytes.begin(), bytes.end());
    const auto old = decode(low_latency::decode_pitch_message, bytes);
    const auto packed = decode(low_latency::decode_pitch_message_packed, bytes);
    Result zero;
    try { zero.message = low_latency::decode_pitch_message_zero_copy(bytes); }
    catch (const std::runtime_error& e) { zero.error = e.what(); }
    if (old.error != packed.error || old.error != zero.error ||
        (old.error.empty() && (old.message != packed.message || old.message != zero.message))) {
        throw std::runtime_error("Decoders differ at case " + std::to_string(checked));
    }
    if (!std::equal(bytes.begin(), bytes.end(), before.begin(), before.end())) {
        throw std::runtime_error("Zero-copy decoder changed input bytes");
    }
    ++checked;
}
void page_boundary(const Bytes& bytes) {
#ifdef __linux__
    const auto page = sysconf(_SC_PAGESIZE);
    if (page <= 0) { throw std::runtime_error("Cannot read page size"); }
    const auto size = static_cast<std::size_t>(page);
    auto* memory = static_cast<std::uint8_t*>(mmap(nullptr, size * 2, PROT_READ | PROT_WRITE,
                                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (memory == MAP_FAILED) { throw std::runtime_error("Cannot allocate guard page"); }
    if (mprotect(memory + size, size, PROT_NONE) != 0) {
        munmap(memory, size * 2);
        throw std::runtime_error("Cannot protect guard page");
    }
    try {
        for (std::size_t length = 0; length <= bytes.size(); ++length) {
            auto* start = memory + size - length;
            std::copy_n(bytes.begin(), length, start);
            if (length) { start[0] = static_cast<std::uint8_t>(length); }
            compare({start, length});
        }
    } catch (...) { munmap(memory, size * 2); throw; }
    munmap(memory, size * 2);
#else
    (void)bytes;
#endif
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) { throw std::runtime_error("Need fixture path"); }
        std::ifstream input(argv[1]);
        if (!input) { throw std::runtime_error("Cannot open fixtures"); }
        std::string line;
        std::size_t fixtures = 0;
        std::mt19937 random(20261114);
        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#') { continue; }
            std::istringstream row(line);
            std::string name, hex;
            row >> name >> hex;
            Bytes bytes;
            for (std::size_t i = 0; i < hex.size(); i += 2) {
                bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
            }
            if (!decode(low_latency::decode_pitch_message_packed, bytes).error.empty()) {
                throw std::runtime_error("Valid fixture rejected: " + name);
            }
            ++fixtures;
            compare(bytes);
            page_boundary(bytes);
            // Different starts exercise unaligned wire members and message starts.
            for (std::size_t offset = 0; offset < 16; ++offset) {
                Bytes storage(offset, 0xff);
                storage.insert(storage.end(), bytes.begin(), bytes.end());
                compare(std::span<std::uint8_t>(storage).subspan(offset));
            }
            for (std::size_t length = 0; length < bytes.size(); ++length) {
                Bytes truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
                compare(truncated); // Bad header length, then self-consistent truncation.
                if (!truncated.empty()) { truncated[0] = static_cast<std::uint8_t>(length); compare(truncated); }
            }
            for (std::size_t length = bytes.size() + 1; length <= 255; ++length) {
                auto extended = bytes;
                extended.resize(length, 0xa5);
                extended[0] = static_cast<std::uint8_t>(length);
                compare(extended);
            }
            // Exercise every field (including reserved bytes and high integer bits),
            // the full type byte, and combinations of invalid fields.
            for (std::size_t position = 0; position < bytes.size(); ++position) {
                for (unsigned value = 0; value < 256; ++value) {
                    auto changed = bytes;
                    changed[position] = static_cast<std::uint8_t>(value);
                    compare(changed);
                }
            }
            for (unsigned i = 0; i < 1000; ++i) {
                auto changed = bytes;
                for (auto& value : changed) { value = static_cast<std::uint8_t>(random()); }
                changed[0] = static_cast<std::uint8_t>(changed.size());
                changed[1] = bytes[1];
                compare(changed);
            }
        }
        if (fixtures != 23) { throw std::runtime_error("Need all 23 fixture layouts"); }
        std::cout << "Decoder equivalence: " << checked << " cases passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
