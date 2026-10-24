// irtactile/shm/common/qpc.h - QueryPerformanceCounter time stamps, the clock
// of every *Qpc field in the shm formats. Windows only; nothing else from the
// section handling is pulled in.
//
// SPDX-License-Identifier: MIT

#ifndef IRTACTILE_SHM_COMMON_QPC_H
#define IRTACTILE_SHM_COMMON_QPC_H

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace irtactile {
namespace shm {

inline uint64_t qpcNow() noexcept
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

inline uint64_t qpcFrequency() noexcept
{
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

}  // namespace shm
}  // namespace irtactile

#endif  // IRTACTILE_SHM_COMMON_QPC_H
