// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <spirv/unified1/spirv.hpp11>
#include "video_core/renderer_vulkan/material_clip_fusion.h"

int main(int argc,char** argv) {
    assert(argc==3);
    std::ifstream in(argv[1],std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),{});
    assert(in && bytes.size()%4==0 && bytes.size()>=20);
    std::vector<std::uint32_t> source(bytes.size()/4),out;
    std::memcpy(source.data(),bytes.data(),bytes.size());
    const auto copy=source;
    if(!Vulkan::MaterialClipFusion::Convert(source,out)) {
        assert(source==copy && out.empty());
        std::puts("PASS: unsupported/non-clipped permutation unchanged");
        return 2;
    }
    auto count=[](const auto& words,spv::Op wanted) {
        unsigned count=0;
        for(size_t at=5;at<words.size();at+=words[at]>>16)
            count+=(words[at]&0xffff)==unsigned(wanted);
        return count;
    };
    assert(source==copy && out[3]==source[3]+3);
    assert(count(source,spv::Op::OpDemoteToHelperInvocation)==4);
    assert(count(out,spv::Op::OpDemoteToHelperInvocation)==1);
    assert(count(out,spv::Op::OpFOrdLessThan)==count(source,spv::Op::OpFOrdLessThan));
    assert(count(out,spv::Op::OpLoad)==count(source,spv::Op::OpLoad));
    assert(count(out,spv::Op::OpLogicalOr)==count(source,spv::Op::OpLogicalOr)+3);
    std::vector<std::uint32_t> second;
    assert(!Vulkan::MaterialClipFusion::Convert(out,second) && second.empty());
    auto malformed=source; malformed[5]&=0xffff;
    assert(!Vulkan::MaterialClipFusion::Convert(malformed,second) && second.empty());
    // A changed predicate must fall back instead of silently changing clipping.
    auto altered=source;
    for(size_t at=5;at<altered.size();at+=altered[at]>>16) {
        if((altered[at]&0xffff)==unsigned(spv::Op::OpFOrdLessThan)) {
            altered[at]=(altered[at]&0xffff0000)|unsigned(spv::Op::OpFOrdGreaterThan);
            break;
        }
    }
    assert(!Vulkan::MaterialClipFusion::Convert(altered,second) && second.empty());
    std::ofstream output(argv[2],std::ios::binary);
    output.write(reinterpret_cast<const char*>(out.data()),out.size()*4);
    assert(output);
    std::puts("PASS: 4 predicates/loads retained, 4 demote branches -> 1; malformed/changed pattern rejected; canonical input intact");
}
