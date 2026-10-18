// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#ifdef _WIN32
#define NDEBUG
#endif
#include "video_core/renderer_vulkan/material_uniforms.h"
#ifdef _WIN32
#undef NDEBUG
#include <cassert>
#endif

int main(int argc, char** argv) {
    assert(argc == 3);
    std::ifstream input(argv[1], std::ios::binary);
    assert(input);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    assert(bytes.size() % 4 == 0);
    std::vector<u32> source(bytes.size() / 4);
    std::memcpy(source.data(), bytes.data(), bytes.size());
    const auto original = source;
    std::vector<u32> converted;
    const std::array<u32, 6> bindings{0, 1, 2, 4, 5, 6};
    assert(Vulkan::MaterialUniforms::Convert(source, bindings, converted));
    assert(source == original && !converted.empty());
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char*>(converted.data()), converted.size() * 4);
    assert(output);
    output.close();
    const std::array<u32, 1> missing{100};
    assert(!Vulkan::MaterialUniforms::Convert(source, missing, converted) && converted.empty());
    const std::array<u32, 1> mixed_alias{3};
    assert(!Vulkan::MaterialUniforms::Convert(source, mixed_alias, converted));
    source[5] &= 0xffff; // malformed instruction word count
    assert(!Vulkan::MaterialUniforms::Convert(source, bindings, converted));
    std::printf("PASS: six constant bindings converted; canonical source intact; mixed/dynamic aliases and malformed inputs rejected\n");
}
