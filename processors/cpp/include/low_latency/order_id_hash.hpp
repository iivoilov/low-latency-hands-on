#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace low_latency {

// SipHash-2-4 for exactly eight little-endian order-ID bytes. Adapted from the
// Aumasson/Bernstein CC0 reference: https://github.com/veorq/SipHash
// Copyright (c) 2012-2022 Jean-Philippe Aumasson; 2012-2014 Daniel J. Bernstein.
// See docs/licenses/SipHash-CC0.txt. This is hash-flooding resistance, not feed
// authentication or a deterministic worst-case lookup bound.
class OrderIdHash {
public:
    OrderIdHash(); // OS entropy once at book construction, before reception.
    OrderIdHash(std::uint64_t key0, std::uint64_t key1) : key_{key0, key1} {} // Test fixtures.

    std::size_t operator()(std::uint64_t id) const noexcept {
        auto v0 = UINT64_C(0x736f6d6570736575) ^ key_[0];
        auto v1 = UINT64_C(0x646f72616e646f6d) ^ key_[1];
        auto v2 = UINT64_C(0x6c7967656e657261) ^ key_[0];
        auto v3 = UINT64_C(0x7465646279746573) ^ key_[1];
        const auto round = [&] {
            v0 += v1; v1 = std::rotl(v1, 13); v1 ^= v0; v0 = std::rotl(v0, 32);
            v2 += v3; v3 = std::rotl(v3, 16); v3 ^= v2;
            v0 += v3; v3 = std::rotl(v3, 21); v3 ^= v0;
            v2 += v1; v1 = std::rotl(v1, 17); v1 ^= v2; v2 = std::rotl(v2, 32);
        };
        v3 ^= id;
        round(); round();
        v0 ^= id;
        // A full eight-byte message still has the SipHash length/final block.
        constexpr auto final_block = UINT64_C(8) << 56;
        v3 ^= final_block;
        round(); round();
        v0 ^= final_block;
        v2 ^= UINT64_C(0xff);
        round(); round(); round(); round();
        return static_cast<std::size_t>(v0 ^ v1 ^ v2 ^ v3);
    }

private:
    std::array<std::uint64_t, 2> key_;
};

} // namespace low_latency
