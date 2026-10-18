// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual registry and cached-walker handler without faulting a guest process.
#include <cassert>
#include <array>
#include <barrier>
#include <cstring>
#include <cstdio>
#include <thread>
#include <vector>
#include <windows.h>
#include "bbport_toggles.h"
#include "core/signals.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/ir_emitter.h"
namespace Shader::Optimization { void FlattenExtendedUserdataPass(IR::Program&); }
static LONG CALLBACK LiveFault(EXCEPTION_POINTERS* ex) {
    if (ex->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        Core::Signals::Instance()->DispatchAccessViolation(ex, reinterpret_cast<void*>(ex->ExceptionRecord->ExceptionInformation[1])))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
int main() {
    for (bool r10 : {false, true}) {
        const std::array<u8, 8> bytes = r10
            ? std::array<u8,8>{0x44,0x8b,0x97,0x40,0,0,0,0xc3}
            : std::array<u8,8>{0x48,0x8b,0xbf,0x40,0,0,0,0xc3};
        const auto code = reinterpret_cast<const u8*>(Shader::RegisterWalkerCode(bytes.data(), bytes.size()));
        CONTEXT context{}; context.Rip = reinterpret_cast<DWORD64>(code);
        EXCEPTION_POINTERS exception{nullptr, &context};
        sigjmp_buf recovery{}; runtime_fault_recover = &recovery;
        assert(!Core::Signals::Instance()->DispatchAccessViolation(&exception, reinterpret_cast<void*>(0x40)));
        assert(std::memcmp(code, bytes.data(), bytes.size()) == 0);
        runtime_fault_recover = nullptr;
        std::barrier start(16);
        std::array<bool,16> handled{};
        std::vector<std::thread> threads;
        for (unsigned n = 0; n < 16; ++n) threads.emplace_back([&,n] {
            CONTEXT local{}; local.Rip = reinterpret_cast<DWORD64>(code);
            local.Rdi = local.R10 = 0x1122334455667788ull;
            EXCEPTION_POINTERS fault{nullptr, &local};
            start.arrive_and_wait();
            handled[n] = Core::Signals::Instance()->DispatchAccessViolation(&fault, reinterpret_cast<void*>(0x40));
            assert(local.Rip == reinterpret_cast<DWORD64>(code) + 7);
            assert((r10 ? local.R10 : local.Rdi) == 0);
            assert((r10 ? local.Rdi : local.R10) == 0x1122334455667788ull);
        });
        for (auto& thread : threads) thread.join();
        for (bool result : handled) assert(result);
        assert(std::memcmp(code, bytes.data(), bytes.size()) == 0);
        std::array<u8,11> live{};
        std::memcpy(live.data(), bytes.data(), 7);
        const std::array<u8,3> move = r10 ? std::array<u8,3>{0x44,0x89,0xd0}
                                               : std::array<u8,3>{0x48,0x89,0xf8};
        std::memcpy(live.data()+7,move.data(),3); live[10]=0xc3;
        const auto live_code = Shader::RegisterWalkerCode(live.data(),live.size());
        using Load = u64 PS4_SYSV_ABI (*)(const void*);
        const auto load = reinterpret_cast<Load>(live_code);
        auto* handler = AddVectoredExceptionHandler(1,LiveFault); assert(handler);
        std::array<u64,16> table{}; table[8]=0x1122334455667788ull;
        const u64 expected = r10 ? 0x55667788ull : table[8];
        for (int repeat=0;repeat<3;++repeat) {
            assert(load(nullptr)==0);
            assert(load(table.data())==expected);
        }
        assert(std::memcmp(reinterpret_cast<const void*>(live_code),live.data(),live.size())==0);
        RemoveVectoredExceptionHandler(handler);

    }
    CONTEXT outside{}; outside.Rip = 1;
    EXCEPTION_POINTERS exception{nullptr, &outside};
    assert(!Core::Signals::Instance()->DispatchAccessViolation(&exception, nullptr));
    {
        Shader::Info info{}; std::array<u32,16> ud{}; info.user_data=ud;
        Common::ObjectPool<Shader::IR::Inst> pool;
        Shader::IR::Block block(pool); Shader::IR::IREmitter ir(block);
        const auto base=ir.CompositeConstruct(ir.GetUserData(static_cast<Shader::IR::ScalarReg>(0)),
                                              ir.GetUserData(static_cast<Shader::IR::ScalarReg>(1)));
        const auto lo=ir.ReadConst(base,ir.Imm32(0u));
        const auto hi=ir.ReadConst(base,ir.Imm32(1u));
        const Shader::IR::Value leaf=ir.ReadConst(ir.CompositeConstruct(lo,hi),ir.Imm32(2u));
        Shader::IR::Program program(info); program.blocks.push_back(&block);
        program.post_order_blocks.push_back(&block);
        Shader::Optimization::FlattenExtendedUserdataPass(program);
        const u32 slot=leaf.Inst()->Flags<u16>();
        assert(slot>=16 && slot<info.flattened_ud_buf.size());
        std::vector<u8> original(reinterpret_cast<const u8*>(info.srt_info.walker_func),
                                 reinterpret_cast<const u8*>(info.srt_info.walker_func)+info.srt_info.walker_func_size);
        std::array<u32,8> child{}; child[2]=0xdecafbad;
        std::array<u32,8> root{};
        u64 pointer=reinterpret_cast<u64>(child.data()); std::memcpy(root.data(),&pointer,8);
        pointer=reinterpret_cast<u64>(root.data()); std::memcpy(ud.data(),&pointer,8);
        info.RefreshFlatBuf(); assert(info.flattened_ud_buf[slot]==child[2]);
        std::memset(ud.data(),0,8); info.RefreshFlatBuf(); assert(info.flattened_ud_buf[slot]==0);
        std::memcpy(ud.data(),&pointer,8); std::memset(root.data(),0,8);
        info.RefreshFlatBuf(); assert(info.flattened_ud_buf[slot]==0);
        const u64 restored=reinterpret_cast<u64>(child.data()); std::memcpy(root.data(),&restored,8);
        info.RefreshFlatBuf(); assert(info.flattened_ud_buf[slot]==child[2]);
        assert(std::memcmp(reinterpret_cast<const void*>(info.srt_info.walker_func),original.data(),original.size())==0);
    }
    std::puts("Production SRT: live AV recovery/re-read, context-only zeroing, 16 concurrent handlers; generated nested walker null->valid->null child->valid without code mutation PASS");
}
