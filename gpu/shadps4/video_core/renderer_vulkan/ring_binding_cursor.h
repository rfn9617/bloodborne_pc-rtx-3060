// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace Vulkan {

// CollectRingBindings appends entries in resource-index order. BindBuffers queries
// all resource indices in increasing order. This view is local to that call: no
// descriptor, offset, data, or pointer is retained for another stage/draw/packet.
template <typename Binding>
class RingBindingCursor {
public:
    explicit RingBindingCursor(std::span<const Binding> bindings) : bindings{bindings} {}

    const Binding* Find(std::uint32_t index) {
        while (position < bindings.size() && bindings[position].index < index) ++position;
        return position < bindings.size() && bindings[position].index == index
                   ? &bindings[position] : nullptr;
    }

private:
    std::span<const Binding> bindings;
    std::size_t position = 0;
};

} // namespace Vulkan
