// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace Vulkan {

// Stage A tracks writes before publishing their packet. They must not retire at the
// previous packet's end: stage B can have consumed it without executing this write.
class PendingGuestWrites {
public:
    using Address = std::uint64_t;
    static constexpr Address Unpublished = std::numeric_limits<Address>::max();

    void Note(Address address, Address size, Address position) {
        if (!size) return;
        const Address end = size > Unpublished - address ? Unpublished : address + size;
        for (auto& write : writes) {
            if (address <= write.end && write.begin <= end) {
                write.begin = std::min(write.begin, address);
                write.end = std::max(write.end, end);
                write.position = std::max(write.position, position);
                minimum = std::min(minimum, address);
                maximum = std::max(maximum, end);
                return;
            }
        }
        writes.push_back({address, end, position});
        minimum = std::min(minimum, address);
        maximum = std::max(maximum, end);
    }

    void Publish(Address position) {
        for (auto& write : writes) {
            if (write.position == Unpublished) write.position = position;
        }
    }

    template <typename Reached>
    bool Overlaps(Address address, Address size, const Reached& reached) {
        if (!size || writes.empty() || address >= maximum) return false;
        const Address end = size > Unpublished - address ? Unpublished : address + size;
        if (end <= minimum) return false;
        const auto done = [&](const Write& write) {
            return write.position != Unpublished && reached(write.position);
        };
        if ((++checks & 63) == 0) {
            std::erase_if(writes, done);
            minimum = Unpublished;
            maximum = 0;
            for (const auto& write : writes) {
                minimum = std::min(minimum, write.begin);
                maximum = std::max(maximum, write.end);
            }
        }
        for (const auto& write : writes) {
            if (address < write.end && write.begin < end && !done(write)) return true;
        }
        return false;
    }

    template <typename Reached, typename Modified>
    bool CanSnapshot(Address address, Address size, const Reached& reached,
                     const Modified& modified) {
        // Acquire packet completion before checking cache state: stage B may have marked
        // the buffer GPU-written between an earlier cache check and completion lookup.
        return !Overlaps(address, size, reached) && !modified();
    }

private:
    struct Write { Address begin, end, position; };
    std::vector<Write> writes;
    Address minimum = Unpublished, maximum = 0;
    std::uint32_t checks = 0;
};

} // namespace Vulkan
