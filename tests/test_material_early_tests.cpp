// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include "video_core/renderer_vulkan/material_early_tests.h"

int main(int argc, char** argv) {
    using namespace Vulkan::MaterialEarlyTests;
    assert(argc == 3);
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    assert(in && bytes.size() >= 20 && bytes.size() % 4 == 0);
    std::vector<std::uint32_t> source(bytes.size()/4), output;
    std::memcpy(source.data(), bytes.data(), bytes.size());
    const auto original = source;
    if (!Convert(source, output)) {
        assert(output.empty() && source == original);
        std::puts("PASS: unsupported execution/side effects rejected; source unchanged");
        return 2;
    }
    assert(source == original && output.size() == source.size() + 3);
    std::vector<std::uint32_t> restored(output.begin(), output.begin() + 5);
    unsigned early_modes = 0;
    for (size_t at = 5; at < output.size();) {
        unsigned words = output[at] >> 16;
        if ((output[at] & 0xffff) == 16 && words == 3 && output[at + 2] == 9)
            ++early_modes;
        else restored.insert(restored.end(), output.begin()+at, output.begin()+at+words);
        at += words;
    }
    assert(early_modes == 1 && restored == source);
    std::vector<std::uint32_t> repeated;
    assert(Convert(output, repeated) && repeated == output);
    auto broken = source;
    broken[5] &= 0xffff;
    assert(!Convert(broken, repeated) && repeated.empty());
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(output.data()), output.size()*4);
    assert(out);
    std::puts("PASS: only EarlyFragmentTests mode added; every other word unchanged; idempotence and malformed input checked");
}
