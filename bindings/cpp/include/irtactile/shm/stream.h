// irtactile/shm/stream.h - umbrella header for the irTactile shared-memory
// audio stream contract, format v1.0.
//
// SPDX-License-Identifier: MIT
//
//   stream/layout.h   wire format: structs, constants, size arithmetic,
//                         addressing. Source for foreign-language bindings.
//   common/atomic.h   acquire/release accessors for the concurrently-
//                         written header fields (bytes [80, 112)).
//   stream/reader.h   consumer API (BlockView, StreamRequest, Reader)
//                         over a caller-supplied buffer.
//
// See stream/win32.h for the Windows mapping/event layer.
//
// Minimal consumer:
//
//     BlockView b;
//     while (reader.next(b))
//         for (uint32_t ch = 0; ch < reader.channelCount(); ++ch) {
//             const float* s = b.channel(ch);
//             for (uint32_t i = 0; i < b.sampleCount; ++i) use(s[i]);
//         }

#ifndef IRTACTILE_SHM_STREAM_H
#define IRTACTILE_SHM_STREAM_H

#include <irtactile/shm/common/atomic.h>
#include <irtactile/shm/stream/layout.h>
#include <irtactile/shm/stream/reader.h>

#endif // IRTACTILE_SHM_STREAM_H
