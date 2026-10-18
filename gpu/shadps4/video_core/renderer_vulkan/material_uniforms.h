// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <span>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Shader { struct Info; }
namespace Vulkan {
class Instance;
namespace MaterialUniforms {
// A bounded experiment for the measured Bloodborne G-buffer material shader.
// Cached binaries remain canonical SSBO binaries; conversion is transient.
constexpr u32 Capacity = 16 * 1024;
bool Use(const Shader::Info& info, u32 index, const Instance& instance);
bool Convert(std::span<const u32> source, std::span<const u32> bindings,
             std::vector<u32>& converted);
vk::ShaderModule Compile(const Shader::Info& info, u32 first_binding,
                         std::span<const u32> source, const Instance& instance);
} // namespace MaterialUniforms
} // namespace Vulkan
