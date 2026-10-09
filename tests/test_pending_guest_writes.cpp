// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "video_core/renderer_vulkan/pending_guest_writes.h"

int main() {
    using Vulkan::PendingGuestWrites;
    std::uint64_t consumed = 64;
    const auto reached = [&](std::uint64_t position) { return consumed >= position; };
    PendingGuestWrites writes;
    // Reproduce the race: the prior packet is consumed while the writer is being built.
    writes.Note(0x1000, 256, PendingGuestWrites::Unpublished);
    assert(writes.Overlaps(0x1080, 16, reached));
    assert(!writes.Overlaps(0x1100, 16, reached));
    writes.Publish(128);
    assert(writes.Overlaps(0x1080, 16, reached));
    consumed = 127;
    assert(writes.Overlaps(0x1080, 16, reached));
    consumed = 128;
    assert(!writes.Overlaps(0x1080, 16, reached));
    // A new aliased writer must keep the range pending through its own packet.
    writes.Note(0x1080, 256, PendingGuestWrites::Unpublished);
    assert(writes.Overlaps(0x1000, 16, reached));
    writes.Publish(256);
    consumed = 200;
    assert(writes.Overlaps(0x1170, 16, reached));
    consumed = 256;
    assert(!writes.Overlaps(0x1170, 16, reached));
    // Writes noted after an ordered task was published retain their exact end position.
    writes.Note(0x2000, 64, 300);
    assert(writes.Overlaps(0x2000, 64, reached));
    assert(!writes.Overlaps(0x1fc0, 64, reached));
    consumed = 300;
    assert(!writes.Overlaps(0x2000, 64, reached));
    // Pruning must preserve unpublished writers, including a packet that wraps the ring.
    writes.Note(0x3000, 64, PendingGuestWrites::Unpublished);
    consumed = 4096;
    for (int i = 0; i < 128; ++i) assert(writes.Overlaps(0x3000, 64, reached));
    writes.Publish(4160);
    assert(writes.Overlaps(0x3000, 64, reached));
    consumed = 4160;
    assert(!writes.Overlaps(0x3000, 64, reached));
    // Stage B publishes GPU-modified state before retiring its packet. Checking that
    // state first would allow a stale CPU snapshot in this interleaving.
    writes.Note(0x4000, 64, 4224);
    bool gpu_modified = false;
    const auto completes_during_check = [&](std::uint64_t position) {
        gpu_modified = true;
        return position == 4224;
    };
    assert(!writes.CanSnapshot(0x4000, 64, completes_during_check,
                               [&] { return gpu_modified; }));
    assert(gpu_modified);
    std::puts("Pending guest writes: writer publication, aliases, ordered tasks and wrap PASS");
}
