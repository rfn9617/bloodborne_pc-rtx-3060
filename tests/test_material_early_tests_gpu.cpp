// SPDX-License-Identifier: GPL-2.0-or-later
// Headless Vulkan: compares all six MRT outputs, depth and stencil for both modes.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "video_core/renderer_vulkan/material_early_tests.h"
#include "video_core/renderer_vulkan/material_clip_fusion.h"

#define CHECK(x) do { VkResult result = (x); if(result != VK_SUCCESS) { std::fprintf(stderr,"Vulkan failure %d at line %d\n",result,__LINE__); return 1; } } while(0)
static std::vector<std::uint32_t> Read(const char* path) {
    std::ifstream file(path,std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
    assert(file && bytes.size()%4==0);
    std::vector<std::uint32_t> words(bytes.size()/4);
    std::memcpy(words.data(),bytes.data(),bytes.size());
    return words;
}

int main(int argc,char** argv) {
    assert(argc==3 || argc==4);
    const bool clip_fusion=argc==4 && std::strcmp(argv[3],"clip")==0;
    std::setbuf(stdout,nullptr);
    const auto vs_code=Read(argv[1]), fs_code=Read(argv[2]);
    std::vector<std::uint32_t> early;
    assert(clip_fusion?Vulkan::MaterialClipFusion::Convert(fs_code,early):Vulkan::MaterialEarlyTests::Convert(fs_code,early));
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr,"material regression",1,"test",1,VK_API_VERSION_1_3};
    const VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,nullptr,0,&app};
    VkInstance instance;
    CHECK(vkCreateInstance(&ic,nullptr,&instance));
    std::uint32_t count=0;
    CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr));
    assert(count);
    std::vector<VkPhysicalDevice> physical(count);
    CHECK(vkEnumeratePhysicalDevices(instance,&count,physical.data()));
    auto gpu=physical[0];
    for(auto device:physical) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(device,&p);
        if(p.vendorID==0x10de) gpu=device;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu,&props);
    std::printf("GPU: %s; six MRT color attachments, D32S8 depth/stencil\n",props.deviceName);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(gpu,&memory);
    auto memory_type=[&](std::uint32_t bits,VkMemoryPropertyFlags flags) {
        for(std::uint32_t i=0;i<memory.memoryTypeCount;++i)
            if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) return i;
        assert(false); return 0u;
    };
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,families.data());
    std::uint32_t family=0;
    while(!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family;
    float priority=1;
    const VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,nullptr,0,family,1,&priority};
    VkPhysicalDeviceFeatures features{};
    features.independentBlend=VK_TRUE;
    const VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures demote_feature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,nullptr,VkBool32(clip_fusion)};
    const VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&demote_feature,0,1,&qc,0,nullptr,0,nullptr,&features};
    VkDevice device;
    CHECK(vkCreateDevice(gpu,&dc,nullptr,&device));
    VkQueue queue;
    vkGetDeviceQueue(device,family,0,&queue);
    constexpr std::uint32_t width=64,height=64,pixels=width*height;
    constexpr VkDeviceSize color_size=pixels*16, depth_size=pixels*4, stencil_size=pixels;
    constexpr VkDeviceSize output_size=6*color_size+depth_size+stencil_size;
    const VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,nullptr,0,output_size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer readback;
    CHECK(vkCreateBuffer(device,&bc,nullptr,&readback));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device,readback,&mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,nullptr,mr.size,memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory read_memory;
    CHECK(vkAllocateMemory(device,&ma,nullptr,&read_memory));
    CHECK(vkBindBufferMemory(device,readback,read_memory,0));
    void* mapped;
    CHECK(vkMapMemory(device,read_memory,0,output_size,0,&mapped));
    std::array<VkImage,7> images;
    std::array<VkDeviceMemory,7> image_memory;
    std::array<VkImageView,7> views;
    std::array<VkAttachmentDescription,7> attachments;
    std::array<VkAttachmentReference,6> references;
    for(unsigned i=0;i<7;++i) {
        const bool depth=i==6;
        const auto format=depth?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R32G32B32A32_SFLOAT;
        const auto aspect=depth?VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
        const VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,nullptr,0,VK_IMAGE_TYPE_2D,format,{width,height,1},1,1,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_TILING_OPTIMAL,
            VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT|(depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)),VK_SHARING_MODE_EXCLUSIVE,0,nullptr,VK_IMAGE_LAYOUT_UNDEFINED};
        CHECK(vkCreateImage(device,&image,nullptr,&images[i]));
        vkGetImageMemoryRequirements(device,images[i],&mr);
        ma.allocationSize=mr.size;
        ma.memoryTypeIndex=memory_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CHECK(vkAllocateMemory(device,&ma,nullptr,&image_memory[i]));
        CHECK(vkBindImageMemory(device,images[i],image_memory[i],0));
        const VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,nullptr,0,images[i],VK_IMAGE_VIEW_TYPE_2D,format,{}, {VkImageAspectFlags(aspect),0,1,0,1}};
        CHECK(vkCreateImageView(device,&vi,nullptr,&views[i]));
        attachments[i]={0,format,VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,
            depth?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_DONT_CARE,depth?VK_ATTACHMENT_STORE_OP_STORE:VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        if(!depth) references[i]={i,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    const VkAttachmentReference depth_ref{6,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkSubpassDescription subpass{0,VK_PIPELINE_BIND_POINT_GRAPHICS,0,nullptr,6,references.data(),nullptr,&depth_ref};
    const std::array<VkSubpassDependency,2> dependencies{{
        {VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,0},
        {0,VK_SUBPASS_EXTERNAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,0}}};
    const VkRenderPassCreateInfo rc{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,nullptr,0,7,attachments.data(),1,&subpass,2,dependencies.data()};
    VkRenderPass render_pass;
    CHECK(vkCreateRenderPass(device,&rc,nullptr,&render_pass));
    const VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,nullptr,0,render_pass,7,views.data(),width,height,1};
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(device,&fc,nullptr,&fb));
    VkShaderModule vs, fs[2];
    VkShaderModuleCreateInfo sc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,nullptr,0,vs_code.size()*4,vs_code.data()};
    CHECK(vkCreateShaderModule(device,&sc,nullptr,&vs));
    sc.codeSize=fs_code.size()*4; sc.pCode=fs_code.data();
    CHECK(vkCreateShaderModule(device,&sc,nullptr,&fs[0]));
    sc.codeSize=early.size()*4; sc.pCode=early.data();
    CHECK(vkCreateShaderModule(device,&sc,nullptr,&fs[1]));
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,8};
    const VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,0,nullptr,1,&push};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device,&lc,nullptr,&layout));
    const VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,nullptr,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,family};
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device,&pc,nullptr,&pool));
    const VkCommandBufferAllocateInfo ac{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,nullptr,pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device,&ac,&cmd));
    const VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,nullptr,0,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,VK_FALSE};
    const VkViewport viewport{0,0,float(width),float(height),0,1};
    const VkRect2D rect{{0,0},{width,height}};
    const VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,nullptr,0,1,&viewport,1,&rect};
    const VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,nullptr,0,VK_FALSE,VK_FALSE,VK_POLYGON_MODE_FILL,VK_CULL_MODE_NONE,VK_FRONT_FACE_COUNTER_CLOCKWISE,VK_FALSE,0,0,0,1};
    const VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,nullptr,0,VK_SAMPLE_COUNT_1_BIT,VK_FALSE,0,nullptr,VK_FALSE,VK_FALSE};
    unsigned cases=0;
    for(auto compare:{VK_COMPARE_OP_LESS,VK_COMPARE_OP_GREATER,VK_COMPARE_OP_EQUAL,VK_COMPARE_OP_ALWAYS,VK_COMPARE_OP_NEVER})
    for(bool depth_write:{false,true})
    for(unsigned stencil_mode:{0u,1u,2u})
    for(bool blend:{false,true}) {
        VkPipeline pipelines[2];
        const VkStencilOpState stencil{VK_STENCIL_OP_KEEP,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_INCREMENT_AND_CLAMP,
            stencil_mode==2?VK_COMPARE_OP_NEVER:VK_COMPARE_OP_ALWAYS,0xff,0xff,3};
        const VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,nullptr,0,VK_TRUE,VkBool32(depth_write),compare,VK_FALSE,VkBool32(stencil_mode!=0),stencil,stencil,0,1};
        std::array<VkPipelineColorBlendAttachmentState,6> blending;
        for(auto& b:blending) b={VkBool32(blend),VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_OP_ADD,
            VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_ZERO,VK_BLEND_OP_ADD,0xf};
        const VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,nullptr,0,VK_FALSE,VK_LOGIC_OP_COPY,6,blending.data(),{}};
        for(unsigned v=0;v<2;++v) {
            const std::array<VkPipelineShaderStageCreateInfo,2> stages{{
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vs,"main",nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fs[v],"main",nullptr}}};
            const VkGraphicsPipelineCreateInfo gc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,nullptr,0,2,stages.data(),&vertex,&assembly,nullptr,&vp,&raster,&ms,&ds,&cb,nullptr,layout,render_pass,0,VK_NULL_HANDLE,-1};
            CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gc,nullptr,&pipelines[v]));
        }
        std::array<std::vector<std::uint8_t>,2> outputs;
        for(unsigned v=0;v<2;++v) {
            CHECK(vkResetCommandBuffer(cmd,0));
            const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            CHECK(vkBeginCommandBuffer(cmd,&begin));
            std::array<VkClearValue,7> clear{};
            for(unsigned i=0;i<6;++i) clear[i].color={{0.125f,0.25f,0.5f,1.0f}};
            clear[6].depthStencil={0.5f,1};
            const VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,render_pass,fb,rect,7,clear.data()};
            vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[v]);
            for(float z:{0.25f,0.75f,0.5f,0.25f}) {
                const std::array<float,2> constants{z,z*0.5f};
                vkCmdPushConstants(cmd,layout,push.stageFlags,0,8,constants.data());
                vkCmdDraw(cmd,3,1,0,0);
            }
            vkCmdEndRenderPass(cmd);
            for(unsigned i=0;i<8;++i) {
                const auto aspect=i<6?VK_IMAGE_ASPECT_COLOR_BIT:i==6?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_STENCIL_BIT;
                const auto offset=i<6?i*color_size:i==6?6*color_size:6*color_size+depth_size;
                const VkBufferImageCopy copy{offset,0,0,{VkImageAspectFlags(aspect),0,0,1},{0,0,0},{width,height,1}};
                vkCmdCopyImageToBuffer(cmd,images[std::min(i,6u)],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback,1,&copy);
            }
            const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
            CHECK(vkEndCommandBuffer(cmd));
            const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,0,nullptr,nullptr,1,&cmd,0,nullptr};
            CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));
            CHECK(vkQueueWaitIdle(queue));
            outputs[v].resize(output_size);
            std::memcpy(outputs[v].data(),mapped,output_size);
        }
        if(outputs[0]!=outputs[1]) {
            std::fprintf(stderr,"FAIL: compare=%d writes=%d stencil=%u blend=%d\n",compare,depth_write,stencil_mode,blend);
            return 1;
        }
        if(compare==VK_COMPARE_OP_LESS && !depth_write && stencil_mode==0 && !blend) {
            // Paired GPU timestamps on a small fixture. This measures whether
            // the driver already performs automatic early rejection; it does
            // not predict the cost of the game's actual material shader.
            assert(families[family].timestampValidBits);
            const VkQueryPoolCreateInfo query_ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,nullptr,0,VK_QUERY_TYPE_TIMESTAMP,2,0};
            VkQueryPool queries;
            CHECK(vkCreateQueryPool(device,&query_ci,nullptr,&queries));
            for(bool occluded:{true,false}) {
                std::array<std::vector<double>,2> timings;
                for(unsigned round=0;round<12;++round) for(unsigned order=0;order<2;++order) {
                    const unsigned v=order^(round&1);
                    CHECK(vkResetCommandBuffer(cmd,0));
                    const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,nullptr,VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                    CHECK(vkBeginCommandBuffer(cmd,&begin));
                    vkCmdResetQueryPool(cmd,queries,0,2);
                    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
                    std::array<VkClearValue,7> clear{};
                    clear[6].depthStencil={0.5f,1};
                    const VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,nullptr,render_pass,fb,rect,7,clear.data()};
                    vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE);
                    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelines[v]);
                    const std::array<float,2> constants{occluded?0.75f:0.25f,0.125f};
                    vkCmdPushConstants(cmd,layout,push.stageFlags,0,8,constants.data());
                    for(unsigned draw=0;draw<512;++draw) vkCmdDraw(cmd,3,1,0,0);
                    vkCmdEndRenderPass(cmd);
                    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);
                    CHECK(vkEndCommandBuffer(cmd));
                    const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,0,nullptr,nullptr,1,&cmd,0,nullptr};
                    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));
                    CHECK(vkQueueWaitIdle(queue));
                    std::array<std::uint64_t,2> ticks;
                    CHECK(vkGetQueryPoolResults(device,queries,0,2,sizeof(ticks),ticks.data(),sizeof(std::uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
                    if(round>=3) timings[v].push_back(double(ticks[1]-ticks[0])*props.limits.timestampPeriod/1e6);
                }
                for(auto& t:timings) std::sort(t.begin(),t.end());
                const double baseline=timings[0][timings[0].size()/2], test=timings[1][timings[1].size()/2];
                std::printf("GPU timing fixture: %s, baseline %.4f ms/batch, %s %.4f ms/batch, delta %.2f%%; 512 draws, 9 paired samples; not a game benchmark\n",
                    occluded?"occluded":"visible",baseline,clip_fusion?"clip fusion":"early",test,(test/baseline-1)*100);
            }
            vkDestroyQueryPool(device,queries,nullptr);
        }
        for(auto p:pipelines) vkDestroyPipeline(device,p,nullptr);
        ++cases;
    }
    std::printf("PASS: %u Vulkan cases; all six color attachments, depth and stencil match bit for bit\n",cases);
    vkDestroyCommandPool(device,pool,nullptr);
    vkDestroyPipelineLayout(device,layout,nullptr);
    vkDestroyShaderModule(device,vs,nullptr);
    for(auto f:fs) vkDestroyShaderModule(device,f,nullptr);
    vkDestroyFramebuffer(device,fb,nullptr);
    vkDestroyRenderPass(device,render_pass,nullptr);
    for(unsigned i=0;i<7;++i) {
        vkDestroyImageView(device,views[i],nullptr);
        vkDestroyImage(device,images[i],nullptr);
        vkFreeMemory(device,image_memory[i],nullptr);
    }
    vkUnmapMemory(device,read_memory);
    vkDestroyBuffer(device,readback,nullptr);
    vkFreeMemory(device,read_memory,nullptr);
    vkDestroyDevice(device,nullptr);
    vkDestroyInstance(instance,nullptr);
}
