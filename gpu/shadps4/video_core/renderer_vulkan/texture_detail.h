// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Vulkan::TextureDetail {

// Include forward and depth-only alpha-tested geometry: changing only its G-buffer pass
// leaves the depth prepass with different alpha coverage. UI and postprocessing without
// depth testing must retain the game's sampling, even when they use discard.
constexpr float AlphaBias(int level, bool has_discard, bool gbuffer, bool depth_test) {
    if (!has_discard || (!gbuffer && !depth_test)) return 0.0f;
    const int clamped = level < 0 ? 0 : level > 3 ? 3 : level;
    return -0.5f * float(clamped);
}

constexpr float SamplerBias(float scene_bias, float alpha_bias, bool fragment, bool depth_sampler) {
    return depth_sampler ? 0.0f : scene_bias + (fragment ? alpha_bias : 0.0f);
}

} // namespace Vulkan::TextureDetail
