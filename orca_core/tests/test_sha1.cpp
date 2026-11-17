#include "orca/crypto/sha1.hpp"
#include <array>
#include <cstdio>
#include <string_view>

int main() {
    constexpr std::string_view input="abc";
    const auto digest=orca::crypto::sha1(std::span<const orca::u8>(
        reinterpret_cast<const orca::u8*>(input.data()),input.size()));
    constexpr std::array<orca::u8,20> expected={
        0xA9,0x99,0x3E,0x36,0x47,0x06,0x81,0x6A,0xBA,0x3E,
        0x25,0x71,0x78,0x50,0xC2,0x6C,0x9C,0xD0,0xD8,0x9D
    };
    if(digest!=expected) { std::puts("SHA-1 known-answer test failed"); return 1; }
    std::puts("SHA-1 known-answer test passed");
    return 0;
}
