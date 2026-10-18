// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <unordered_map>
#include <spirv/unified1/spirv.hpp11>
#include "video_core/renderer_vulkan/material_clip_fusion.h"

namespace Vulkan::MaterialClipFusion {
bool Convert(std::span<const std::uint32_t> source, std::vector<std::uint32_t>& output) {
    output.clear();
    using Word=std::uint32_t;
    using Inst=std::span<const Word>;
    if(source.size()<5 || source[0]!=spv::MagicNumber) return false;
    std::vector<Inst> code;
    std::vector<size_t> offsets;
    std::unordered_map<Word,Inst> types, variables, constants;
    std::unordered_map<Word,unsigned> label_uses;
    unsigned entries=0;
    auto op=[](Inst i){ return static_cast<spv::Op>(i[0]&0xffff); };
    for(size_t at=5;at<source.size();) {
        Word size=source[at]>>16;
        if(!size || size>source.size()-at) return false;
        const Inst i=source.subspan(at,size);
        code.push_back(i); offsets.push_back(at);
        if(op(i)==spv::Op::OpEntryPoint) {
            if(size<4 || i[1]!=Word(spv::ExecutionModel::Fragment)) return false;
            ++entries;
        }
        if(op(i)>=spv::Op::OpTypeVoid && op(i)<=spv::Op::OpTypeForwardPointer && size>=2)
            types.emplace(i[1],i);
        if(op(i)==spv::Op::OpVariable && size==4) variables.emplace(i[2],i);
        if(op(i)==spv::Op::OpConstant && size==4) constants.emplace(i[2],i);
        if(op(i)==spv::Op::OpBranch && size==2) ++label_uses[i[1]];
        if(op(i)==spv::Op::OpBranchConditional && size>=4) {
            ++label_uses[i[2]]; ++label_uses[i[3]];
        }
        if(op(i)==spv::Op::OpSelectionMerge && size==3) ++label_uses[i[1]];
        // Count every control-flow reference. Only the last merge label survives
        // fusion; an intermediate label referenced outside its own clip block
        // must never be removed (including parents of later phi nodes).
        if(op(i)==spv::Op::OpLoopMerge && size>=4) {
            ++label_uses[i[1]]; ++label_uses[i[2]];
        }
        if(op(i)==spv::Op::OpPhi && size>=5)
            for(size_t n=4;n<size;n+=2) ++label_uses[i[n]];
        // Switch selector literal widths may be 64-bit. Unsupported switch
        // modules fall back rather than guessing which words are label IDs.
        if(op(i)==spv::Op::OpSwitch) return false;
        at+=size;
    }
    if(entries!=1) return false;
    auto float32=[&](Word id) {
        auto t=types.find(id);
        return t!=types.end() && op(t->second)==spv::Op::OpTypeFloat &&
            t->second.size()==3 && t->second[2]==32;
    };
    for(size_t start=0;start+36<=code.size();++start) {
        std::array<Word,4> conditions{};
        Word base=0,boolean=0,scalar=0;
        bool match=true;
        for(unsigned plane=0;plane<4 && match;++plane) {
            const size_t at=start+plane*9;
            const auto a=code[at], load=code[at+1], compare=code[at+2];
            const auto merge=code[at+3], branch=code[at+4], yes=code[at+5];
            const auto demote=code[at+6], end=code[at+7], no=code[at+8];
            if(op(a)!=spv::Op::OpAccessChain || a.size()!=5 ||
                op(load)!=spv::Op::OpLoad || load.size()!=4 || load[3]!=a[2] ||
                op(compare)!=spv::Op::OpFOrdLessThan || compare.size()!=5 || compare[3]!=load[2] ||
                op(merge)!=spv::Op::OpSelectionMerge || merge.size()!=3 || merge[2]!=0 ||
                op(branch)!=spv::Op::OpBranchConditional || branch.size()!=4 || branch[1]!=compare[2] ||
                op(yes)!=spv::Op::OpLabel || yes.size()!=2 || yes[1]!=branch[2] ||
                op(demote)!=spv::Op::OpDemoteToHelperInvocation || demote.size()!=1 ||
                op(end)!=spv::Op::OpBranch || end.size()!=2 || end[1]!=merge[1] ||
                op(no)!=spv::Op::OpLabel || no.size()!=2 || no[1]!=merge[1] || no[1]!=branch[3] ||
                label_uses[yes[1]]!=1 || (plane<3 ? label_uses[no[1]]!=3 : label_uses[no[1]]<3)) { match=false; break; }
            const auto index=constants.find(a[4]), zero=constants.find(compare[4]);
            const auto ptr=types.find(a[1]), variable=variables.find(a[3]);
            if(index==constants.end() || index->second[3]!=plane ||
                zero==constants.end() || zero->second[3]!=0 || !float32(zero->second[1]) ||
                !float32(load[1]) || ptr==types.end() || op(ptr->second)!=spv::Op::OpTypePointer ||
                ptr->second.size()!=4 || ptr->second[2]!=Word(spv::StorageClass::Input) ||
                ptr->second[3]!=load[1] || variable==variables.end() ||
                variable->second[3]!=Word(spv::StorageClass::Input)) { match=false; break; }
            const auto variable_ptr=types.find(variable->second[1]);
            if(variable_ptr==types.end() || op(variable_ptr->second)!=spv::Op::OpTypePointer ||
                variable_ptr->second.size()!=4) { match=false; break; }
            const auto vector=types.find(variable_ptr->second[3]);
            if(vector==types.end() || op(vector->second)!=spv::Op::OpTypeVector ||
                vector->second.size()!=4 || vector->second[3]!=4 || vector->second[2]!=load[1]) {
                match=false; break;
            }
            if(plane==0) { base=a[3]; boolean=compare[1]; scalar=load[1]; }
            if(a[3]!=base || compare[1]!=boolean || load[1]!=scalar) { match=false; break; }
            conditions[plane]=compare[2];
        }
        if(!match) continue;
        output.assign(source.begin(),source.begin()+offsets[start]);
        for(unsigned plane=0;plane<4;++plane) for(unsigned j=0;j<3;++j) {
            const auto i=code[start+plane*9+j];
            output.insert(output.end(),i.begin(),i.end());
        }
        Word next=source[3], condition=conditions[0];
        for(unsigned plane=1;plane<4;++plane) {
            output.insert(output.end(),{(5u<<16)|Word(spv::Op::OpLogicalOr),boolean,next,condition,conditions[plane]});
            condition=next++;
        }
        const auto merge=code[start+30], branch=code[start+31];
        output.insert(output.end(),merge.begin(),merge.end());
        output.insert(output.end(),{branch[0],condition,branch[2],branch[3]});
        // Preserve the final demote block and merge label.
        for(unsigned j=32;j<36;++j) {
            const auto i=code[start+j];
            output.insert(output.end(),i.begin(),i.end());
        }
        const size_t after=start+36<code.size()?offsets[start+36]:source.size();
        output.insert(output.end(),source.begin()+after,source.end());
        output[3]=next;
        return true;
    }
    return false;
}
}
