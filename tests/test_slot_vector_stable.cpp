// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <array>
#include <cstdio>
#include <memory>
#include <vector>
#include "common/slot_vector.h"
struct Entry {
    static inline unsigned constructed = 0, destroyed = 0, moved = 0;
    std::array<unsigned char, 997> padding{}; // odd size exercises chunk boundaries
    std::unique_ptr<unsigned> value;
    explicit Entry(unsigned n) : value(std::make_unique<unsigned>(n)) { ++constructed; }
    Entry(Entry&& other) noexcept : value(std::move(other.value)) { ++constructed; ++moved; }
    ~Entry() { ++destroyed; }
};
int main() {
    unsigned expected = 0;
    {
        Common::SlotVector<Entry> entries;
        std::vector<Common::SlotId> ids;
        std::vector<Entry*> pointers;
        for (unsigned n = 0; n < 50000; ++n) {
            const auto id = entries.insert(n);
            ids.push_back(id); pointers.push_back(&entries[id]);
            assert(&entries[ids.front()] == pointers.front());
        }
        for (unsigned n = 0; n < ids.size(); ++n) {
            assert(&entries[ids[n]] == pointers[n] && *entries[ids[n]].value == n);
            if (n % 3 == 0) entries.erase(ids[n]); else ++expected;
        }
        assert(entries.size() == expected);
        unsigned visited = 0;
        for (const auto& e : entries) { assert(*e.value % 3 != 0); ++visited; }
        assert(visited == expected);
        for (unsigned n = 0; n < ids.size(); n += 3) {
            const auto id = entries.insert(50000 + n);
            assert(id.index < 50000); // reuse erased slots before allocating new chunks
        }
        for (unsigned n = 0; n < ids.size(); ++n)
            if (n % 3) assert(&entries[ids[n]] == pointers[n] && *entries[ids[n]].value == n);
        assert(entries.size() == 50000 && Entry::moved == 0);
    }
    assert(Entry::constructed == Entry::destroyed);
    std::puts("Stable SlotVector: 50000 inserts, odd chunk boundaries, addresses, reuse, iteration and destruction PASS");
}
