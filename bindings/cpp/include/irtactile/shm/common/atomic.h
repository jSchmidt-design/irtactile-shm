// irtactile/shm/common/atomic.h - acquire/release accessors for the six
// concurrently-written fields of ShmStreamHeader (bytes [80, 112)).
//
// SPDX-License-Identifier: MIT
//
// The producer publishes with releaseStore(), the Reader reads with
// acquireLoad().

#ifndef IRTACTILE_SHM_COMMON_ATOMIC_H
#define IRTACTILE_SHM_COMMON_ATOMIC_H

#include <atomic>

#if defined(__cpp_lib_atomic_ref) && __cpp_lib_atomic_ref >= 201806L
#  define IRTACTILE_SHM_HAS_ATOMIC_REF 1
#else
#  define IRTACTILE_SHM_HAS_ATOMIC_REF 0
#endif

namespace irtactile {
namespace shm {

// ===========================================================================
// Atomic accessors
// ===========================================================================
//
// The concurrent fields are plain integers (a fixed byte layout for foreign
// bindings); access them only through these accessors.
//
// For non-C++ bindings:
//  - x86-64's TSO model already provides acquire/release ordering, so these
//    accessors compile to plain movs there. ARM64 needs real barriers
//    (dmb/ldar/stlr); a non-C++ binding must emit them itself on that target.
//  - Acquire/release accessors alone are NOT enough for a seqlock: see the
//    seqlock helpers below (spec/common/seqlock.md).
//  - 64-bit fields must be naturally aligned to be tear-free; the layout
//    guarantees this.
//  - A 32-bit consumer must use an 8-byte atomic load (cmpxchg8b) for the
//    64-bit fields; a plain read can tear.

namespace detail {

template <class T>
inline T acquireLoad(const T& v) noexcept
{
#if IRTACTILE_SHM_HAS_ATOMIC_REF
    return std::atomic_ref<T>(const_cast<T&>(v)).load(std::memory_order_acquire);
#else
    const T x = *static_cast<const volatile T*>(&v);
    std::atomic_thread_fence(std::memory_order_acquire);
    return x;
#endif
}

template <class T>
inline void releaseStore(T& v, T x) noexcept
{
#if IRTACTILE_SHM_HAS_ATOMIC_REF
    std::atomic_ref<T>(v).store(x, std::memory_order_release);
#else
    std::atomic_thread_fence(std::memory_order_release);
    *static_cast<volatile T*>(&v) = x;
#endif
}

// ===========================================================================
// Seqlock helpers
// ===========================================================================
//
// An acquire load keeps LATER accesses after it, and a release store keeps
// EARLIER accesses before it. A seqlock also needs the opposite direction at
// two points, which the accessors above do not give:
//
//  - Writer: the protected stores that follow the odd counter store must not
//    become visible before it. seqWriteBegin() stores the odd value and then
//    issues a release fence.
//  - Reader: the protected loads (the copy) must complete before the closing
//    counter recheck. seqReadBeforeRecheck() issues an acquire fence; call it
//    after the copy and before the first recheck load.
//
// The opening reader load (acquireLoad) and the closing writer store
// (releaseStore) need nothing extra. On x86-64 both fences are compiler
// barriers only; on ARM64 they emit dmb.
//
// Platform assumption: the protected bytes are copied with plain loads and
// stores (memcpy) that race with the other process. That is formally a data
// race in ISO C++; these helpers rely on MSVC, Clang and GCC on x86-64 and
// ARM64 treating such accesses as ordinary machine loads and stores ordered by
// the fences, which they do. The seqlock rechecks discard any torn copy.

/// Opens a seqlock write: stores the odd counter value `odd`, then fences so
/// no later protected store becomes visible before it.
template <class T>
inline void seqWriteBegin(T& seq, T odd) noexcept
{
    releaseStore(seq, odd);
    std::atomic_thread_fence(std::memory_order_release);
}

/// Closes a seqlock read copy: no protected load before this call may complete
/// after the counter rechecks that follow it.
inline void seqReadBeforeRecheck() noexcept
{
    std::atomic_thread_fence(std::memory_order_acquire);
}

} // namespace detail

} // namespace shm
} // namespace irtactile

#endif // IRTACTILE_SHM_COMMON_ATOMIC_H
