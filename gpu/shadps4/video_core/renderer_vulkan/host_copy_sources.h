// SPDX-License-Identifier: GPL-2.0-or-later
// Conservative range tracking adapted from deadinside28/bloodborne_pc 3c57da4.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>

namespace Vulkan {
class HostCopySources {
public:
    using U64 = std::uint64_t;
    static constexpr U64 GranuleShift = 16, MaxGranules = 64;
    static constexpr std::size_t Slots = 4096;
    static constexpr U64 Wildcard = std::numeric_limits<U64>::max();

    static std::size_t SlotOf(U64 granule) {
        return ((granule * 0x9e3779b97f4a7c15ull) >> 40) % Slots;
    }
    void Note(U64 address, U64 size) {
        if (!size) return;
        std::scoped_lock lock{mutex};
        const U64 seq = ++issued;
        U64 first, last;
        if (!Granules(address, size, first, last)) {
            big_seq = seq;
            return;
        }
        for (U64 g = first; g <= last; ++g) {
            auto& slot = slots[SlotOf(g)];
            if (slot.seq > done && slot.granule != g) {
                slot.granule = Wildcard;
            } else if (slot.granule != Wildcard || slot.seq <= done) {
                slot.granule = g;
            }
            slot.seq = seq;
        }
    }
    bool NeedsWait(U64 address, U64 size) const {
        std::scoped_lock lock{mutex};
        if (big_seq > done) return true;
        U64 first, last;
        // Unknown/overflowing or very large writes always take the safe full wait.
        if (!Granules(address, std::max<U64>(size, 1), first, last)) return true;
        for (U64 g = first; g <= last; ++g) {
            const auto& slot = slots[SlotOf(g)];
            if (slot.seq > done && (slot.granule == g || slot.granule == Wildcard))
                return true;
        }
        return false;
    }
    U64 Snapshot() const {
        std::scoped_lock lock{mutex};
        return issued;
    }
    // Only retire sources captured BEFORE the actual global copy wait. A source
    // published concurrently after that snapshot must remain pending.
    void Complete(U64 seen) {
        std::scoped_lock lock{mutex};
        done = std::max(done, seen);
    }
private:
    static bool Granules(U64 address, U64 size, U64& first, U64& last) {
        if (size - 1 > std::numeric_limits<U64>::max() - address) return false;
        first = address >> GranuleShift;
        last = (address + size - 1) >> GranuleShift;
        return last - first < MaxGranules;
    }
    struct Slot { U64 granule = 0, seq = 0; };
    std::array<Slot, Slots> slots{};
    mutable std::mutex mutex;
    U64 issued = 0, done = 0, big_seq = 0;
};
} // namespace Vulkan
