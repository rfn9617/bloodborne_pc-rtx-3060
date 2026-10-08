// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/texture_detail.h"

int main() {
    using namespace Vulkan::TextureDetail;
    // The same alpha material keeps coverage through deferred, forward and depth prepasses.
    for (int level = 0; level <= 3; ++level) {
        const float expected = -0.5f * level;
        assert(AlphaBias(level, true, true, true) == expected);
        assert(AlphaBias(level, true, false, true) == expected);
        assert(AlphaBias(level, true, true, false) == expected);
        assert(AlphaBias(level, false, true, true) == 0.0f); // opaque material
        assert(AlphaBias(level, true, false, false) == 0.0f); // UI/postprocess
    }
    assert(AlphaBias(-2, true, true, true) == 0.0f);
    assert(AlphaBias(99, true, true, true) == -1.5f);
    assert(SamplerBias(-0.5f, -1.0f, true, false) == -1.5f);
    assert(SamplerBias(-0.5f, -1.0f, false, false) == -0.5f); // vertex sampling
    assert(SamplerBias(-0.5f, -1.0f, true, true) == 0.0f); // shadow compare sampler
    std::puts("Texture detail: material/depth coverage and unaffected UI/depth samplers PASS");
}
