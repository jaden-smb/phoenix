// phx/memory/memory_root.h — boots the root arena and hands out sub-allocators.
// ONE OS allocation at boot (host) or a static buffer (GBA). After boot, the engine
// performs zero general-purpose heap allocations. See docs/05-memory.md.
#ifndef PHX_MEMORY_MEMORY_ROOT_H
#define PHX_MEMORY_MEMORY_ROOT_H

#include "phx/core/types.h"
#include "phx/memory/allocators.h"

namespace phx {

class MemoryRoot {
public:
    // total           = bytes for the whole engine (from Caps::total_main_ram)
    // frame_scratch   = per-frame transient size; allocated x2 (double-buffered)
    static Result<MemoryRoot*> boot(size_t total, size_t frame_scratch);
    static void shutdown(MemoryRoot*);

    // App-lifetime allocator: the root arena itself (everything persistent comes here).
    ArenaAllocator& persistent() { return root_; }

    // The double-buffered per-frame transient stack (reset every frame, O(1)).
    StackAllocator& frame_stack() { return frame_[cur_]; }
    void            swap_frame()  { cur_ ^= 1; frame_[cur_].reset(); }

    // Carve a named child arena from the root at boot time. tag is for the memory map.
    ArenaAllocator  sub(size_t bytes, const char* tag = nullptr);

    // Diagnostics
    size_t total_capacity() const { return root_.capacity(); }
    size_t used()           const { return root_.used(); }
    void   dump_map() const;       // prints the boot-time memory map (host builds)

private:
    MemoryRoot() = default;

    void*          block_ = nullptr;   // the single owning allocation (host)
    ArenaAllocator root_;
    StackAllocator frame_[2];
    uint8_t        cur_ = 0;

    struct Tag { const char* name; size_t bytes; };
    Tag      tags_[16];
    uint8_t  tag_count_ = 0;
};

} // namespace phx
#endif // PHX_MEMORY_MEMORY_ROOT_H
