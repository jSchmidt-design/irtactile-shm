// irtactile/shm/stream/win32.h - the Windows half of the consumer API:
// mapping lifetime, the event, waiting, QPC.
//
// SPDX-License-Identifier: MIT
//
// Typical consumer (the names are the RESOLVED names - base name plus the
// producer's size-class suffix; see the spec, "Size classes and resolved
// names"):
//
//     Client c;
//     OpenResult r;
//     while (shouldRetry(r = c.open(L"MyBridge_256k", L"MyBridge_Event_256k", &kExpected)))
//         std::this_thread::sleep_for(std::chrono::milliseconds(250));
//
//     BlockView b;
//     while (c.wait() != WaitResult::Closed)
//         while (c.next(b))
//             render(b);

#ifndef IRTACTILE_SHM_STREAM_WIN32_H
#define IRTACTILE_SHM_STREAM_WIN32_H

#include "reader.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <string>
#include <thread>

namespace irtactile {
namespace shm {
namespace stream {

/// What the consumer should do next.
enum class WaitResult : uint32_t {
    Blocks,        ///< writeSeq advanced - drain with next() / latest()
    NoBlocks,      ///< woken or timed out with nothing new; ask state() why
    Reconfigured,  ///< Same-section rewrite of an addressing-relevant field.
                   ///< The snapshot is already refreshed, so header() and all
                   ///< layout-derived accessors are valid for the new
                   ///< layoutSeq. Re-check any StreamRequest.
    ProducerRestarted, ///< Producer restart that left the memory layout
                       ///< unchanged; only cosmetic/diagnostic fields differ.
    Closed,        ///< producer shut down cleanly; drain what is left, then
                   ///< stop. Also returned when a header rewrite failed
                   ///< re-validation - the reader is detached either way and
                   ///< the stream is over for this Client.
};

/// Client's failure surface is Reader's plus lookup failures. Same
/// retry-vs-give-up split.
enum class OpenResult : uint32_t {
    Ok,
    NotFound,      ///< ERROR_FILE_NOT_FOUND - irTactile is not up yet. RETRY.
    NotReady,      ///< section exists, header not yet written. RETRY.
    AccessDenied,  ///< wrong session, or a Global\ name without the privilege.
    BadVersion,    ///< as AttachResult
    Malformed,     ///< as AttachResult
    Mismatch,      ///< as AttachResult - the installed device profile is not yours.
};

constexpr bool shouldRetry(OpenResult r) noexcept
{
    return r == OpenResult::NotFound || r == OpenResult::NotReady;
}

inline const char* toCString(OpenResult r) noexcept
{
    switch (r) {
        case OpenResult::Ok:           return "Ok";
        case OpenResult::NotFound:     return "NotFound";
        case OpenResult::NotReady:     return "NotReady";
        case OpenResult::AccessDenied: return "AccessDenied";
        case OpenResult::BadVersion:   return "BadVersion";
        case OpenResult::Malformed:    return "Malformed";
        case OpenResult::Mismatch:     return "Mismatch";
    }
    return "?";
}

inline const char* toCString(WaitResult r) noexcept
{
    switch (r) {
        case WaitResult::Blocks:            return "Blocks";
        case WaitResult::NoBlocks:          return "NoBlocks";
        case WaitResult::Reconfigured:      return "Reconfigured";
        case WaitResult::ProducerRestarted: return "ProducerRestarted";
        case WaitResult::Closed:            return "Closed";
    }
    return "?";
}

inline const char* toCString(StreamState s) noexcept
{
    switch (s) {
        case StreamState::Live:    return "Live";
        case StreamState::Quiet:   return "Quiet";
        case StreamState::Stalled: return "Stalled";
        case StreamState::Closed:  return "Closed";
    }
    return "?";
}

/// Open-only: a typo'd section name fails with NotFound instead of silently
/// polling an empty section.
///
/// Move-only, and single-threaded for wait(): the producer's event is
/// auto-reset and wakes exactly one waiter. Polling consumers (no event) can
/// share a section freely.
#if defined(_MSC_VER)
#  pragma warning(push)
// C4324: struct padded due to alignas - expected, m_lastHeader is alignas(64).
#  pragma warning(disable: 4324)
#endif
class Client {
public:
    Client() = default;
    ~Client() { close(); }

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    Client(Client&& o) noexcept { moveFrom(o); }
    Client& operator=(Client&& o) noexcept
    {
        if (this != &o) { close(); moveFrom(o); }
        return *this;
    }

    /// `eventName` may be null or empty ⇒ polling mode, same API either way.
    /// `expected` may be null ⇒ no structural assertion.
    OpenResult open(const wchar_t* shmName, const wchar_t* eventName,
                    const StreamRequest* expected = nullptr)
    {
        close();
        if (!shmName || !*shmName) return OpenResult::NotFound;

        // FILE_MAP_READ: a buggy consumer faults instead of corrupting the
        // producer's ring.
        m_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, shmName);
        if (!m_mapping) {
            const DWORD err = GetLastError();
            if (err == ERROR_ACCESS_DENIED) return OpenResult::AccessDenied;
            return OpenResult::NotFound;
        }

        // Map the whole section in one call: the view is 64 KB-aligned
        // (preserving alignas(64)), and the cost is address space only -
        // pages arrive on first touch.
        m_view = MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, 0);
        if (!m_view) {
            const DWORD err = GetLastError();
            close();
            return err == ERROR_ACCESS_DENIED ? OpenResult::AccessDenied : OpenResult::Malformed;
        }

        // The real mapped size. Every offset is validated against this, never
        // against header claims (see Reader::validateLayout()).
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(m_view, &mbi, sizeof(mbi)) == 0) { close(); return OpenResult::Malformed; }
        m_mappedBytes = mbi.RegionSize;

        const AttachResult ar = m_reader.attach(m_view, m_mappedBytes, expected);
        if (ar != AttachResult::Ok) {
            // releaseHandles() rather than close() keeps m_reader, so
            // mismatchDetail() survives open() returning Mismatch. The retry
            // path for NotReady is simply another open(); remapping a section
            // is cheap against any sane retry cadence.
            releaseHandles();
            return fromAttach(ar);
        }

        // SYNCHRONIZE access (wait-only). A missing event leaves polling mode;
        // eventFallback() reports that it did.
        if (eventName && *eventName) {
            m_event = OpenEventW(SYNCHRONIZE, FALSE, eventName);
            m_eventRequested = true;
        }

        // Seed the watermarks from the current header so the first wait()
        // classifies as Blocks / NoBlocks rather than Reconfigured.
        m_lastSeq       = m_reader.writeSeq();
        m_lastLayoutSeq = m_reader.layoutSeq();
        m_lastHeader    = m_reader.header();
        return OpenResult::Ok;
    }

    /// The OS handles only - m_reader (and mismatchDetail()) survives.
    /// close() below is the full reset a caller reaches for.
    void releaseHandles()
    {
        if (m_event)   { CloseHandle(m_event);   m_event   = nullptr; }
        if (m_view)    { UnmapViewOfFile(m_view); m_view   = nullptr; }
        if (m_mapping) { CloseHandle(m_mapping); m_mapping = nullptr; }
        m_mappedBytes    = 0;
        m_eventRequested = false;
    }

    void close()
    {
        releaseHandles();
        m_reader = Reader{};
    }

    bool isOpen() const noexcept { return m_view != nullptr && m_reader.attached(); }

    /// True when open() was given an event name but no event of that name
    /// existed, so the Client is silently in polling mode. Usually a
    /// misspelled or unresolved event name - worth a log line.
    bool eventFallback() const noexcept { return m_eventRequested && m_event == nullptr; }

    /// Wait with the library's own timeout - max(50 ms, 4 × period).
    WaitResult wait() { return wait(std::chrono::milliseconds(
                            static_cast<long long>(m_reader.waitTimeoutMs() + 0.5))); }

    /// The event is auto-reset, so wakes coalesce: this returns "there is
    /// something"; drain with the caller's `while (next(b))` loop.
    WaitResult wait(std::chrono::milliseconds timeout)
    {
        if (!isOpen()) return WaitResult::Closed;

        if (m_event) {
            WaitForSingleObject(m_event, static_cast<DWORD>(timeout.count()));
        } else {
            // Polling mode: same API, up to ~quarter-period extra latency. The
            // 0.5 ms floor avoids burning CPU on tight loops; the 5.0 ms
            // ceiling keeps latency under a 20 ms period, the slowest
            // reasonable update rate.
            double stepMs = m_reader.periodMs() / 4.0;
            if (stepMs < 0.5) stepMs = 0.5;
            if (stepMs > 5.0) stepMs = 5.0;
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            for (;;) {
                if (m_reader.writeSeq() != m_lastSeq)        break;
                if (m_reader.layoutSeq() != m_lastLayoutSeq) break;
                if (m_reader.deviceState() == DeviceState::Closed) break;
                if (std::chrono::steady_clock::now() >= deadline) break;
                std::this_thread::sleep_for(
                    std::chrono::microseconds(static_cast<long long>(stepMs * 1000.0)));
            }
        }

        return classify();
    }

    bool next(BlockView& b)   { return m_reader.next(b); }
    bool latest(BlockView& b) { return m_reader.latest(b); }

    StreamState state() const { return m_reader.state(nowQpc()); }

    Reader&       reader() noexcept       { return m_reader; }
    const Reader& reader() const noexcept { return m_reader; }

    /// The event, for a consumer with its own wait loop: pass it to
    /// WaitForMultipleObjects alongside your own handles, then call next()
    /// and state() unchanged. Null in polling mode.
    HANDLE nativeEvent() const noexcept { return m_event; }

    uint64_t nowQpc() const noexcept
    {
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return static_cast<uint64_t>(c.QuadPart);
    }

private:
    static OpenResult fromAttach(AttachResult r) noexcept
    {
        switch (r) {
            case AttachResult::Ok:         return OpenResult::Ok;
            case AttachResult::NotReady:   return OpenResult::NotReady;
            case AttachResult::BadVersion: return OpenResult::BadVersion;
            case AttachResult::Malformed:  return OpenResult::Malformed;
            case AttachResult::Mismatch:   return OpenResult::Mismatch;
        }
        return OpenResult::Malformed;
    }

    WaitResult classify()
    {
        const uint32_t layout = m_reader.layoutSeq();
        if (layout != m_lastLayoutSeq) {
            if (!m_reader.refreshSnapshot()) {
                m_lastLayoutSeq = m_reader.layoutSeq();
                m_lastSeq       = m_reader.writeSeq();
                return WaitResult::Closed;
            }
            // Classified against the header this Client last REPORTED, not
            // against Reader::lastReconfigureKind(). That accessor describes
            // the most recent refresh, and Reader::next() refreshes on every
            // read cycle - so a reconfigure landing mid-drain is already
            // consumed by the time wait() is called again, and reading it back
            // would yield None and misreport a geometry change as
            // ProducerRestarted ("layout unchanged"). A private watermark also
            // classifies correctly across several restarts in one interval.
            const auto kind = Reader::classifyReconfigure(m_lastHeader, m_reader.header());
            m_lastHeader    = m_reader.header();
            m_lastLayoutSeq = m_reader.layoutSeq();
            m_lastSeq       = m_reader.writeSeq();
            return kind == Reader::ReconfigureKind::Geometry
                   ? WaitResult::Reconfigured
                   : WaitResult::ProducerRestarted;
        }
        if (m_reader.deviceState() == DeviceState::Closed) return WaitResult::Closed;

        const uint64_t seq = m_reader.writeSeq();
        if (seq != m_lastSeq) { m_lastSeq = seq; return WaitResult::Blocks; }
        return WaitResult::NoBlocks;
    }

    void moveFrom(Client& o) noexcept
    {
        m_mapping        = o.m_mapping;        o.m_mapping = nullptr;
        m_view           = o.m_view;           o.m_view    = nullptr;
        m_event          = o.m_event;          o.m_event   = nullptr;
        m_mappedBytes    = o.m_mappedBytes;    o.m_mappedBytes = 0;
        m_eventRequested = o.m_eventRequested; o.m_eventRequested = false;
        m_lastSeq        = o.m_lastSeq;
        m_lastLayoutSeq  = o.m_lastLayoutSeq;
        m_lastHeader     = o.m_lastHeader;
        m_reader         = o.m_reader;
        o.m_reader       = Reader{};
    }

    HANDLE   m_mapping        = nullptr;
    void*    m_view           = nullptr;
    HANDLE   m_event          = nullptr;
    size_t   m_mappedBytes    = 0;
    bool     m_eventRequested = false;
    uint64_t m_lastSeq        = 0;
    uint32_t m_lastLayoutSeq  = 0;
    /// The header as of the last reconfigure this Client reported - its own
    /// watermark, so classification never depends on who refreshed.
    ShmStreamHeader m_lastHeader{};
    Reader   m_reader;
};
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

} // namespace stream
} // namespace shm
} // namespace irtactile

#endif // IRTACTILE_SHM_STREAM_WIN32_H
