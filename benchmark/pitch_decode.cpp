#include "low_latency/pitch_message.hpp"
#include "low_latency/pcap.hpp"
#include "low_latency/processor.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <variant>
#ifdef __linux__
#include <sched.h>
#endif

namespace {
using ReadOnlyDecoder = low_latency::PitchMessage (*)(std::span<const std::uint8_t>);
using MutableDecoder = low_latency::PitchMessage (*)(std::span<std::uint8_t>);
using Decoder = std::variant<ReadOnlyDecoder, MutableDecoder>;
constexpr std::array<const char*, 3> decoder_names{"original", "packed", "zero-copy"};
struct Range { std::size_t offset, length; };
struct Corpus {
    std::vector<std::uint8_t> bytes;
    std::vector<Range> messages;
    std::array<std::size_t, 256> counts{};
    void append(std::span<const std::uint8_t> message) {
        messages.push_back({bytes.size(), message.size()});
        bytes.insert(bytes.end(), message.begin(), message.end());
        ++counts[message[1]];
    }
    std::span<std::uint8_t> view(const Range& range) {
        return std::span<std::uint8_t>(bytes).subspan(range.offset, range.length);
    }
    std::span<const std::uint8_t> view(const Range& range) const {
        return std::span<const std::uint8_t>(bytes).subspan(range.offset, range.length);
    }
};
struct Options {
    std::string fixtures, pcap, order = "input";
    std::uint64_t messages = 5'000'000, warmup = 100'000;
    unsigned repeats = 9, seed = 20261114;
    std::array<bool, 256> types{};
    bool filter_types = false;
    std::size_t length = 0, copies = 1;
    std::vector<unsigned> decoders{0, 1};
};
std::uint64_t integer(const std::string& value) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) { throw std::runtime_error("Invalid integer"); }
    return result;
}
Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (name == "--help") {
            std::cout << "Usage: pitch_decode_bench (--fixtures TSV | --pcap PCAP) [options]\n"
                         "  --messages N  Minimum decoded messages per timed run (5000000)\n"
                         "  --warmup N    Minimum warmup messages per decoder (100000)\n"
                         "  --repeats N   Paired repetitions, 1..101 (9)\n"
                         "  --seed N      Reproducible decoder-order shuffle (20261114)\n"
                         "  --decoders LIST  2 or 3 of original,packed,zero-copy (original,packed)\n"
                         "  --type N      Keep a message type (decimal or 0xHEX); repeat for several\n"
                         "  --length N    Keep a wire length, 2..255 (separates Time variants)\n"
                         "  --copies N    Repeat selected views (1; max 10000000 total views)\n"
                         "  --order MODE  input, grouped by type/length, or shuffled (input)\n"
                         "Selection/repetition/order preserve backing bytes and run before timing.\n"
                         "Whole corpus passes round message counts up. File I/O, validation\n"
                         "between decoders and JSON output are outside the timed interval.\n"
                         "Results are batch-average decode ns/message, not RX-to-L1 latency.\n";
            std::exit(0);
        }
        if (++i == argc) { throw std::runtime_error("Missing argument"); }
        const std::string value = argv[i];
        if (name == "--fixtures") { o.fixtures = value; }
        else if (name == "--pcap") { o.pcap = value; }
        else if (name == "--messages") { o.messages = integer(value); }
        else if (name == "--warmup") { o.warmup = integer(value); }
        else if (name == "--decoders") {
            o.decoders.clear();
            std::istringstream list(value);
            std::string item;
            while (std::getline(list, item, ',')) {
                const auto found = std::find_if(decoder_names.begin(), decoder_names.end(),
                                                [&](const char* n) { return item == n; });
                if (found == decoder_names.end()) { throw std::runtime_error("Unknown decoder: " + item); }
                const auto id = static_cast<unsigned>(found - decoder_names.begin());
                if (std::find(o.decoders.begin(), o.decoders.end(), id) != o.decoders.end()) {
                    throw std::runtime_error("Duplicate decoder");
                }
                o.decoders.push_back(id);
            }
            if (o.decoders.size() < 2 || o.decoders.size() > 3) { throw std::runtime_error("Select two or three distinct decoders"); }
        }
        else if (name == "--type") {
            std::uint64_t type = 0;
            if (value.starts_with("0x")) {
                const auto [end, error] = std::from_chars(value.data() + 2, value.data() + value.size(), type, 16);
                if (error != std::errc{} || end != value.data() + value.size()) { throw std::runtime_error("Invalid type"); }
            } else { type = integer(value); }
            if (type > 255) { throw std::runtime_error("Type out of range"); }
            o.types[static_cast<std::size_t>(type)] = true;
            o.filter_types = true;
        } else if (name == "--length") {
            const auto n = integer(value);
            if (n < 2 || n > 255) { throw std::runtime_error("Need length 2..255"); }
            o.length = static_cast<std::size_t>(n);
        } else if (name == "--copies") {
            const auto n = integer(value);
            if (n == 0 || n > 10'000'000) { throw std::runtime_error("Need 1..10000000 copies"); }
            o.copies = static_cast<std::size_t>(n);
        } else if (name == "--order") {
            if (value != "input" && value != "grouped" && value != "shuffled") { throw std::runtime_error("Invalid corpus order"); }
            o.order = value;
        }
        else if (name == "--repeats") {
            const auto n = integer(value);
            if (n == 0 || n > 101) { throw std::runtime_error("Need 1..101 repeats"); }
            o.repeats = static_cast<unsigned>(n);
        } else if (name == "--seed") {
            const auto n = integer(value);
            if (n > std::numeric_limits<unsigned>::max()) { throw std::runtime_error("Seed out of range"); }
            o.seed = static_cast<unsigned>(n);
        } else { throw std::runtime_error("Unknown option: " + name); }
    }
    if (o.fixtures.empty() == o.pcap.empty() || o.messages == 0 || o.messages > 1'000'000'000 ||
        o.warmup > 1'000'000'000) { throw std::runtime_error("Choose one input and bounded, nonzero message count"); }
    return o;
}
Corpus read(const Options& o) {
    Corpus corpus;
    if (!o.fixtures.empty()) {
        std::ifstream input(o.fixtures);
        if (!input) { throw std::runtime_error("Cannot open fixtures"); }
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#') { continue; }
            std::istringstream row(line);
            std::string name, hex;
            row >> name >> hex;
            if (row.fail() || hex.size() < 4 || hex.size() % 2 != 0) { throw std::runtime_error("Invalid fixture row"); }
            std::vector<std::uint8_t> bytes;
            for (std::size_t i = 0; i < hex.size(); i += 2) {
                bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
            }
            corpus.append(bytes);
        }
    } else {
        low_latency::PcapReader reader(o.pcap);
        low_latency::Packet packet;
        while (reader.next(packet)) {
            const auto payload = packet.data();
            low_latency::validate_pitch(payload);
            std::size_t offset = 8;
            for (unsigned i = 0; i < payload[2]; ++i) {
                corpus.append(payload.subspan(offset, payload[offset]));
                offset += payload[offset];
            }
        }
    }
    if (corpus.messages.empty()) { throw std::runtime_error("Empty corpus"); }
    for (const auto& range : corpus.messages) {
        const auto bytes = corpus.view(range);
        const auto expected = low_latency::decode_pitch_message(bytes);
        if (expected != low_latency::decode_pitch_message_packed(bytes) ||
            expected != low_latency::decode_pitch_message_zero_copy(bytes)) {
            throw std::runtime_error("Decoders disagree on input");
        }
    }
    return corpus;
}
void select_and_order(Corpus& corpus, const Options& o) {
    std::erase_if(corpus.messages, [&](const Range& r) {
        return (o.filter_types && !o.types[corpus.bytes[r.offset + 1]]) || (o.length != 0 && r.length != o.length);
    });
    if (corpus.messages.empty()) { throw std::runtime_error("Selection contains no messages"); }
    constexpr std::size_t maximum_views = 10'000'000;
    if (corpus.messages.size() > maximum_views / o.copies) { throw std::runtime_error("Too many repeated message views"); }
    if (o.copies != 1) {
        const auto original = corpus.messages;
        corpus.messages.reserve(original.size() * o.copies);
        for (std::size_t i = 1; i < o.copies; ++i) { corpus.messages.insert(corpus.messages.end(), original.begin(), original.end()); }
    }
    if (o.order == "grouped") {
        std::stable_sort(corpus.messages.begin(), corpus.messages.end(), [&](const Range& a, const Range& b) {
            const auto a_type = corpus.bytes[a.offset + 1], b_type = corpus.bytes[b.offset + 1];
            return a_type < b_type || (a_type == b_type && a.length < b.length);
        });
    } else if (o.order == "shuffled") {
        std::mt19937 random(o.seed);
        std::shuffle(corpus.messages.begin(), corpus.messages.end(), random);
    }
    corpus.counts.fill(0);
    for (const auto& r : corpus.messages) { ++corpus.counts[corpus.bytes[r.offset + 1]]; }
}
std::uint64_t order_hash(const Corpus& corpus) {
    // A reproducibility fingerprint, not an authentication hash.
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const auto& r : corpus.messages) {
        hash ^= r.offset;
        hash *= UINT64_C(1099511628211);
        hash ^= r.length;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}
// Both out-of-line decoders return the same normalized object. Consume results
// identically. No LTO is used for the recorded comparison; do not remove this
// checksum or compare a fully inlined build with an out-of-line build.
template<class Function>
std::uint64_t decode_loop(Function decoder, Corpus& corpus, std::uint64_t passes) {
    std::uint64_t checksum = 0;
    for (std::uint64_t pass = 0; pass < passes; ++pass) {
        for (const auto& range : corpus.messages) {
            const auto m = decoder(corpus.view(range));
            checksum += m.order_id ^ m.price ^ m.execution_id ^ (std::uint64_t{m.quantity} << 32) ^ m.type;
        }
    }
    return checksum;
}
// The read-only/mutable signature is selected once per batch, outside the
// message loop. Each loop calls the actual decoder directly through its pointer.
std::uint64_t decode(const Decoder& decoder, Corpus& corpus, std::uint64_t passes) {
    return std::visit([&](auto function) { return decode_loop(function, corpus, passes); }, decoder);
}
int current_cpu() {
#ifdef __linux__
    return sched_getcpu();
#else
    return -1;
#endif
}
std::vector<int> affinity() {
    std::vector<int> result;
#ifdef __linux__
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) != 0) { throw std::runtime_error("Cannot read benchmark CPU affinity"); }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) { if (CPU_ISSET(cpu, &mask)) { result.push_back(cpu); } }
#endif
    return result;
}
struct Run { unsigned repeat, position, decoder; std::uint64_t elapsed, checksum; int start_cpu, end_cpu; };
double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto o = parse(argc, argv);
        const auto start_affinity = affinity();
        auto corpus = read(o);
        select_and_order(corpus, o);
        const auto size = static_cast<std::uint64_t>(corpus.messages.size());
        const auto passes = (o.messages + size - 1) / size;
        const auto warmup_passes = (o.warmup + size - 1) / size;
        const auto count = passes * size;
        const std::array<Decoder, 3> available{
            ReadOnlyDecoder{low_latency::decode_pitch_message},
            ReadOnlyDecoder{low_latency::decode_pitch_message_packed},
            MutableDecoder{low_latency::decode_pitch_message_zero_copy}};
        std::vector<Decoder> decoders;
        std::vector<const char*> names;
        for (const auto id : o.decoders) { decoders.push_back(available[id]); names.push_back(decoder_names[id]); }
        const auto variants = static_cast<unsigned>(decoders.size());
        const auto warmup_original = decode(decoders[0], corpus, warmup_passes);
        for (unsigned i = 1; i < variants; ++i) {
            if (decode(decoders[i], corpus, warmup_passes) != warmup_original) {
                throw std::runtime_error("Warmup checksum mismatch");
            }
        }
        std::vector<Run> runs;
        std::mt19937 random(o.seed);
        for (unsigned repeat = 0; repeat < o.repeats; ++repeat) {
            std::vector<unsigned> order;
            if (variants == 2) {
                const auto first = static_cast<unsigned>(random() % 2);
                order = {first, first ^ 1U};
            } else {
                order = {0, 1, 2};
                std::shuffle(order.begin(), order.end(), random);
            }
            const auto start_run = runs.size();
            for (unsigned position = 0; position < variants; ++position) {
                const auto decoder = order[position];
                const auto start_cpu = current_cpu();
                const auto start = low_latency::monotonic_ns();
                const auto checksum = decode(decoders[decoder], corpus, passes);
                const auto elapsed = low_latency::monotonic_ns() - start;
                const auto end_cpu = current_cpu();
                runs.push_back({repeat + 1, position + 1, decoder, elapsed, checksum, start_cpu, end_cpu});
            }
            for (unsigned i = 1; i < variants; ++i) {
                if (runs[start_run + i].checksum != runs[start_run].checksum) {
                    throw std::runtime_error("Measured checksum mismatch");
                }
            }
        }
        if (affinity() != start_affinity) { throw std::runtime_error("Benchmark affinity changed during run"); }
        std::cout << std::setprecision(12) << "{\n  \"compiler\": " << std::quoted(__VERSION__)
            << ",\n  \"build_type\": " << std::quoted(DECODER_BUILD_TYPE)
            << ",\n  \"clock\": \"CLOCK_MONOTONIC\",\n  \"measurement\": \"batch_average_decode_ns_per_message\""
            << ",\n  \"input\": " << std::quoted(o.fixtures.empty() ? o.pcap : o.fixtures)
            << ",\n  \"corpus_messages\": " << size << ",\n  \"corpus_bytes\": " << corpus.bytes.size()
            << ",\n  \"corpus_order\": " << std::quoted(o.order) << ",\n  \"corpus_copies\": " << o.copies
            << ",\n  \"message_order_hash\": " << order_hash(corpus)
            << ",\n  \"passes_per_run\": " << passes << ",\n  \"messages_per_run\": " << count
            << ",\n  \"warmup_messages_per_decoder\": " << warmup_passes * size
            << ",\n  \"warmup_checksum\": " << warmup_original << ",\n  \"order_seed\": " << o.seed
            << ",\n  \"affinity_cpus\": [";
        for (std::size_t i = 0; i < start_affinity.size(); ++i) {
            if (i) { std::cout << ','; }
            std::cout << start_affinity[i];
        }
        std::cout << "],\n  \"message_type_counts\": {";
        bool first_type = true;
        for (unsigned type = 0; type < corpus.counts.size(); ++type) {
            if (corpus.counts[type] == 0) { continue; }
            if (!first_type) { std::cout << ','; }
            first_type = false;
            std::cout << '"' << type << "\":" << corpus.counts[type];
        }
        std::array<std::size_t, 64> start_alignment{};
        const auto base_alignment = reinterpret_cast<std::uintptr_t>(corpus.bytes.data()) % 64;
        for (const auto& r : corpus.messages) { ++start_alignment[(base_alignment + r.offset) % 64]; }
        std::cout << "},\n  \"storage_mod64\": " << base_alignment << ",\n  \"message_start_mod64_counts\": [";
        for (std::size_t i = 0; i < start_alignment.size(); ++i) {
            if (i) { std::cout << ','; }
            std::cout << start_alignment[i];
        }
        std::cout << "],\n  \"runs\": [\n";
        for (std::size_t i = 0; i < runs.size(); ++i) {
            const auto& r = runs[i];
            std::cout << "    {\"repeat\":" << r.repeat << ",\"position\":" << r.position
                << ",\"decoder\":\"" << names[r.decoder] << "\",\"elapsed_ns\":" << r.elapsed
                << ",\"ns_per_message\":" << static_cast<double>(r.elapsed) / static_cast<double>(count)
                << ",\"start_cpu\":" << r.start_cpu << ",\"end_cpu\":" << r.end_cpu
                << ",\"checksum\":" << r.checksum << '}' << (i + 1 == runs.size() ? "\n" : ",\n");
        }
        std::vector<std::vector<double>> averages(variants), paired_speedups(variants);
        for (const auto& r : runs) { averages[r.decoder].push_back(static_cast<double>(r.elapsed) / static_cast<double>(count)); }
        for (std::size_t i = 0; i < runs.size(); i += variants) {
            std::array<std::uint64_t, 3> elapsed{};
            for (unsigned j = 0; j < variants; ++j) { elapsed[runs[i + j].decoder] = runs[i + j].elapsed; }
            for (unsigned j = 1; j < variants; ++j) {
                paired_speedups[j].push_back(static_cast<double>(elapsed[0]) / static_cast<double>(elapsed[j]));
            }
        }
        std::cout << "  ],\n  \"summary\": [\n";
        for (unsigned decoder = 0; decoder < variants; ++decoder) {
            const auto& values = averages[decoder];
            std::cout << "    {\"decoder\":\"" << names[decoder] << "\",\"runs\":" << values.size()
                << ",\"median_ns_per_message\":" << median(values)
                << ",\"min_ns_per_message\":" << *std::min_element(values.begin(), values.end())
                << ",\"max_ns_per_message\":" << *std::max_element(values.begin(), values.end())
                << '}' << (decoder + 1 == variants ? "\n" : ",\n");
        }
        std::cout << "  ]";
        if (variants == 2) { std::cout << ",\n  \"median_paired_speedup\": " << median(paired_speedups[1]); }
        std::cout << ",\n  \"median_paired_speedups_vs_first\": {";
        for (unsigned j = 1; j < variants; ++j) {
            if (j != 1) { std::cout << ','; }
            std::cout << std::quoted(names[j]) << ':' << median(paired_speedups[j]);
        }
        std::cout << "}\n}\n";
    } catch (const std::exception& e) { std::cerr << "pitch_decode_bench: " << e.what() << '\n'; return 1; }
}
