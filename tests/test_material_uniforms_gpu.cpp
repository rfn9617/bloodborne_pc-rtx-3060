// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <vector>
#ifdef _WIN32
#define NDEBUG
#endif
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/material_uniforms.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/blit_helper.h"
#include "common/serdes.h"
#include "shader_recompiler/specialization.h"
#ifdef _WIN32
#undef NDEBUG
#include <cassert>
#endif

namespace Vulkan {
bool LoadShaderMeta(Serialization::Archive&, Shader::Info&,
    std::optional<Shader::Gcn::FetchShaderData>&, Shader::StageSpecialization&, size_t&);
}

int main(int argc, char** argv) {
    std::setbuf(stdout,nullptr);
    std::setbuf(stderr,nullptr);
    std::puts("Material UBO GPU: loading test shader");
    assert(argc == 3);
#ifdef _WIN32
    _putenv_s("BB_MATERIAL_UBO","1");
#else
    setenv("BB_MATERIAL_UBO","1",1);
#endif
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    assert(input && !bytes.empty() && bytes.size() % 4 == 0);
    std::vector<u32> source(bytes.size()/4), converted;
    std::memcpy(source.data(), bytes.data(), bytes.size());
    constexpr std::array<u32, 6> bindings{0,1,2,4,5,6};
    if (!Vulkan::MaterialUniforms::Convert(source, bindings, converted)) {
        std::puts("FAIL: synthetic shader has unsupported addressing; inspect fixture SPIR-V");
        return 1;
    }
    std::puts("Material UBO GPU: converted fixture, creating Vulkan device");
    Vulkan::Instance instance(0, false);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());
    dispatch.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    // Check the actual cached resource metadata too: transient SPIR-V conversion,
    // pipeline layout and draw binding must select exactly the same descriptors.
    u32 metadata_count = 0;
    const u64 hash = 0xbb10e440;
    const auto* hash_bytes = reinterpret_cast<const char*>(&hash);
    for (const auto& entry : std::filesystem::directory_iterator(argv[2])) {
        if (entry.path().extension() != ".meta") continue;
        std::ifstream file(entry.path(),std::ios::binary);
        std::vector<char> metadata((std::istreambuf_iterator<char>(file)),{});
        if (std::search(metadata.begin(),metadata.end(),hash_bytes,hash_bytes+8)==metadata.end()) continue;
        std::vector<u8> data_bytes(metadata.begin(),metadata.end());
        Serialization::Archive archive{std::move(data_bytes)};
        Shader::Info info;
        Shader::StageSpecialization spec;
        spec.info=&info;
        std::optional<Shader::Gcn::FetchShaderData> fetch;
        size_t permutation;
        assert(Vulkan::LoadShaderMeta(archive,info,fetch,spec,permutation));
        if (info.pgm_hash!=hash) continue;
        std::vector<u32> chosen;
        for(u32 i=0;i<info.buffers.size();++i)
            if(Vulkan::MaterialUniforms::Use(info,i,instance)) chosen.push_back(spec.start.unified+i);
        assert(std::ranges::equal(chosen,bindings));
        ++metadata_count;
    }
    assert(metadata_count >= 20);
    std::printf("PASS: %u cached metadata permutations agree on six UBO bindings\n",metadata_count);
    std::puts("Material UBO GPU: device ready, constructing scheduler and buffers");
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    constexpr u32 capacity = Vulkan::MaterialUniforms::Capacity;
    constexpr u32 stride = capacity + 1024;
    constexpr u32 invocations = 64, values = 6 * 4;
    VideoCore::Buffer data(instance, 0, 6*stride, VideoCore::MemoryType::Stream);
    VideoCore::Buffer output(instance, 0, invocations*values*4, VideoCore::MemoryType::HostCached);
    std::vector<u32> expected_data(6*stride/4);
    for (u32 i=0; i<expected_data.size(); ++i) {
        // Preserve arbitrary bit patterns, including zeros, NaNs and infinities.
        constexpr std::array<u32,8> special{0,0x80000000,0x7f800000,0xff800000,
            0x7fc01234,0x00000001,0x3f800000,0xffffffff};
        expected_data[i] = i%16 < 8 ? special[i%8] : i*1664525u+1013904223u;
    }
    std::memcpy(data.mapped_data.data(), expected_data.data(), expected_data.size()*4);
    data.Flush(0, data.SizeBytes());
    const std::array pool_sizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer,8},
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer,6}};
    auto pool = device.createDescriptorPoolUnique({.maxSets=2,
        .poolSizeCount=u32(pool_sizes.size()), .pPoolSizes=pool_sizes.data()},nullptr,dispatch).value;
    std::array<vk::UniqueDescriptorSetLayout,2> layouts;
    std::array<vk::UniquePipelineLayout,2> pipeline_layouts;
    std::array<vk::UniquePipeline,2> pipelines;
    std::array<vk::ShaderModule,2> modules{
        Vulkan::CompileSPV(source,device), Vulkan::CompileSPV(converted,device)};
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute,0,4};
    std::array<vk::DescriptorSet,2> sets;
    for (u32 variant=0; variant<2; ++variant) {
        std::printf("Material UBO GPU: creating pipeline %u\n",variant);
        std::array<vk::DescriptorSetLayoutBinding,7> layout_bindings;
        for(u32 i=0;i<6;++i) layout_bindings[i]={bindings[i],variant ? vk::DescriptorType::eUniformBuffer
            : vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute};
        layout_bindings[6]={7,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eCompute};
        layouts[variant]=device.createDescriptorSetLayoutUnique({.bindingCount=7,
            .pBindings=layout_bindings.data()},nullptr,dispatch).value;
        const auto layout=layouts[variant].get();
        pipeline_layouts[variant]=device.createPipelineLayoutUnique({.setLayoutCount=1,
            .pSetLayouts=&layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push},nullptr,dispatch).value;
        const vk::ComputePipelineCreateInfo ci{
            .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=modules[variant],.pName="main"},
            .layout=pipeline_layouts[variant].get()};
        auto result=device.createComputePipelineUnique({},ci,nullptr,dispatch);
        assert(result.result==vk::Result::eSuccess);
        pipelines[variant]=std::move(result.value);
        sets[variant]=device.allocateDescriptorSets({.descriptorPool=pool.get(),
            .descriptorSetCount=1,.pSetLayouts=&layout},dispatch).value[0];
    }
    constexpr std::array<u32,4> indices{0,7,191,4032};
    u32 cases=0;
    for(u32 descriptor_offset : {0u,256u,512u}) {
        for(u32 byte_adjust : {0u,4u,12u,64u,128u,252u}) {
            std::array<std::vector<u32>,2> results;
            for(u32 variant=0;variant<2;++variant) {
                std::array<vk::DescriptorBufferInfo,7> buffer_infos;
                std::array<vk::WriteDescriptorSet,7> writes;
                for(u32 i=0;i<6;++i) {
                    buffer_infos[i]={data.Handle(),i*stride+descriptor_offset,capacity};
                    writes[i]={.dstSet=sets[variant],.dstBinding=bindings[i],.descriptorCount=1,
                        .descriptorType=variant ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eStorageBuffer,
                        .pBufferInfo=&buffer_infos[i]};
                }
                buffer_infos[6]={output.Handle(),0,output.SizeBytes()};
                writes[6]={.dstSet=sets[variant],.dstBinding=7,.descriptorCount=1,
                    .descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffer_infos[6]};
                device.updateDescriptorSets(writes,{},dispatch);
                const auto command=scheduler.CommandBuffer();
                runtime.AccessBuffer(&output,0,output.SizeBytes(),vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderWrite);
                runtime.FlushBarriers();
                command.bindPipeline(vk::PipelineBindPoint::eCompute,pipelines[variant].get(),dispatch);
                command.bindDescriptorSets(vk::PipelineBindPoint::eCompute,pipeline_layouts[variant].get(),
                    0,sets[variant],{},dispatch);
                command.pushConstants(pipeline_layouts[variant].get(),vk::ShaderStageFlagBits::eCompute,
                    0,4,&byte_adjust,dispatch);
                command.dispatch(1,1,1,dispatch);
                runtime.AccessBuffer(&output,0,output.SizeBytes(),vk::PipelineStageFlagBits2::eHost,
                    vk::AccessFlagBits2::eHostRead);
                runtime.FlushBarriers();
                scheduler.Finish();
                std::printf("Material UBO GPU: offset %u/%u variant %u finished\n",descriptor_offset,byte_adjust,variant);
                output.Invalidate(0,output.SizeBytes());
                results[variant].resize(invocations*values);
                std::memcpy(results[variant].data(),output.mapped_data.data(),output.SizeBytes());
            }
            assert(results[0]==results[1]);
            for(u32 invocation=0;invocation<invocations;++invocation)
                for(u32 b=0;b<6;++b) for(u32 j=0;j<4;++j)
                    assert(results[1][invocation*values+b*4+j] == expected_data[
                        (b*stride+descriptor_offset)/4+indices[j]+byte_adjust/4]);
            ++cases;
        }
    }
    scheduler.Finish();
    for(auto module:modules) device.destroyShaderModule(module,nullptr,dispatch);
    std::printf("PASS: %u actual Vulkan cases, six buffers, aligned descriptors and packed offsets; raw zero/NaN/infinity and finite bits identical\n",cases);
}
