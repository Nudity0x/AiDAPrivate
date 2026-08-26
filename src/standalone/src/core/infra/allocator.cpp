#include "allocator.hpp"

#include "../../helpers/diag_log.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <malloc.h>
#include <new>

#include <mimalloc.h>

#if defined(_MSC_VER) && defined(_Ret_notnull_) && defined(_Post_writable_byte_size_)
#define AIDA_MI_DECL_NEW(n) mi_decl_nodiscard mi_decl_restrict _Ret_notnull_ _Post_writable_byte_size_(n)
#define AIDA_MI_DECL_NEW_NOTHROW(n) mi_decl_nodiscard mi_decl_restrict _Ret_maybenull_ _Success_(return != NULL) _Post_writable_byte_size_(n)
#else
#define AIDA_MI_DECL_NEW(n) mi_decl_nodiscard mi_decl_restrict
#define AIDA_MI_DECL_NEW_NOTHROW(n) mi_decl_nodiscard mi_decl_restrict
#endif

namespace {

std::atomic<bool> g_foreign_route_logged{false};

void log_foreign_route_once() noexcept {
    bool expected = false;
    if (g_foreign_route_logged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        diag::log_tagged_fmt("allocator", "allocator_foreign_free_routed active=1");
    }
}

void free_possibly_foreign(void* block) noexcept {
    if (block != nullptr && !mi_is_in_heap_region(block)) {
        log_foreign_route_once();
        std::free(block);
        return;
    }
    mi_free(block);
}

void free_possibly_foreign_sized(void* block, std::size_t size) noexcept {
    if (block != nullptr && !mi_is_in_heap_region(block)) {
        log_foreign_route_once();
        std::free(block);
        return;
    }
    mi_free_size(block, size);
}

void free_possibly_foreign_aligned(void* block, std::size_t alignment) noexcept {
    if (block != nullptr && !mi_is_in_heap_region(block)) {
        log_foreign_route_once();
        _aligned_free(block);
        return;
    }
    mi_free_aligned(block, alignment);
}

void free_possibly_foreign_sized_aligned(void* block, std::size_t size,
                                          std::size_t alignment) noexcept {
    if (block != nullptr && !mi_is_in_heap_region(block)) {
        log_foreign_route_once();
        _aligned_free(block);
        return;
    }
    mi_free_size_aligned(block, size, alignment);
}

}

void operator delete(void* p) noexcept { free_possibly_foreign(p); }
void operator delete[](void* p) noexcept { free_possibly_foreign(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { free_possibly_foreign(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { free_possibly_foreign(p); }

AIDA_MI_DECL_NEW(n) void* operator new(std::size_t n) noexcept(false) { return mi_new(n); }
AIDA_MI_DECL_NEW(n) void* operator new[](std::size_t n) noexcept(false) { return mi_new(n); }

AIDA_MI_DECL_NEW_NOTHROW(n) void* operator new(std::size_t n, const std::nothrow_t& tag) noexcept {
    (void)(tag);
    return mi_new_nothrow(n);
}
AIDA_MI_DECL_NEW_NOTHROW(n) void* operator new[](std::size_t n, const std::nothrow_t& tag) noexcept {
    (void)(tag);
    return mi_new_nothrow(n);
}

void operator delete(void* p, std::size_t n) noexcept { free_possibly_foreign_sized(p, n); }
void operator delete[](void* p, std::size_t n) noexcept { free_possibly_foreign_sized(p, n); }

void operator delete(void* p, std::align_val_t al) noexcept {
    free_possibly_foreign_aligned(p, static_cast<size_t>(al));
}
void operator delete[](void* p, std::align_val_t al) noexcept {
    free_possibly_foreign_aligned(p, static_cast<size_t>(al));
}
void operator delete(void* p, std::size_t n, std::align_val_t al) noexcept {
    free_possibly_foreign_sized_aligned(p, n, static_cast<size_t>(al));
}
void operator delete[](void* p, std::size_t n, std::align_val_t al) noexcept {
    free_possibly_foreign_sized_aligned(p, n, static_cast<size_t>(al));
}
void operator delete(void* p, std::align_val_t al, const std::nothrow_t&) noexcept {
    free_possibly_foreign_aligned(p, static_cast<size_t>(al));
}
void operator delete[](void* p, std::align_val_t al, const std::nothrow_t&) noexcept {
    free_possibly_foreign_aligned(p, static_cast<size_t>(al));
}

void* operator new(std::size_t n, std::align_val_t al) noexcept(false) {
    return mi_new_aligned(n, static_cast<size_t>(al));
}
void* operator new[](std::size_t n, std::align_val_t al) noexcept(false) {
    return mi_new_aligned(n, static_cast<size_t>(al));
}
void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return mi_new_aligned_nothrow(n, static_cast<size_t>(al));
}
void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return mi_new_aligned_nothrow(n, static_cast<size_t>(al));
}

namespace aida::infra::allocator {

namespace {

std::atomic<bool> g_backend_logged{false};

}

bool initialize() noexcept {
    const bool active = override_active();
    bool expected = false;
    if (g_backend_logged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        diag::log_tagged_fmt("allocator",
            "allocator_backend kind=mimalloc version=%u override_active=%u",
            static_cast<unsigned>(mi_version()),
            active ? 1u : 0u);
    }
    return active;
}

void trim() noexcept {
    mi_collect(true);
}

bool override_active() noexcept {
    void* block = ::operator new(sizeof(void*) * 2u, std::nothrow);
    if (block == nullptr)
        return false;
    const bool inside = mi_is_in_heap_region(block);
    ::operator delete(block);
    return inside;
}

std::uint64_t version() noexcept {
    return static_cast<std::uint64_t>(mi_version());
}

}

#undef AIDA_MI_DECL_NEW
#undef AIDA_MI_DECL_NEW_NOTHROW
