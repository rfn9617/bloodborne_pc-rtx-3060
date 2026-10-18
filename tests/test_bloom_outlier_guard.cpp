// Off-screen replay of the exact original and guarded bloom fragment programs.
#include <cassert>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <vector>
#ifdef _WIN32
#define NDEBUG // Match the renderer's Vulkan-Hpp dispatcher layout.
#endif
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/renderer_vulkan/bloom_outlier_guard_profiles.h"
#include "video_core/renderer_vulkan/material_uniforms.h"
#include "shader_recompiler/info.h"
#include <vk_mem_alloc.h>
#ifdef _WIN32
#undef NDEBUG
#include <cassert>
#endif

int main(int argc,char** argv) {
    using namespace Vulkan;
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::puts("Bloom test: initializing Vulkan");
    assert(argc==4);
    Instance instance(0,false);
    Scheduler scheduler(instance);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance());d.init(instance.GetDevice());
    const auto device=instance.GetDevice();
    const float timestamp_period=instance.GetPhysicalDevice().getProperties(d).limits.timestampPeriod;
    auto queries=device.createQueryPoolUnique({.queryType=vk::QueryType::eTimestamp,.queryCount=2},nullptr,d).value;
    std::array<std::vector<double>,2> times;
    std::puts("Bloom test: creating images");
    constexpr u32 W=1280,H=720,N=W*H,OW=W/2,OH=H/2;
    constexpr size_t InputBytes=N*16,OutputBytes=OW*OH*16,Constants=InputBytes+OutputBytes;
    std::array<VideoCore::UniqueImage,2> images;
    std::array<vk::UniqueImageView,2> views;
    for(u32 i=0;i<2;++i) {
        images[i]=VideoCore::UniqueImage(device,instance.GetAllocator());
        images[i].Create({.imageType=vk::ImageType::e2D,.format=vk::Format::eR32G32B32A32Sfloat,
            .extent={i?OW:W,i?OH:H,1},.mipLevels=1,.arrayLayers=1,.samples=vk::SampleCountFlagBits::e1,
            .tiling=vk::ImageTiling::eOptimal,.usage=vk::ImageUsageFlagBits::eSampled|
            vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst});
        views[i]=device.createImageViewUnique({.image=vk::Image(images[i]),.viewType=vk::ImageViewType::e2D,
            .format=vk::Format::eR32G32B32A32Sfloat,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}},nullptr,d).value;
    }
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{0,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eFragment},
        vk::DescriptorSetLayoutBinding{1,vk::DescriptorType::eSampledImage,1,vk::ShaderStageFlagBits::eFragment},
        vk::DescriptorSetLayoutBinding{2,vk::DescriptorType::eSampler,1,vk::ShaderStageFlagBits::eFragment}};
    auto desc=device.createDescriptorSetLayoutUnique({.flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount=3,.pBindings=bindings.data()},nullptr,d).value;
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eFragment,0,128};
    auto layout=device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*desc,
        .pushConstantRangeCount=1,.pPushConstantRanges=&push},nullptr,d).value;
    std::ifstream vf(argv[1],std::ios::binary|std::ios::ate);assert(vf);
    std::vector<u32> vs(size_t(vf.tellg())/4);vf.seekg(0);vf.read(reinterpret_cast<char*>(vs.data()),vs.size()*4);assert(vf);
    auto vertex=device.createShaderModuleUnique({.codeSize=vs.size()*4,.pCode=vs.data()},nullptr,d).value;
    const std::array<std::span<const u32>,2> codes{BloomOutlierGuard::source,BloomOutlierGuard::guarded};
    assert(!BloomOutlierGuard::Match(codes[0]).empty());
    auto changed=std::vector<u32>(codes[0].begin(),codes[0].end());changed.back()^=1;
    assert(BloomOutlierGuard::Match(changed).empty());
    std::array<vk::UniqueShaderModule,2> fragments;
    std::array<vk::UniquePipeline,2> pipelines;
    const vk::Format format=vk::Format::eR32G32B32A32Sfloat;
    const vk::PipelineRenderingCreateInfo rendering{.colorAttachmentCount=1,.pColorAttachmentFormats=&format};
    const vk::PipelineVertexInputStateCreateInfo input{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology=vk::PrimitiveTopology::eTriangleList};
    const vk::PipelineViewportStateCreateInfo viewport{.viewportCount=1,.scissorCount=1};
    const vk::PipelineRasterizationStateCreateInfo raster{.lineWidth=1.f};
    const vk::PipelineMultisampleStateCreateInfo ms{.rasterizationSamples=vk::SampleCountFlagBits::e1};
    const vk::PipelineColorBlendAttachmentState blend{.colorWriteMask=vk::ColorComponentFlagBits::eR|vk::ColorComponentFlagBits::eG|vk::ColorComponentFlagBits::eB|vk::ColorComponentFlagBits::eA};
    const vk::PipelineColorBlendStateCreateInfo blending{.attachmentCount=1,.pAttachments=&blend};
    const std::array dynamics{vk::DynamicState::eViewport,vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{.dynamicStateCount=2,.pDynamicStates=dynamics.data()};
    Shader::Info bloom_info{};
    bloom_info.sw_stage=Shader::SwStage::Fragment;
    bloom_info.pgm_hash=0x4b766f62;
    const auto production=MaterialUniforms::Compile(bloom_info,0,codes[0],instance);
    for(u32 i=0;i<2;++i) {
        fragments[i]=device.createShaderModuleUnique({.codeSize=codes[i].size_bytes(),.pCode=codes[i].data()},nullptr,d).value;
        std::printf("Bloom test: creating pipeline %u\n",i);
        const std::array stages{
            vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eVertex,.module=*vertex,.pName="main"},
            vk::PipelineShaderStageCreateInfo{.stage=vk::ShaderStageFlagBits::eFragment,.module=i?production:*fragments[i],.pName="main"}};
        pipelines[i]=device.createGraphicsPipelineUnique({}, {.pNext=&rendering,.stageCount=2,.pStages=stages.data(),
            .pVertexInputState=&input,.pInputAssemblyState=&assembly,.pViewportState=&viewport,.pRasterizationState=&raster,
            .pMultisampleState=&ms,.pColorBlendState=&blending,.pDynamicState=&dynamic,.layout=*layout},nullptr,d).value;
    }
    device.destroyShaderModule(production,nullptr,d);
    auto sampler=device.createSamplerUnique({.magFilter=vk::Filter::eLinear,.minFilter=vk::Filter::eLinear,
        .mipmapMode=vk::SamplerMipmapMode::eNearest,.addressModeU=vk::SamplerAddressMode::eClampToEdge,
        .addressModeV=vk::SamplerAddressMode::eClampToEdge,.addressModeW=vk::SamplerAddressMode::eClampToEdge},nullptr,d).value;
    VkBuffer buffer{};VmaAllocation allocation{};VmaAllocationInfo ai{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=Constants+4096,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage=VMA_MEMORY_USAGE_AUTO,.requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(),&bi,&ac,&buffer,&allocation,&ai)==VK_SUCCESS);
    std::memset(static_cast<char*>(ai.pMappedData)+Constants,0,4096);
    auto* constants=reinterpret_cast<float*>(static_cast<char*>(ai.pMappedData)+Constants);
    for(u32 i=616;i<620;++i) constants[i]=1.f;
    bool initialized=false;
    const auto run=[&](const std::vector<float>& pixels,u32 shader) {
        std::memcpy(ai.pMappedData,pixels.data(),InputBytes);
        const auto cmd=scheduler.CommandBuffer();
        for(u32 i=0;i<2;++i) {
            const vk::ImageMemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout=initialized?vk::ImageLayout::eGeneral:vk::ImageLayout::eUndefined,
                .newLayout=vk::ImageLayout::eGeneral,.image=images[i],.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b},d);
        }
        const auto barrier=[&] {
            const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite|vk::AccessFlagBits2::eMemoryRead,
                .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask=vk::AccessFlagBits2::eMemoryWrite|vk::AccessFlagBits2::eMemoryRead};
            cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b},d);
        };
        const vk::BufferImageCopy upload{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={W,H,1}};
        cmd.copyBufferToImage(buffer,images[0],vk::ImageLayout::eGeneral,upload,d);barrier();
        const vk::RenderingAttachmentInfo attachment{.imageView=*views[1],.imageLayout=vk::ImageLayout::eGeneral,
            .loadOp=vk::AttachmentLoadOp::eDontCare,.storeOp=vk::AttachmentStoreOp::eStore};
        cmd.resetQueryPool(*queries,0,2,d);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe,*queries,0,d);
        cmd.beginRendering({.renderArea={{0,0},{OW,OH}},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&attachment},d);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,*pipelines[shader],d);
        cmd.setViewport(0,vk::Viewport{0,0,float(OW),float(OH),0,1},d);cmd.setScissor(0,vk::Rect2D{{0,0},{OW,OH}},d);
        std::array<u32,32> push_data{};cmd.pushConstants(*layout,vk::ShaderStageFlagBits::eFragment,0,128,push_data.data(),d);
        const vk::DescriptorBufferInfo cb{buffer,Constants,4096};
        const vk::DescriptorImageInfo img{{},*views[0],vk::ImageLayout::eGeneral},smpl{*sampler,{},vk::ImageLayout::eGeneral};
        const std::array writes{
            vk::WriteDescriptorSet{.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&cb},
            vk::WriteDescriptorSet{.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&img},
            vk::WriteDescriptorSet{.dstBinding=2,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampler,.pImageInfo=&smpl}};
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics,*layout,0,writes,d);cmd.draw(3,1,0,0,d);cmd.endRendering(d);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe,*queries,1,d);barrier();
        const vk::BufferImageCopy download{.bufferOffset=InputBytes,.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={OW,OH,1}};
        cmd.copyImageToBuffer(images[1],vk::ImageLayout::eGeneral,buffer,download,d);
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host},d);scheduler.Finish();initialized=true;
        std::array<u64,2> ticks{};
        const auto status=device.getQueryPoolResults(*queries,0,2,sizeof(ticks),ticks.data(),sizeof(u64),vk::QueryResultFlagBits::e64|vk::QueryResultFlagBits::eWait,d);
        assert(status==vk::Result::eSuccess);
        times[shader].push_back(double(ticks[1]-ticks[0])*timestamp_period/1000000.0);
        auto* result=reinterpret_cast<float*>(static_cast<char*>(ai.pMappedData)+InputBytes);
        return std::vector<float>(result,result+OW*OH*4);
    };
    std::vector<float> pixels(N*4,1.f);
    for(float value:{2.f,100.f}) {
        for(u32 p=0;p<N;++p){pixels[p*4]=value;pixels[p*4+1]=value*.5f;pixels[p*4+2]=value*.25f;pixels[p*4+3]=.375f;}
        const auto a=run(pixels,0),b=run(pixels,1);for(size_t i=0;i<a.size();++i)assert(a[i]==b[i]);
    }
    for(u32 p=0;p<N;++p){pixels[p*4]=.2f;pixels[p*4+1]=.1f;pixels[p*4+2]=.05f;pixels[p*4+3]=.375f;}
    pixels[(200*W+400)*4]=1500.f;pixels[(200*W+400)*4+1]=800.f;pixels[(200*W+400)*4+2]=400.f;
    const auto a=run(pixels,0),b=run(pixels,1);const u32 at=(100*OW+200)*4;
    assert(a[at]>370 && b[at]<8.2f && b[at]>7.9f);for(size_t i=3;i<a.size();i+=4)assert(a[i]==b[i]);
    std::printf("GPU synthetic PASS: uniform/dense bright fields unchanged; isolated 1500 -> bloom sample %.6f to %.6f; alpha unchanged\n",a[at],b[at]);
    pixels[(200*W+400)*4]=80.f;pixels[(200*W+400)*4+1]=40.f;pixels[(200*W+400)*4+2]=20.f;
    const auto weak_a=run(pixels,0),weak_b=run(pixels,1);
    const bool v2=std::getenv("BB_BLOOM_OUTLIER_GUARD") && std::getenv("BB_BLOOM_OUTLIER_GUARD")[0]=='2';
    assert(weak_a[at]>20.f && weak_a[at]<21.f);
    assert(v2 ? weak_b[at]>7.9f && weak_b[at]<8.2f : weak_b[at]==weak_a[at]);
    std::printf("GPU diluted source outlier PASS: v%u red=80 -> original %.6f guarded %.6f\n",v2?2u:1u,weak_a[at],weak_b[at]);
    // Preserve a multi-pixel bright flame instead of clipping it like an isolated spike.
    for(u32 y=199;y<=201;++y)for(u32 x=399;x<=401;++x) {
        pixels[(y*W+x)*4]=80.f;pixels[(y*W+x)*4+1]=40.f;pixels[(y*W+x)*4+2]=20.f;
    }
    const auto flame_a=run(pixels,0),flame_b=run(pixels,1);
    assert(flame_a==flame_b);
    std::puts("GPU multi-pixel bright flame PASS: unchanged");
    std::ifstream recorded(argv[2],std::ios::binary);std::ofstream output(argv[3],std::ios::binary);assert(recorded&&output);
    const auto half=[](u16 v){const int e=(v>>10)&31;return (v&0x8000?-1.f:1.f)*(e==0?std::ldexp(float(v&1023),-24):std::ldexp(1.f+float(v&1023)/1024.f,e-15));};
    std::vector<u16> roi(640*360*4);u32 cases=0;
    while(recorded.read(reinterpret_cast<char*>(roi.data()),roi.size()*2)) {
        std::fill(pixels.begin(),pixels.end(),0.f);
        for(u32 y=0;y<360;++y)for(u32 x=0;x<640;++x)for(u32 c=0;c<4;++c)pixels[(y*W+x+320)*4+c]=half(roi[(y*640+x)*4+c]);
        const auto original=run(pixels,0),guarded=run(pixels,1);
        for(u32 mode=0;mode<2;++mode) {
            const auto& r=mode?guarded:original;
            for(u32 y=0;y<180;++y)output.write(reinterpret_cast<const char*>(r.data()+(y*OW+160)*4),320*16);
        }
        ++cases;
    }
    assert(recorded.eof()&&cases==5);std::printf("GPU captured HDR replay PASS: %u original/guarded pairs written\n",cases);
    // Timestamp only drawing, excluding uploads/readbacks; warm the last captured input.
    times[0].clear();times[1].clear();
    for(u32 i=0;i<12;++i){run(pixels,0);run(pixels,1);}
    for(u32 mode=0;mode<2;++mode){auto t=times[mode];t.erase(t.begin(),t.begin()+2);std::sort(t.begin(),t.end());
        std::printf("GPU prefilter-only timing: %s median %.6f ms (%zu warm samples, captured ROI padded to 1280x720)\n",mode?"guard":"original",(t[4]+t[5])*.5,t.size());}
    scheduler.Finish();vmaDestroyBuffer(instance.GetAllocator(),buffer,allocation);
}
