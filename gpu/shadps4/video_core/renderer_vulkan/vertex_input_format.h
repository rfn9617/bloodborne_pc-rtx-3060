// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <utility>
#include "video_core/amdgpu/resource.h"

namespace Vulkan {
inline std::pair<AmdGpu::DataFormat, AmdGpu::NumberFormat>
VertexInputDescriptorFormat(const AmdGpu::Buffer& buffer) {
    if (!buffer && buffer.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) {
        // Keep a null binding (robustness2), but Vulkan still requires a valid
        // attribute format. An absent optional input must not discard the draw.
        return {AmdGpu::DataFormat::Format32_32_32_32, AmdGpu::NumberFormat::Float};
    }
    return {buffer.GetDataFmt(), buffer.GetNumberFmt()};
}
}
