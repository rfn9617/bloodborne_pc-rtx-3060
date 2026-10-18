// SPDX-License-Identifier: GPL-2.0-or-later
// Real production query lifecycle + sample writeback, without starting a game.
#define NDEBUG
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_occlusion.h"
#include "video_core/renderer_vulkan/occlusion_model.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/scene_resolution.h"
#include "video_core/texture_cache/image.h"
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>
extern "C" int runtime_test_fake_memory;
extern "C" uintptr_t runtime_test_memory_base;
extern "C" uint64_t runtime_test_memory_size;
static std::vector<uint32_t> Read(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);
    std::vector<uint32_t> v(size_t(f.tellg())/4);f.seekg(0);f.read((char*)v.data(),v.size()*4);assert(f);return v;
}
int main(int argc,char**argv) {
    assert(argc==3);std::setbuf(stdout,nullptr);
    using namespace Vulkan;
    Instance instance(0,false);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());dispatch.init(instance.GetDevice());
    VULKAN_HPP_DEFAULT_DISPATCHER=dispatch;
    Scheduler scheduler(instance);
    auto occlusion=Occlusion::Create(instance,scheduler);assert(occlusion);
    alignas(16) uint64_t block[16]{};
    runtime_test_fake_memory=3;runtime_test_memory_base=(uintptr_t)block;runtime_test_memory_size=sizeof(block);
    const auto vs=Read(argv[1]),fs=Read(argv[2]);
    const auto device=instance.GetDevice();
    auto vertex=device.createShaderModuleUnique({.codeSize=vs.size()*4,.pCode=vs.data()});assert(vertex.result==vk::Result::eSuccess);
    auto fragment=device.createShaderModuleUnique({.codeSize=fs.size()*4,.pCode=fs.data()});assert(fragment.result==vk::Result::eSuccess);
    auto layout=device.createPipelineLayoutUnique({});assert(layout.result==vk::Result::eSuccess);
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eVertex,.module=*vertex.value,.pName="main"},
        vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eFragment,.module=*fragment.value,.pName="main"}};
    vk::PipelineVertexInputStateCreateInfo vi{};
    vk::PipelineInputAssemblyStateCreateInfo ia{.topology=vk::PrimitiveTopology::eTriangleList};
    vk::Viewport viewport{0,0,32,32,0,1};vk::Rect2D scissor{{0,0},{32,32}};
    vk::PipelineViewportStateCreateInfo vp{.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    vk::PipelineRasterizationStateCreateInfo rs{.polygonMode=vk::PolygonMode::eFill,.cullMode=vk::CullModeFlagBits::eNone,.lineWidth=1};
    vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples=vk::SampleCountFlagBits::e1};
    vk::PipelineColorBlendStateCreateInfo blend{};
    vk::PipelineDepthStencilStateCreateInfo depth_test{.depthTestEnable=true,.depthCompareOp=vk::CompareOp::eGreater};
    VideoCore::UniqueImage depth(device,instance.GetAllocator());
    depth.Create({.imageType=vk::ImageType::e2D,.format=vk::Format::eD32Sfloat,.extent={32,32,1},
        .mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
        .usage=vk::ImageUsageFlagBits::eDepthStencilAttachment});
    auto depth_view=device.createImageViewUnique({.image=vk::Image(depth),.viewType=vk::ImageViewType::e2D,
        .format=vk::Format::eD32Sfloat,.subresourceRange={vk::ImageAspectFlagBits::eDepth,0,1,0,1}});
    assert(depth_view.result==vk::Result::eSuccess);
    const vk::ImageMemoryBarrier2 barrier{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
        .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask=vk::AccessFlagBits2::eDepthStencilAttachmentWrite|vk::AccessFlagBits2::eDepthStencilAttachmentRead,
        .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .image=vk::Image(depth),.subresourceRange={vk::ImageAspectFlagBits::eDepth,0,1,0,1}};
    scheduler.CommandBuffer().pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier},dispatch);
    vk::PipelineRenderingCreateInfo rendering{.depthAttachmentFormat=vk::Format::eD32Sfloat};
    vk::GraphicsPipelineCreateInfo desc{.pNext=&rendering,.stageCount=2,.pStages=stages.data(),
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,
        .pRasterizationState=&rs,.pMultisampleState=&ms,.pDepthStencilState=&depth_test,.pColorBlendState=&blend,.layout=*layout.value};
    auto pipeline=device.createGraphicsPipelineUnique({},desc);assert(pipeline.result==vk::Result::eSuccess);
    depth_test.depthCompareOp=vk::CompareOp::eLess; // fragment z=0.5 is behind depth=0.25
    auto hidden=device.createGraphicsPipelineUnique({},desc);assert(hidden.result==vk::Result::eSuccess);
    auto run=[&](bool visible,bool poison=false) {
        std::memset(block,0,sizeof(block));
        auto begin=occlusion->MakeDump((uintptr_t)block,8);occlusion->Dump(begin);
        occlusion->NoteIssued();
        RenderState state{};state.width=32;state.height=32;state.num_layers=1;
        state.depth_stencil_attachment.image_view=*depth_view.value;
        state.depth_stencil_attachment.image_layout=vk::ImageLayout::eDepthStencilAttachmentOptimal;
        state.depth_stencil_attachment.has_depth=true;state.depth_stencil_attachment.depth_clear=true;
        state.depth_stencil_attachment.clear_value[0]=std::bit_cast<u32>(0.25f);
        scheduler.BeginRendering(state);
        auto query=occlusion->BeginDraw(SceneResolution::Pack({32,32}));assert(query.pool);
        const auto command=scheduler.CommandBuffer();
        command.beginQuery(query.pool,query.index,query.flags,dispatch);
        command.bindPipeline(vk::PipelineBindPoint::eGraphics,visible?*pipeline.value:*hidden.value,dispatch);
        command.draw(3,1,0,0,dispatch);command.endQuery(query.pool,query.index,dispatch);
        if (poison) occlusion->NoteIssued(); // decoded draw was filtered: cannot report hidden
        occlusion->Dump(occlusion->MakeDump((uintptr_t)block+8,8));
        assert(occlusion->MayWriteGuestBuffer((uintptr_t)block,sizeof(block)));
        scheduler.Finish();
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(occlusion->MayWriteGuestBuffer((uintptr_t)block,sizeof(block))) {
            assert(std::chrono::steady_clock::now()<deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto result=OcclusionModel::ReadBlock(block,8);
        uint64_t sum=0;for(unsigned k=0;k<8;++k)sum+=(block[2*k+1]&~OcclusionModel::ValidBit)-(block[2*k]&~OcclusionModel::ValidBit);
        std::printf("GPU query visible=%d poison=%d: state=%u samples=%llu\n",visible,poison,unsigned(result),(unsigned long long)sum);
        return result;
    };
    using B=OcclusionModel::Block;
    assert(run(false)==B::Visible);assert(run(false)==B::Visible);assert(run(false)==B::Occluded);
    occlusion->SetPredication({(uintptr_t)block,1,1,0,0});assert(occlusion->SkipDraw());
    occlusion->SetPredication({(uintptr_t)block,0,1,0,0});assert(!occlusion->SkipDraw());
    assert(run(true)==B::Visible);
    assert(run(false,true)==B::Visible);
    assert(Dlss::QualityForScale(4.f/3.f)==1);
    assert(OcclusionModel::Samples(1,true,false,1<<16)==OcclusionModel::Unmeasured);
    scheduler.Finish();occlusion.reset();
    puts("Production occlusion GPU: query resolve/reset, scale, hysteresis, predication and fail-open PASS");
}
