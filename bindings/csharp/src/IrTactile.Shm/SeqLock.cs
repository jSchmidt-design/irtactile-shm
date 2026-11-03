// IrTactile.Shm - seqlock ordering helpers shared by the stream and telemetry code.
//
// SPDX-License-Identifier: MIT
//
// Volatile.Read is an acquire load: it keeps LATER accesses after it.
// Volatile.Write is a release store: it keeps EARLIER accesses before it.
// A seqlock also needs the opposite direction at two points, which neither
// gives (spec/common/seqlock.md):
//
//  - Writer: the protected stores that follow the odd counter store must not
//    become visible before it. BeginWrite() stores the odd value, then fences.
//  - Reader: the protected loads (the copy) must complete before the closing
//    counter recheck. BeforeRecheck() fences; call it after the copy and before
//    the first recheck load.
//
// The opening reader load and the closing writer store stay plain
// Volatile.Read / Volatile.Write. .NET has no standalone acquire or release
// fence, so both helpers use a full fence: Thread.MemoryBarrier(), the same
// barrier as Interlocked.MemoryBarrier() but also available on netstandard2.0.
// It is a locked instruction on x64 and dmb ish on ARM64, and it runs once per
// frame or block, which is negligible at these rates.
//
// This mirrors irtactile::shm::detail::seqWriteBegin / seqReadBeforeRecheck in
// bindings/cpp/include/irtactile/shm/common/atomic.h.

using System.Runtime.CompilerServices;

namespace IrTactile.Shm;

internal static class SeqLock
{
    /// <summary>Stores the odd counter value, then fences so no later protected store is visible before it.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void BeginWrite(ref uint seq, uint odd)
    {
        Volatile.Write(ref seq, odd);
        Thread.MemoryBarrier();
    }

    /// <inheritdoc cref="BeginWrite(ref uint, uint)"/>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void BeginWrite(ref ulong seq, ulong odd)
    {
        Volatile.Write(ref seq, odd);
        Thread.MemoryBarrier();
    }

    /// <summary>Fences so no protected load before this call completes after the counter rechecks that follow it.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void BeforeRecheck() => Thread.MemoryBarrier();
}
