// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_object_motion.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>

#include <vk_mem_alloc.h>
#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

bool CreateBuffer(const Instance& instance, vk::DeviceSize size, bool host, vk::Buffer& buffer,
                  VmaAllocation& allocation, void** mapped, u64& address) {
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    VmaAllocationCreateInfo alloc_ci{
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    if (host) {
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    } else {
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VkBuffer raw{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &raw, &allocation, &info) !=
        VK_SUCCESS) {
        return false;
    }
    buffer = raw;
    if (mapped) {
        *mapped = info.pMappedData;
    }
    address = instance.GetDevice().getBufferAddress({.buffer = buffer});
    return address != 0;
}

} // namespace

ObjectMotion::ObjectMotion(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_},
      compact_history{[] { const char* p = std::getenv("BB_OBJECT_MOTION_COMPACT");
                           return p && p[0] == '1'; }()},
      diagnostics{[] { const char* p = std::getenv("BB_OBJECT_MOTION_STATS");
                       return p && p[0] == '1'; }()},
      PositionsPerFrame{compact_history ? (12u << 20) : (8u << 20)} {
    // Pipeline selection limits the extra attachment and vertex stores to likely
    // animated draws. A setting or environment override can still disable the path.
    if (!BbSettings::Get().object_motion ||
        BbSettings::Get().upscaler == BbSettings::UpscalerOff) {
        std::puts("Object motion: off (enable in menu or BB_OBJECT_MOTION=1)");
        return;
    }
    const auto& features = instance.GetPhysicalDevice().getFeatures();
    if (!features.vertexPipelineStoresAndAtomics) {
        std::printf("Object motion: vertexPipelineStoresAndAtomics unsupported, off\n");
        return;
    }
    void* mapped = nullptr;
    Shader::MotionVectors::position_stride = compact_history ? 12 : 16;
    u64 params_address = 0, positions_address = 0;
    if (!CreateBuffer(instance, vk::DeviceSize(1 + FrameSlots * ParamsPerFrame) * 32, true,
                      params_buffer, params_allocation, &mapped, params_address) ||
        !CreateBuffer(instance, vk::DeviceSize(1 + 2 * PositionsPerFrame) *
                          Shader::MotionVectors::position_stride, false,
                      positions_buffer, positions_allocation, nullptr, positions_address)) {
        std::printf("Object motion: buffer creation failed, off\n");
        return;
    }
    params_mapped = static_cast<u32*>(mapped);
    std::memset(params_mapped, 0, 32);
    Shader::MotionVectors::params_address = params_address;
    Shader::MotionVectors::positions_address = positions_address;
    enabled = true;
    std::printf("Object motion: on (%u vertices per frame, %u bytes/vertex, %.2f MiB fixed pool; diagnostics %s)\n",
                PositionsPerFrame, Shader::MotionVectors::position_stride,
                double(1 + 2 * PositionsPerFrame) * Shader::MotionVectors::position_stride / (1024 * 1024),
                diagnostics ? "on" : "off");
}

ObjectMotion::~ObjectMotion() {
    scheduler.Finish();
    const auto allocator = instance.GetAllocator();
    if (params_buffer) {
        vmaDestroyBuffer(allocator, params_buffer, params_allocation);
    }
    if (positions_buffer) {
        vmaDestroyBuffer(allocator, positions_buffer, positions_allocation);
    }
}

void ObjectMotion::OnFrameStart() {
    if (!enabled) {
        return;
    }
    peak_vertices = std::max(peak_vertices, history.Used());
    peak_params = std::max(peak_params, params_used);
    if ((BbStats::enabled || diagnostics) && frame && (frame % 300) == 0) {
        const auto& s = history.stats;
        std::printf("Object motion: %llu draws, %llu stored, %llu with history, %llu unmatched, "
                    "%llu capacity skips, %llu invalid, %llu still (300 frames); last frame %u/%u vertices; "
                    "index ranges %llu reused, %llu scanned (%llu found changed), %zu cached\n",
                    (unsigned long long)s.draws, (unsigned long long)s.stored,
                    (unsigned long long)s.loaded, (unsigned long long)s.unmatched,
                    (unsigned long long)s.exhausted, (unsigned long long)s.invalid,
                    (unsigned long long)s.still,
                    history.Used(), PositionsPerFrame, (unsigned long long)index_ranges.stats.hits,
                    (unsigned long long)index_ranges.stats.scans,
                    (unsigned long long)index_ranges.stats.stale, index_ranges.Size());
        index_ranges.stats = {};
        history.stats = {};
        std::printf("Object motion pool: peak %u/%u vertices, peak %u/%u params, %llu param skips, %zu attachments, %llu retired; old-location entries expire each frame\n",
                    peak_vertices, PositionsPerFrame, peak_params, ParamsPerFrame,
                    (unsigned long long)param_skips, images.size(), (unsigned long long)retired_images);
        peak_vertices = peak_params = 0;
        param_skips = retired_images = 0;
        for (const auto& [shader, invalid] : invalid_by_shader) {
            const auto& draw = invalid.example;
            std::printf("Object motion invalid: vs %016llx count %llu, range %u+%u, indices %u, instances %u first %u, topology %016llx; history unavailable, not capacity overflow\n",
                        (unsigned long long)shader, (unsigned long long)invalid.count,
                        draw.vertices.first, draw.vertices.count, draw.index_count,
                        draw.instances, draw.first_instance, (unsigned long long)draw.topology);
        }
        invalid_by_shader.clear();
    }
    history.NextFrame();
    ++frame;
    if (frame % Motion::IndexRangeCache::Unused == 0) {
        index_ranges.Trim(frame);
    }
    const u32 slot = u32(frame % FrameSlots);
    // Do not overwrite host parameters still referenced by queued GPU work.
    if (params_ticks[slot]) {
        scheduler.Wait(params_ticks[slot]);
        params_ticks[slot] = 0;
    }
    params_used = 0;
    frame_time = std::chrono::steady_clock::now();
    read_image = nullptr;
    for (auto it = images.begin(); it != images.end();) {
        if (frame - it->second->last_frame > 600 ||
            (compact_history && frame_time - it->second->last_use > std::chrono::seconds(10))) {
            // A settings/resolution change need not retain unused attachments forever.
            scheduler.DeferOperation([target = std::move(it->second)] {});
            it = images.erase(it);
            ++retired_images;
        } else {
            it->second->written = false;
            ++it;
        }
    }
    scheduler.EndRendering();
    // Last frame's stores must be visible to this frame's loads. Also finish old reads
    // before the ping-pong half they reference is reused for writes.
    scheduler.Record([](vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier});
    });
}

u32 ObjectMotion::PrepareDraw(const DrawInfo& draw) {
    if (!enabled || BbToggle::Disabled(1u << 29)) {
        return 0;
    }
    if (params_used >= ParamsPerFrame) { ++param_skips; return 0; }
    const auto invalid_before = history.stats.invalid;
    const auto allocation = history.Prepare(draw);
    if (diagnostics && history.stats.invalid != invalid_before &&
        (invalid_by_shader.size() < 32 || invalid_by_shader.contains(draw.shader))) {
        auto& invalid = invalid_by_shader[draw.shader];
        ++invalid.count;
        invalid.example = draw;
    }
    if(FrameCapture::Active()) {
        static const bool details=[] {
            const char* value=std::getenv("BB_CAPTURE_DRAW_DETAILS");
            return value && value[0]=='1';
        }();
        if(details) {
            const auto note=std::format("\n  OBJECT_HISTORY frame {} vs {:016x} geometry {:016x} index_address {:#x} topology {:016x} count {} instances {} first_instance {} range {}+{} store {} load {} allocated_vertices {}",
                frame,draw.shader,draw.geometry,draw.indices,draw.topology,draw.index_count,
                draw.instances,draw.first_instance,draw.vertices.first,draw.vertices.count,
                allocation.store,allocation.load,allocation.vertices);
            FrameCapture::Note(note.c_str());
        }
    }
    u32 flags = (allocation.store ? Shader::MotionVectors::FlagStore : 0) |
                (allocation.load ? Shader::MotionVectors::FlagLoad : 0);
    if (!flags) {
        return 0;
    }
    const u32 slot = u32(frame % FrameSlots);
    const u32 index = 1 + slot * ParamsPerFrame + params_used++;
    u32* param = params_mapped + index * 8;
    param[0] = allocation.store;
    param[1] = allocation.load;
    param[2] = allocation.vertices;
    param[3] = flags;
    param[4] = allocation.first_vertex;
    param[5] = allocation.first_instance;
    param[6] = allocation.instances;
    param[7] = 0;
    params_ticks[slot] = scheduler.CurrentTick();
    return index;
}

ObjectMotion::MotionImage& ObjectMotion::EnsureImage(u32 width, u32 height) {
    auto& target = images[{width, height}];
    if (target) {
        target->last_frame = frame;
        target->last_use = frame_time;
        return *target;
    }
    target = std::make_unique<MotionImage>();
    target->last_frame = frame;
    target->last_use = frame_time;
    const auto device = instance.GetDevice();
    auto& image = target->image;
    image = VideoCore::UniqueImage(device, instance.GetAllocator());
    image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR32G32B32A32Sfloat,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    target->view = Check(device.createImageViewUnique({
        .image = vk::Image(image),
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR32G32B32A32Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
    return *target;
}

void ObjectMotion::Attach(RenderState& state, u32 width, u32 height) {
    constexpr u32 slot = Shader::MotionVectors::Output;
    auto& target = EnsureImage(width, height);
    const auto& image = target.image;
    const auto& view = target.view;
    auto& written = target.written;
    auto& image_layout = target.layout;
    auto& attachment = state.color_attachments[slot];
    attachment = {};
    attachment.image_view = *view;
    attachment.image_layout = vk::ImageLayout::eColorAttachmentOptimal;
    if (!written || image_layout != vk::ImageLayout::eColorAttachmentOptimal) {
        // Clear once per frame; later reads must not erase the validity of this history.
        scheduler.EndRendering();
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                             vk::AccessFlagBits2::eColorAttachmentRead,
            .oldLayout = image_layout,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = vk::Image(image),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        // Recorded in order on the recording thread: CommandBuffer() here switched the rest of
        // the submission (most of the G-buffer) to direct recording on the GPU thread.
        scheduler.Record([barrier](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        });
        image_layout = vk::ImageLayout::eColorAttachmentOptimal;
        attachment.is_clear = !written;
        written = true;
    }
    state.num_color_attachments = std::max<u16>(state.num_color_attachments, slot + 1);
}

vk::ImageView ObjectMotion::PrepareRead(vk::CommandBuffer cmdbuf, u32 width, u32 height,
                                        bool& valid) {
    // A missing size uses a tiny valid descriptor without allocating a full attachment.
    const auto found = images.find({width, height});
    auto& target = found != images.end() ? *found->second : EnsureImage(1, 1);
    read_image = &target;
    valid = found != images.end() && target.written;
    const auto& image = target.image;
    auto& image_layout = target.layout;
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = image_layout,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = vk::Image(image),
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    image_layout = vk::ImageLayout::eGeneral;
    return *target.view;
}

} // namespace Vulkan
