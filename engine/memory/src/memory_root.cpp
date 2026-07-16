// phx/memory/src/memory_root.cpp — the root memory carving. This one file is linked on
// EVERY target: the single boot malloc lands in the newlib EWRAM heap on GBA. A GBA
// variant sourcing `block_` from a static EWRAM buffer instead is planned, not built.
#include "phx/memory/memory_root.h"

#include "phx/core/log.h"

#include <cstdlib>
#include <new>

namespace phx {

Result<MemoryRoot*> MemoryRoot::boot(size_t total, size_t frame_scratch) {
    // The ONE general-purpose allocation in the whole engine lifetime (host).
    void* mem = std::malloc(total);
    if (!mem) return Result<MemoryRoot*>::fail(Status::OutOfMemory);

    // Place the control struct at the front of the block, then arena the remainder.
    MemoryRoot* mr = new (mem) MemoryRoot();
    mr->block_ = mem;

    const size_t hdr = align_up(sizeof(MemoryRoot), 16);
    if (hdr + frame_scratch * 2 >= total) {
        std::free(mem);
        return Result<MemoryRoot*>::fail(Status::OutOfMemory);
    }
    mr->root_.init(static_cast<uint8_t*>(mem) + hdr, total - hdr);

    // Carve the two frame stacks up front (hot, fixed size).
    void* fa = mr->root_.alloc(frame_scratch);
    void* fb = mr->root_.alloc(frame_scratch);
    mr->frame_[0].init(fa, frame_scratch);
    mr->frame_[1].init(fb, frame_scratch);
    mr->tags_[mr->tag_count_++] = { "frame_stack[2]", frame_scratch * 2 };

    return Result<MemoryRoot*>::good(mr);
}

void MemoryRoot::shutdown(MemoryRoot* mr) {
    if (!mr) return;
    void* block = mr->block_;
    mr->~MemoryRoot();
    std::free(block);
}

ArenaAllocator MemoryRoot::sub(size_t bytes, const char* tag) {
    ArenaAllocator a;
    void* p = root_.alloc(bytes);
    PHX_ASSERT_MSG(p != nullptr, "MemoryRoot::sub out of root budget");
    a.init(p, bytes);
    if (tag && tag_count_ < 16) tags_[tag_count_++] = { tag, bytes };
    return a;
}

void MemoryRoot::dump_map() const {
    // Through the log seam, not printf: on PSP a bare printf() is resolved from libpspkernel's
    // KERNEL StdioForKernel stub (no -lc printf wins the link order), and a user-mode EBOOT
    // importing a kernel library fails to load on real hardware (8002013C LIBRARY_NOTFOUND).
    PHX_LOG_INFO("[phx.mem] root %.1f KB | used %.1f KB",
                 root_.capacity() / 1024.0, root_.used() / 1024.0);
    for (uint8_t i = 0; i < tag_count_; ++i)
        PHX_LOG_INFO("          %-16s %.1f KB", tags_[i].name, tags_[i].bytes / 1024.0);
}

} // namespace phx
