// Test helpers: a GC-pinned stand-in for a mapped segment, and raw
// accessors/release stores over it.
//
// SPDX-License-Identifier: MIT
//
// Every test here runs over a plain pinned byte[] - no real mapping, no
// Win32 object, no thread. That mirrors the C++ suite
// (bindings/cpp/tests/stream/StreamShm_test.cpp): the publish protocol, the
// addressing arithmetic and the reader's forward-compatibility rules are all
// testable as pure data transformations.

using System.Runtime.InteropServices;

namespace IrTactile.Shm.Tests.Stream;

/// <summary>A pinned byte buffer standing in for a mapped segment.</summary>
internal sealed unsafe class PinnedSegment : IDisposable
{
    private readonly GCHandle _pin;

    public PinnedSegment(long size)
    {
        Bytes = new byte[size];
        _pin = GCHandle.Alloc(Bytes, GCHandleType.Pinned);
        Raw = (byte*)_pin.AddrOfPinnedObject();
    }

    public PinnedSegment(byte[] content)
    {
        Bytes = content;
        _pin = GCHandle.Alloc(Bytes, GCHandleType.Pinned);
        Raw = (byte*)_pin.AddrOfPinnedObject();
    }

    public byte[] Bytes { get; }
    public byte* Raw { get; }
    public IntPtr Ptr => (IntPtr)Raw;
    public long Length => Bytes.LongLength;

    public void Dispose() => _pin.Free();
}

/// <summary>
/// Plain and release-store accessors over a segment buffer. Release stores
/// mirror the producer's obligations; the Reader under test does the acquire
/// loads.
/// </summary>
internal static unsafe class SegmentMemory
{
    public static ushort U16(byte* b, int off) => *(ushort*)(b + off);
    public static uint U32(byte* b, int off) => *(uint*)(b + off);
    public static ulong U64(byte* b, int off) => *(ulong*)(b + off);

    public static void SetU16(byte* b, int off, ushort v) => *(ushort*)(b + off) = v;
    public static void SetU32(byte* b, int off, uint v) => *(uint*)(b + off) = v;
    public static void SetU64(byte* b, int off, ulong v) => *(ulong*)(b + off) = v;

    public static void Release32(byte* b, int off, uint v) =>
        Volatile.Write(ref *(uint*)(b + off), v);

    public static void Release64(byte* b, int off, ulong v) =>
        Volatile.Write(ref *(ulong*)(b + off), v);
}
