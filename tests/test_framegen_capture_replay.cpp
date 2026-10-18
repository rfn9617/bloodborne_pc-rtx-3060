// Replay saved full-screen color/motion/depth through the production generator.
// Reset after capture gaps. This compares variants; it is not a reconstruction
// of the game's uncaptured optical-flow history or camera parameters.
#ifndef NDEBUG
#define NDEBUG
#endif
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include "video_core/renderer_vulkan/vk_frame_generator.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>
#include "bbport_toggles.h"
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::abort();}} while(0)
int main(int argc,char** argv) {
    using namespace Vulkan;
    CHECK(argc==3);std::ifstream in(argv[1],std::ios::binary);CHECK(in);
    std::filesystem::create_directories(argv[2]);
    std::array<u32,4> header{};in.read(reinterpret_cast<char*>(header.data()),sizeof(header));
    CHECK(header[0]==0x31475246 && header[1]==1280 && header[2]==720);
    Instance instance(0,true);Scheduler scheduler(instance);FrameGenerator fg(instance,scheduler);
    static vk::detail::DynamicLoader loader;vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());dispatch.init(instance.GetDevice());
    const auto device=instance.GetDevice();auto allocator=instance.GetAllocator();
    constexpr u64 Color=1920ull*1080*4,Small=1280ull*720*4,Upload=Color+Small*2;
    VideoCore::UniqueImage color(device,allocator),motion(device,allocator),depth(device,allocator);
    const auto create=[&](auto& image,u32 w,u32 h,vk::Format format) {
        image.Create({.imageType=vk::ImageType::e2D,.format=format,.extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
            .usage=vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc});
    };
    create(color,1920,1080,vk::Format::eR8G8B8A8Unorm);create(motion,1280,720,vk::Format::eR16G16Sfloat);create(depth,1280,720,vk::Format::eR32Sfloat);
    const auto view=[&](auto& image){return Check(device.createImageViewUnique({.image=image.image,.viewType=vk::ImageViewType::e2D,.format=image.image_ci.format,
        .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}));};
    auto color_view=view(color),depth_view=view(depth);
    VkBuffer host{};VmaAllocation allocation{};VmaAllocationInfo mapped{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=Upload+Color,.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ai{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage=VMA_MEMORY_USAGE_AUTO,.requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    CHECK(vmaCreateBuffer(allocator,&bi,&ai,&host,&allocation,&mapped)==VK_SUCCESS);
    const auto barrier=[&](auto cmd,vk::Image image,vk::ImageLayout old,vk::ImageLayout next) {
        const vk::ImageMemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,.srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,.dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout=old,.newLayout=next,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,
            .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b});
    };
    const std::array<std::array<float,3>,4> camera{{{0,0,0},{0,1,0},{1,0,0},{0,0,1}}};
    for(u32 f=0;f<header[3];++f) {
        std::array<u32,2> item{};std::array<float,3> values{};
        in.read(reinterpret_cast<char*>(item.data()),sizeof(item));in.read(reinterpret_cast<char*>(values.data()),sizeof(values));
        in.read(static_cast<char*>(mapped.pMappedData),Upload);CHECK(in);
        vmaFlushAllocation(allocator,allocation,0,Upload);
        CHECK(fg.Prepare(1280,720));auto cmd=scheduler.CommandBuffer();u64 offset=0;
        for(auto* image:{&color,&motion,&depth}) {
            barrier(cmd,image->image,vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal);
            cmd.copyBufferToImage(host,image->image,vk::ImageLayout::eTransferDstOptimal,vk::BufferImageCopy{.bufferOffset=offset,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent=image->image_ci.extent},dispatch);
            barrier(cmd,image->image,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eGeneral);offset+=image==&color?Color:Small;
        }
        fg.Inputs(*depth_view,motion.image,1280,720,0.1f,1.0472f,{values[1],values[2]},camera,values[0],item[1]!=0);
        fg.CaptureScene(*color_view,vk::ImageLayout::eGeneral,1920,1080);fg.Boundary();
        fg.GenerateFinal(*color_view,*color_view,vk::ImageLayout::eGeneral,1920,1080);
        const auto output=fg.Compose(*color_view,vk::ImageLayout::eGeneral,false);
        CHECK(bool(output)==!bool(item[1]));
        if(output) {
            auto raw=fg.GetDiagnosticInputs().interpolated;CHECK(raw);
            barrier(cmd,raw,vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferSrcOptimal);
            cmd.copyImageToBuffer(raw,vk::ImageLayout::eTransferSrcOptimal,host,vk::BufferImageCopy{.bufferOffset=Upload,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={1920,1080,1}},dispatch);
            barrier(cmd,raw,vk::ImageLayout::eTransferSrcOptimal,vk::ImageLayout::eGeneral);
            const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b});
        }
        scheduler.Finish();
        if(output) {
            CHECK(vmaInvalidateAllocation(allocator,allocation,Upload,Color)==VK_SUCCESS);
            char name[64];std::snprintf(name,sizeof(name),"frame-%06u.bin",item[0]);
            std::ofstream file(std::filesystem::path(argv[2])/name,std::ios::binary);
            file.write(static_cast<char*>(mapped.pMappedData)+Upload,Color);CHECK(file);
        }
    }
    fg.Stop();vmaDestroyBuffer(allocator,host,allocation);
    std::puts("PASS: production interpolation replay completed");
}
