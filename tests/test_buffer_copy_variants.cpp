// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone Vulkan correctness/throughput experiment, no game or saves.
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
#define CHECK(x) do { auto result=(x); if(result!=VK_SUCCESS) { std::fprintf(stderr,"Vulkan %d line %d\n",result,__LINE__); return 1; } } while(0)
int main(int argc, char** argv) {
    assert(argc==5); std::setbuf(stdout,nullptr);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr,"copy experiment",1,"test",1,VK_API_VERSION_1_3};
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,nullptr,0,&app};
    VkInstance instance; CHECK(vkCreateInstance(&ic,nullptr,&instance));
    std::uint32_t count=0; CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr)); assert(count);
    std::vector<VkPhysicalDevice> physical(count); CHECK(vkEnumeratePhysicalDevices(instance,&count,physical.data()));
    VkPhysicalDevice gpu=physical[0];
    for(auto p:physical) { VkPhysicalDeviceProperties prop;vkGetPhysicalDeviceProperties(p,&prop);if(prop.vendorID==0x10de)gpu=p; }
    VkPhysicalDeviceProperties prop; vkGetPhysicalDeviceProperties(gpu,&prop);
    std::printf("GPU %s; timestamp %.3f ns\n",prop.deviceName,prop.limits.timestampPeriod);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,nullptr);
    std::vector<VkQueueFamilyProperties> fam(count); vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,fam.data());
    std::uint32_t family=0;while(!(fam[family].queueFlags&VK_QUEUE_COMPUTE_BIT)||!fam[family].timestampValidBits)++family;
    float priority=1; VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,nullptr,0,family,1,&priority};
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,nullptr,0,1,&qc}; VkDevice device;CHECK(vkCreateDevice(gpu,&dc,nullptr,&device));
    VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties mem;vkGetPhysicalDeviceMemoryProperties(gpu,&mem);
    constexpr std::uint32_t MaxRegions=1024,Stride=2048,Words=MaxRegions*Stride+16;
    constexpr VkDeviceSize Bytes=Words*4;
    struct Buffer { VkBuffer handle;VkDeviceMemory memory;void* mapped; };
    std::array<Buffer,4> buffers{};
    for(unsigned i=0;i<4;++i) {
        const VkBufferUsageFlags usage=i==3?VK_BUFFER_USAGE_TRANSFER_DST_BIT:
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|(i==1?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:0);
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,nullptr,0,i==2?MaxRegions*16:Bytes,usage,VK_SHARING_MODE_EXCLUSIVE};
        CHECK(vkCreateBuffer(device,&bc,nullptr,&buffers[i].handle));
        VkMemoryRequirements mr;vkGetBufferMemoryRequirements(device,buffers[i].handle,&mr);
        std::uint32_t mt=0;
        const VkMemoryPropertyFlags flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|
            (i==3?VK_MEMORY_PROPERTY_HOST_CACHED_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        while(mt<mem.memoryTypeCount && !((mr.memoryTypeBits&(1u<<mt)) && (mem.memoryTypes[mt].propertyFlags&flags)==flags)) ++mt;
        if(mt==mem.memoryTypeCount) {std::puts("SKIP: requires coherent CPU mapping of device-local buffers");return 77;}
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,nullptr,mr.size,mt};CHECK(vkAllocateMemory(device,&ma,nullptr,&buffers[i].memory));
        CHECK(vkBindBufferMemory(device,buffers[i].handle,buffers[i].memory,0));CHECK(vkMapMemory(device,buffers[i].memory,0,VK_WHOLE_SIZE,0,&buffers[i].mapped));
    }
    auto* source=static_cast<std::uint32_t*>(buffers[0].mapped);
    for(std::uint32_t i=0;i<Words;++i)source[i]=i*2654435761u ^ (i>>7);
    std::array<VkDescriptorSetLayoutBinding,3> binds{};
    for(unsigned i=0;i<3;++i)binds[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo slc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,nullptr,0,3,binds.data()};
    VkDescriptorSetLayout set_layout;CHECK(vkCreateDescriptorSetLayout(device,&slc,nullptr,&set_layout));
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
    VkPipelineLayoutCreateInfo plc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,nullptr,0,1,&set_layout,1,&push};
    VkPipelineLayout layout;CHECK(vkCreatePipelineLayout(device,&plc,nullptr,&layout));
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3};
    VkDescriptorPoolCreateInfo dpc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,nullptr,0,1,1,&pool_size};
    VkDescriptorPool dp;CHECK(vkCreateDescriptorPool(device,&dpc,nullptr,&dp));
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,nullptr,dp,1,&set_layout};
    VkDescriptorSet ds;CHECK(vkAllocateDescriptorSets(device,&dsa,&ds));
    std::array<VkDescriptorBufferInfo,3> infos{};std::array<VkWriteDescriptorSet,3>writes{};
    for(unsigned i=0;i<3;++i) {infos[i]={buffers[i].handle,0,VK_WHOLE_SIZE};writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,ds,i,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&infos[i],nullptr};}
    vkUpdateDescriptorSets(device,3,writes.data(),0,nullptr);
    std::array<VkPipeline,4> pipelines{};
    for(unsigned i=0;i<4;++i) {
        std::ifstream file(argv[i+1],std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
        assert(file && !bytes.empty() && bytes.size()%4==0);std::vector<std::uint32_t> words(bytes.size()/4);std::memcpy(words.data(),bytes.data(),bytes.size());
        VkShaderModuleCreateInfo mc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,nullptr,0,bytes.size(),words.data()};VkShaderModule module;
        CHECK(vkCreateShaderModule(device,&mc,nullptr,&module));
        VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,nullptr,0,{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr},layout};
        CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pc,nullptr,&pipelines[i]));vkDestroyShaderModule(device,module,nullptr);
    }
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,nullptr,VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,family};VkCommandPool command_pool;CHECK(vkCreateCommandPool(device,&cp,nullptr,&command_pool));
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,nullptr,command_pool,VK_COMMAND_BUFFER_LEVEL_PRIMARY,1};VkCommandBuffer cmd;CHECK(vkAllocateCommandBuffers(device,&ca,&cmd));
    VkQueryPoolCreateInfo qpc{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,nullptr,0,VK_QUERY_TYPE_TIMESTAMP,2};VkQueryPool queries;CHECK(vkCreateQueryPool(device,&qpc,nullptr,&queries));
    const std::array<const char*,4> names{"scalar64","scalar32","vector64","vector32"};
    unsigned cases=0;
    for(auto n: {8u,32u,64u,128u,1024u}) for(auto size:{7u,32u,64u,257u,1024u}) for(auto mixed:{0u,1u,2u}) {
        struct Region {std::uint32_t src,dst,size,pad;};auto* table=static_cast<Region*>(buffers[2].mapped);
        std::vector<std::uint32_t> expected(Words,0xa5a5a5a5u);
        for(std::uint32_t i=0;i<n;++i) {
            table[i]={i*Stride+(mixed?i%4:0),
                      i*Stride+(mixed==2?i%4:mixed?(i/4)%4:0),size,0};
            std::memcpy(expected.data()+table[i].dst,source+table[i].src,size*4);
        }
        std::array<std::vector<double>,4> timings;
        // Alternate order across rounds to reduce warmup / clock-order bias.
        for(unsigned round=0;round<5;++round) for(unsigned order=0;order<4;++order) {
            const unsigned variant=(order+round)%4;
            std::memset(buffers[1].mapped,0xa5,Bytes);
            CHECK(vkResetCommandBuffer(cmd,0));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(cmd,&begin));
            vkCmdResetQueryPool(cmd,queries,0,2);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&host,0,nullptr,0,nullptr);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[variant]);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&ds,0,nullptr);vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&n);
            vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,queries,0);
            constexpr unsigned repeats=64;
            for(unsigned repeat=0;repeat<repeats;++repeat) {
                vkCmdDispatch(cmd,n,1,1);
                VkMemoryBarrier waw{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT};
                vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&waw,0,nullptr,0,nullptr);
            }
            vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,queries,1);
            VkMemoryBarrier copy_ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT};
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&copy_ready,0,nullptr,0,nullptr);
            const VkBufferCopy read_copy{0,0,Bytes};vkCmdCopyBuffer(cmd,buffers[1].handle,buffers[3].handle,1,&read_copy);
            VkMemoryBarrier read{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&read,0,nullptr,0,nullptr);
            CHECK(vkEndCommandBuffer(cmd));VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO,nullptr,0,nullptr,nullptr,1,&cmd};CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
            assert(std::memcmp(expected.data(),buffers[3].mapped,Bytes)==0);++cases;
            std::array<std::uint64_t,2> stamps{};CHECK(vkGetQueryPoolResults(device,queries,0,2,sizeof(stamps),stamps.data(),8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
            if(round)timings[variant].push_back((stamps[1]-stamps[0])*prop.limits.timestampPeriod/1000.0/repeats);
        }
        std::printf("COPY n=%u bytes=%u mixed=%u",n,size*4,unsigned(mixed));
        for(unsigned i=0;i<4;++i) {auto& v=timings[i];std::sort(v.begin(),v.end());std::printf(" %s=%.3f_us",names[i],(v[1]+v[2])*0.5);}
        std::puts("");
    }
    std::printf("PASS: %u full-buffer byte comparisons, aligned/misaligned offsets, scalar tails and untouched sentinels; microbench is not an FPS benchmark\n",cases);
    vkDestroyQueryPool(device,queries,nullptr);vkDestroyCommandPool(device,command_pool,nullptr);
    for(auto p:pipelines)vkDestroyPipeline(device,p,nullptr);vkDestroyDescriptorPool(device,dp,nullptr);vkDestroyPipelineLayout(device,layout,nullptr);vkDestroyDescriptorSetLayout(device,set_layout,nullptr);
    for(auto b:buffers) {vkUnmapMemory(device,b.memory);vkDestroyBuffer(device,b.handle,nullptr);vkFreeMemory(device,b.memory,nullptr);}
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
}
