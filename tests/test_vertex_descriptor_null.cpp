// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vertex_input_format.h"

int main() {
    Shader::Info info{};
    Shader::Gcn::VertexAttribute attribute{};
    attribute.sgpr_base = 2;
    attribute.dword_offset = 0;
    const AmdGpu::Buffer zero{};
    const auto null_format = Vulkan::VertexInputDescriptorFormat(zero);
    assert(null_format.first == AmdGpu::DataFormat::Format32_32_32_32 &&
           null_format.second == AmdGpu::NumberFormat::Float);
    const auto empty = [&] {
        const auto sharp = attribute.GetSharp(info);
        assert(!sharp && std::memcmp(&sharp, &zero, sizeof(sharp)) == 0);
        bool has_data = true;
        attribute.GetSharp(info, &has_data);
        assert(!has_data);
        // Exercise the actual specialization loop from the crash, not a copy of its logic.
        Shader::StageSpecialization spec;
        spec.info = &info;
        std::vector<Shader::Gcn::VertexAttribute> attributes{attribute};
        Shader::Gcn::FetchShaderData fetch;
        fetch.attributes = attributes;
        unsigned called = 0;
        spec.ForEachSharp(spec.vs_attribs, attributes,
                          [&](auto&, const auto&, AmdGpu::Buffer) { ++called; });
        assert(spec.vs_attribs.size() == 1 && called == 0);
    };
    empty(); // no user-data span at all
    std::array<u32,16> ud{};
    info.user_data = ud;
    empty(); // exact null-table case at bb-probe.exe+c8ae2
    for (u64 low : {u64{8},u64{0x40},u64{0xfff0}}) {
        std::memcpy(ud.data()+2, &low, 8);
        empty();
    }
    std::memset(ud.data()+2, 0, 8);
    attribute.dword_offset = 12;
    empty(); // null table plus a nonzero descriptor offset
    const u64 tagged_null = 0xabcd000000000000ULL;
    std::memcpy(ud.data()+2, &tagged_null, 8);
    empty();
    attribute.sgpr_base = 15;
    empty(); // final SGPR cannot hold a two-dword pointer
    attribute.sgpr_base = 16;
    empty(); // past the user-data span
    attribute.sgpr_base = Shader::IR::NumScalarRegs;
    attribute.dword_offset = 16;
    empty();
    attribute.dword_offset = 13;
    empty(); // only three dwords remain for a four-dword descriptor

    AmdGpu::Buffer expected{};
    expected.base_address = 0x123456780;
    expected.stride = 32;
    expected.num_records = 117;
    expected.data_format = 10;
    expected.num_format = 7;
    expected.dst_sel_x = 4; expected.dst_sel_y = 5;
    std::memcpy(ud.data()+12, &expected, sizeof(expected));
    attribute.dword_offset = 12;
    auto sharp = attribute.GetSharp(info);
    assert(std::memcmp(&sharp, &expected, sizeof(sharp)) == 0);

    std::array<u32,12> table{};
    std::memcpy(table.data()+4, &expected, sizeof(expected));
    const u64 pointer = reinterpret_cast<uintptr_t>(table.data()) | 0xabcd000000000000ULL;
    std::memcpy(ud.data()+2, &pointer, 8);
    attribute.sgpr_base = 2;
    attribute.dword_offset = 4;
    sharp = attribute.GetSharp(info);
    assert(std::memcmp(&sharp, &expected, sizeof(sharp)) == 0);
    const auto valid_format = Vulkan::VertexInputDescriptorFormat(expected);
    assert(valid_format.first == expected.GetDataFmt() && valid_format.second == expected.GetNumberFmt());
    auto absent_attribute = attribute;
    absent_attribute.sgpr_base = 4;
    Shader::StageSpecialization mixed;
    mixed.info = &info;
    std::vector<Shader::Gcn::VertexAttribute> mixed_inputs{absent_attribute,attribute};
    unsigned valid_inputs = 0;
    mixed.ForEachSharp(mixed.vs_attribs,mixed_inputs,
                       [&](auto&,const auto&,AmdGpu::Buffer) { ++valid_inputs; });
    assert(mixed.vs_attribs.size()==2 && valid_inputs==1);
    Shader::Gcn::FetchShaderData fetch;
    fetch.attributes = {attribute};
    attribute.inst_offset = 8;
    attribute.data_format = 11; attribute.num_format = 3;
    sharp = attribute.GetSharp(info);
    auto adjusted = expected;
    adjusted.base_address += 8; adjusted.data_format = 11; adjusted.num_format = 3;
    assert(std::memcmp(&sharp, &adjusted, sizeof(sharp)) == 0);

    attribute.inst_offset = 0; attribute.data_format = 0;
    std::array<u32,16> snapshot{};
    info.ud_snapshots[0] = {&info, snapshot.data(), 16, nullptr, 0, 0};
    info.num_ud_snapshots = 1;
    empty(); // snapshot has no table, live UD still has a valid one
    std::memcpy(snapshot.data()+2, &pointer, 8);
    sharp = attribute.GetSharp(info);
    assert(std::memcmp(&sharp, &expected, sizeof(sharp)) == 0);
    info.num_ud_snapshots = 0;
    auto empty_buffer = expected;
    empty_buffer.num_records = 0;
    std::memcpy(table.data()+4, &empty_buffer, sizeof(empty_buffer));
    bool has_data = false;
    sharp = attribute.GetSharp(info, &has_data);
    assert(has_data && !sharp);
    // Zero records is a valid empty buffer, distinct from an absent descriptor table.
    std::memcpy(table.data()+4, &expected, sizeof(expected));
    fetch.attributes.clear();
    std::puts("Vertex descriptors: null/tagged-null tables, empty/truncated UD, inline bounds, actual specialization, valid pointers/offsets/formats and per-draw snapshots PASS");
}
