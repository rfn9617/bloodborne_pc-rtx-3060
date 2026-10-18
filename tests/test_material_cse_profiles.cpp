// SPDX-License-Identifier: GPL-2.0-or-later
// Match only fully certified shader binaries; changed sources must fall back.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "video_core/renderer_vulkan/material_cse_profiles.h"

static std::vector<std::uint32_t> Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    assert(file && bytes.size() >= 20 && bytes.size() % 4 == 0);
    std::vector<std::uint32_t> result(bytes.size() / 4);
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}

int main(int argc, char** argv) {
    assert(argc == 3);
    const auto source = Read(argv[1]), expected = Read(argv[2]);
    const auto before = source;
    const auto matched = Vulkan::MaterialCse::Match(source);
    assert(std::ranges::equal(matched, expected));
    assert(source == before);
    for (const auto index : {0u, 3u, unsigned(source.size() / 2), unsigned(source.size() - 1)}) {
        auto changed = source;
        changed[index] ^= 1;
        assert(Vulkan::MaterialCse::Match(changed).empty());
    }
    auto truncated = source;
    truncated.pop_back();
    assert(Vulkan::MaterialCse::Match(truncated).empty());
    assert(Vulkan::MaterialCse::Match(std::span<const std::uint32_t>{}).empty());
    std::puts("PASS: exact certified output, immutable canonical input; changed/truncated sources rejected");
}
