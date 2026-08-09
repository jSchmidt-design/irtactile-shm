// IrTactile.Shm.Stream - Client: mapping lifetime, the event, waiting, QPC.
//
// SPDX-License-Identifier: MIT
//
// OS half of the consumer API: owns the mapping and the event, drives Wait.
// Fully managed - no P/Invoke, no native dependency.
//
// Typical consumer (the names are the RESOLVED names - base name plus the
// producer's size-class suffix; see the spec, "Size classes and resolved
// names"):
//
//     using var c = new Client();
//     OpenResult r;
//     while ((r = c.Open("MyBridge_256k", "MyBridge_Event_256k", kExpected)).ShouldRetry())
//         Thread.Sleep(250);
//
//     var b = new BlockView();
//     while (c.Wait() != WaitResult.Closed)
//         while (c.Next(b))
//             render(b);
//
// Open-only: a typo'd section name fails with NotFound instead of silently
// polling an empty section.
//
// Single-threaded for Wait(): the producer's event is auto-reset and wakes
// exactly one waiter. Polling consumers (no event) can share a section freely.

using System.Diagnostics;
using System.IO.MemoryMappedFiles;

namespace IrTactile.Shm.Stream;

/// <summary>
/// Owns the mapping and the event; drives a <see cref="Reader"/> over them.
/// </summary>
public sealed unsafe class Client : IDisposable
{
    private MemoryMappedFile? _mmf;
    private MemoryMappedViewAccessor? _view;
    private EventWaitHandle? _event;
    private byte* _ptr;
    private bool _pointerAcquired;
    private bool _eventRequested;
    private string _eventFallbackReason = "";
    private ulong _lastSeq;
    private uint _lastLayoutSeq;
    /// <summary>The header as of the last reconfigure this Client reported -
    /// its own watermark, so classification never depends on who refreshed.</summary>
    private ShmStreamHeader _lastHeader;
    private readonly Reader _reader = new();

    // OperatingSystem.IsWindows() is net5+; netstandard2.0 consumers (.NET
    // Framework, Unity) get the RuntimeInformation spelling. The guard
    // attribute is what lets the platform-compatibility analyzer keep proving
    // that the windows-only OpenExisting calls below are reachable only on
    // Windows - without it, multi-targeting would trade two errors for two
    // CA1416 warnings.
#if NET5_0_OR_GREATER
    [System.Runtime.Versioning.SupportedOSPlatformGuard("windows")]
#endif
    private static bool IsWindows =>
#if NET5_0_OR_GREATER
        OperatingSystem.IsWindows();
#else
        System.Runtime.InteropServices.RuntimeInformation.IsOSPlatform(
            System.Runtime.InteropServices.OSPlatform.Windows);
#endif

    /// <summary>
    /// <paramref name="eventName"/> may be null or empty ⇒ polling mode, same
    /// API either way. <paramref name="expected"/> may be null ⇒ no structural
    /// assertion.
    /// </summary>
    public OpenResult Open(string shmName, string? eventName, StreamRequest? expected = null)
    {
        Close();
        if (string.IsNullOrEmpty(shmName)) return OpenResult.NotFound;

        // Kernel objects by name - section and event - are the Windows half
        // of the contract (win32.h in the C++ binding). The portable half is
        // Reader over any mapped buffer; on other platforms Open reports
        // NotFound rather than pretending a mapping exists.
        if (!IsWindows) return OpenResult.NotFound;

        try
        {
            // Read-only, both halves: MemoryMappedFileRights.Read is the
            // managed FILE_MAP_READ, and the view below inherits it, so a
            // buggy consumer faults instead of corrupting the producer's ring.
            // The single-argument OpenExisting would ask for ReadWrite and be
            // refused outright by a producer that shares the section read-only.
            _mmf = MemoryMappedFile.OpenExisting(shmName, MemoryMappedFileRights.Read);

            // Map the whole section in one call: the cost is address space
            // only - pages arrive on first touch.
            _view = _mmf.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read);

            byte* ptr = null;
            _view.SafeMemoryMappedViewHandle.AcquirePointer(ref ptr);
            _ptr = ptr;
            _pointerAcquired = true;
        }
        catch (FileNotFoundException) { Close(); return OpenResult.NotFound; }
        catch (DirectoryNotFoundException) { Close(); return OpenResult.NotFound; }
        catch (UnauthorizedAccessException) { Close(); return OpenResult.AccessDenied; }
        catch (Exception) { Close(); return OpenResult.Malformed; }

        // The real mapped size. Every offset is validated against this, never
        // against header claims (see Reader.ValidateLayout()).
        long mappedBytes = (long)_view.SafeMemoryMappedViewHandle.ByteLength;

        AttachResult ar = _reader.Attach((IntPtr)_ptr, mappedBytes, expected);
        if (ar != AttachResult.Ok)
        {
            // ReleaseHandles() rather than Close() keeps _reader, so
            // MismatchDetail survives Open() returning Mismatch. The retry
            // path for NotReady is simply another Open(); remapping a section
            // is cheap against any sane retry cadence.
            ReleaseHandles();
            return FromAttach(ar);
        }

        // A missing event leaves polling mode; EventFallback reports that it
        // did and EventFallbackReason says why. Named kernel events are a
        // Windows concept; elsewhere the client is always in polling mode.
        //
        // The spec asks for SYNCHRONIZE (wait-only). .NET's in-box
        // OpenExisting asks for EVENT_MODIFY_STATE | SYNCHRONIZE and offers no
        // narrower overload - narrowing it needs the
        // System.Threading.AccessControl package, which would cost this
        // assembly its no-dependency property for a right it then never
        // exercises (the Client only ever waits). The consequence is confined
        // to a producer that shares its event wait-only: the open is refused
        // and the Client polls instead, which is a latency difference, not a
        // correctness one. EventFallbackReason distinguishes that case from a
        // misspelled name.
        if (!string.IsNullOrEmpty(eventName))
        {
            _eventRequested = true;
            if (IsWindows)
            {
                try
                {
                    _event = EventWaitHandle.OpenExisting(eventName);
                }
                catch (WaitHandleCannotBeOpenedException)
                {
                    _eventFallbackReason = "no event of that name exists";
                }
                catch (UnauthorizedAccessException)
                {
                    _eventFallbackReason =
                        "access denied - the event exists but is not shared with " +
                        "EVENT_MODIFY_STATE | SYNCHRONIZE";
                }
                catch (Exception e)
                {
                    _eventFallbackReason = e.GetType().Name;
                }
            }
            else
            {
                _eventFallbackReason = "named events are Windows-only";
            }
        }

        // Seed the watermarks from the current header so the first Wait()
        // classifies as Blocks / NoBlocks rather than Reconfigured.
        _lastSeq = _reader.WriteSeq;
        _lastLayoutSeq = _reader.LayoutSeq;
        _lastHeader = _reader.Header;
        return OpenResult.Ok;
    }

    /// <summary>
    /// The OS handles only - the <see cref="Reader"/> (and MismatchDetail)
    /// survives. <see cref="Close"/> is the full reset a caller reaches for.
    /// </summary>
    public void ReleaseHandles()
    {
        // Before the view goes: the Reader must not keep a pointer into a
        // mapping that is about to be released. DetachFromMapping() preserves
        // MismatchDetail, which is the whole reason this is not Close().
        _reader.DetachFromMapping();

        if (_pointerAcquired && _view != null)
        {
            _view.SafeMemoryMappedViewHandle.ReleasePointer();
            _pointerAcquired = false;
        }
        _ptr = null;
        _event?.Dispose();
        _event = null;
        _view?.Dispose();
        _view = null;
        _mmf?.Dispose();
        _mmf = null;
        _eventRequested = false;
        _eventFallbackReason = "";
    }

    /// <summary>Full reset: handles plus reader state.</summary>
    public void Close()
    {
        ReleaseHandles();
        _reader.Attach(IntPtr.Zero, 0);
    }

    /// <inheritdoc/>
    public void Dispose() => Close();

    /// <summary>True between a successful Open and Close / a hostile reconfigure.</summary>
    public bool IsOpen => _ptr != null && _reader.Attached;

    /// <summary>
    /// True when Open was given an event name but the event could not be
    /// opened, so the Client is silently in polling mode. Usually a misspelled
    /// or unresolved event name - worth a log line, with
    /// <see cref="EventFallbackReason"/>.
    /// </summary>
    public bool EventFallback => _eventRequested && _event == null;

    /// <summary>
    /// Why <see cref="EventFallback"/> is true, ready to log. Empty when the
    /// event opened, or when none was asked for.
    /// </summary>
    public string EventFallbackReason => EventFallback ? _eventFallbackReason : "";

    /// <summary>Wait with the library's own timeout - max(50 ms, 4 × period).</summary>
    public WaitResult Wait() =>
        Wait(TimeSpan.FromMilliseconds(_reader.WaitTimeoutMs));

    /// <summary>
    /// The event is auto-reset, so wakes coalesce: this returns "there is
    /// something"; drain with the caller's <c>while (Next(b))</c> loop.
    /// </summary>
    public WaitResult Wait(TimeSpan timeout)
    {
        if (!IsOpen) return WaitResult.Closed;

        if (_event != null)
        {
            _event.WaitOne(timeout);
        }
        else
        {
            // Polling mode: same API, up to ~quarter-period extra latency. The
            // 0.5 ms floor avoids burning CPU on tight loops; the 5.0 ms
            // ceiling keeps latency under a 20 ms period, the slowest
            // reasonable update rate.
            double stepMs = _reader.PeriodMs / 4.0;
            if (stepMs < 0.5) stepMs = 0.5;
            if (stepMs > 5.0) stepMs = 5.0;
            long deadline = Stopwatch.GetTimestamp() +
                            (long)(timeout.TotalSeconds * Stopwatch.Frequency);
            for (;;)
            {
                if (_reader.WriteSeq != _lastSeq) break;
                if (_reader.LayoutSeq != _lastLayoutSeq) break;
                if (_reader.DeviceState == DeviceState.Closed) break;
                if (Stopwatch.GetTimestamp() >= deadline) break;
                Thread.Sleep(TimeSpan.FromMilliseconds(stepMs));
            }
        }

        return Classify();
    }

    // The three reads below are gated on IsOpen, not merely on Reader.Attached.
    // ReleaseHandles() unmaps the view while the Reader still holds a pointer
    // into it and still reports Attached; reaching the section through that
    // pointer is an access violation the CLR cannot turn into an exception, so
    // the gate is what makes the public ReleaseHandles() safe to call.

    /// <summary>Lossless read - see <see cref="Reader.Next"/>. False once closed.</summary>
    public bool Next(BlockView b) => IsOpen && _reader.Next(b);

    /// <summary>Latest-value-wins read - see <see cref="Reader.Latest"/>. False once closed.</summary>
    public bool Latest(BlockView b) => IsOpen && _reader.Latest(b);

    /// <summary>Liveness against the current QPC reading. Closed once closed.</summary>
    public StreamState State() => IsOpen ? _reader.State(NowQpc()) : StreamState.Closed;

    /// <summary>The reader over the mapped section.</summary>
    public Reader Reader => _reader;

    /// <summary>
    /// The event, for a consumer with its own wait loop: pass it to
    /// <see cref="WaitHandle.WaitAny(WaitHandle[])"/> alongside your own
    /// handles, then call <see cref="Next"/> and <see cref="State()"/>
    /// unchanged. Null in polling mode.
    /// </summary>
    public EventWaitHandle? NativeEvent => _event;

    /// <summary>QPC now, in the section's qpcFrequency units on Windows.</summary>
    public static ulong NowQpc() => (ulong)Stopwatch.GetTimestamp();

    private static OpenResult FromAttach(AttachResult r) => r switch
    {
        AttachResult.Ok => OpenResult.Ok,
        AttachResult.NotReady => OpenResult.NotReady,
        AttachResult.BadVersion => OpenResult.BadVersion,
        AttachResult.Malformed => OpenResult.Malformed,
        AttachResult.Mismatch => OpenResult.Mismatch,
        _ => OpenResult.Malformed,
    };

    private WaitResult Classify()
    {
        uint layout = _reader.LayoutSeq;
        if (layout != _lastLayoutSeq)
        {
            if (!_reader.RefreshSnapshot())
            {
                _lastLayoutSeq = _reader.LayoutSeq;
                _lastSeq = _reader.WriteSeq;
                return WaitResult.Closed;
            }
            // Classified against the header this Client last REPORTED, not
            // against Reader.LastReconfigureKind. That property describes the
            // most recent refresh, and Reader.Next() refreshes on every read
            // cycle - so a reconfigure landing mid-drain is already consumed by
            // the time Wait() is called again, and reading the property back
            // would yield None and misreport a geometry change as
            // ProducerRestarted ("layout unchanged"). A private watermark also
            // classifies correctly across several restarts in one interval.
            Reader.ReconfigureKind kind =
                Reader.ClassifyReconfigure(_lastHeader, _reader.Header);
            _lastHeader = _reader.Header;
            _lastLayoutSeq = _reader.LayoutSeq;
            _lastSeq = _reader.WriteSeq;
            return kind == Reader.ReconfigureKind.Geometry
                ? WaitResult.Reconfigured
                : WaitResult.ProducerRestarted;
        }
        if (_reader.DeviceState == DeviceState.Closed) return WaitResult.Closed;

        ulong seq = _reader.WriteSeq;
        if (seq != _lastSeq) { _lastSeq = seq; return WaitResult.Blocks; }
        return WaitResult.NoBlocks;
    }
}
