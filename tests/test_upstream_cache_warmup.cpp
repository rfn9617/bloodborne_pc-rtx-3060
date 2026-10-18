// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "common/elf_info.h"
#include "../gpu/bbgpu.h"
#define NDEBUG
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#undef NDEBUG
#include <cassert>
// Same friend definition as shim/bbgpu.cpp: initialize the game identity without
// creating an SDL window or starting a guest process.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const BbGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};
}
int main() {
    BbGpuConfig config{};
    config.serial = "CUSA03173";
    Core::Emulator::FillElfInfo(config);
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::PipelineCache cache(instance, scheduler, nullptr, 16);
    cache.WarmUp();
    const auto [graphics, motion] = cache.GraphicsPipelineCounts();
    const bool expect_rejected = std::getenv("BB_TEST_OLD_CACHE_REJECTED") != nullptr;
    assert((expect_rejected ? graphics == 0 : graphics > 0) && motion == 0);
    std::printf("Actual warm cache: %zu graphics pipelines retained, %zu motion pipelines preloaded PASS\n", graphics, motion);
}
