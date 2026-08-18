// SPDX-License-Identifier: MIT
#include "machinery/fuse.h"

#include <format>
#include <unordered_map>

namespace arcjit {

std::string Fuse::dump() const {
    return std::format(
        "Fuse(clones={}, max_clones_per_fn={}, max_total={})",
        total_clones_, budget_.max_clones_per_function, budget_.max_total_clones);
}

Fuse& global_fuse() {
    static thread_local Fuse f;
    return f;
}

}  // namespace arcjit
