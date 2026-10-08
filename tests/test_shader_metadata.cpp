// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "common/serdes.h"
#include "shader_recompiler/info.h"

int main() {
    for (bool discard : {false, true}) {
        Shader::Info original;
        original.has_discard = discard;
        original.pgm_hash = 0x123456789abcdef0ull;
        original.mrt_mask = 0x3f;
        original.sw_stage = Shader::SwStage::Fragment;
        original.flattened_ud_buf = {12, 34, 56};
        Serialization::Archive archive;
        original.Serialize(archive);
        Serialization::Archive loaded{archive.TakeOff()};
        Shader::Info restored;
        restored.has_discard = !discard;
        assert(restored.Deserialize(loaded));
        assert(loaded.IsEoS());
        assert(restored.has_discard == discard);
        assert(restored.pgm_hash == original.pgm_hash);
        assert(restored.mrt_mask == original.mrt_mask);
        assert(restored.sw_stage == Shader::SwStage::Fragment);
        assert(restored.flattened_ud_buf == original.flattened_ud_buf);
    }
    std::puts("Shader metadata: alpha discard survives real archive serialization PASS");
}
