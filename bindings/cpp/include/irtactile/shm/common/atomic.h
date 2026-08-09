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
//  - x86-64's TSO model already provides acquire/release ordering, so the
//    accessor above compiles to plain movs there. ARM64 needs real barriers
//    (dmb/ldar/stlr); a non-C++ binding must emit them itself on that target.
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

} // namespace detail

} // namespace shm
} // namespace irtactile

#endif // IRTACTILE_SHM_COMMON_ATOMIC_H
