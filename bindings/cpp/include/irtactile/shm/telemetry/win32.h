// irtactile/shm/telemetry/win32.h - the Windows half of telemetry-shm v1:
// named segments (named shared memory; Windows: file mapping) for the producer (creates the inbound region, opens the
// return region), and for the consumer (opens the inbound region, creates the
// return region), plus QPC.
//
// SPDX-License-Identifier: MIT
//
//     // producer
//     ProducerSegment s;
//     Identity id; id.simName = "My Sim"; id.nominalRateMilliHz = 60000;   // 60 Hz
//     s.create(inboundName("demo"), 256, id);
//     Producer& p = s.producer();
//     p.declare(channels::standard::Id::EngineRpm);  p.commitLayout();
//     ...
//     ReturnView rv; if (s.pollReturn(rv)) { /* rv.isAccepted(slot) */ }

#ifndef IRTACTILE_SHM_TELEMETRY_WIN32_H
#define IRTACTILE_SHM_TELEMETRY_WIN32_H

#include "producer.h"
#include "reader.h"

#include <irtactile/shm/common/qpc.h>

#include <cstdlib>
#include <string>

namespace irtactile {
namespace shm {
namespace telemetry {

using shm::qpcFrequency;   // common/qpc.h; kept reachable as telemetry::qpcNow / qpcFrequency
using shm::qpcNow;

namespace detail {

inline std::wstring widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end());   // segment names are ASCII
}

/// True when `pid` is non-zero and no longer names a running process. A process we may not open counts as running.
inline bool processGone(uint32_t pid) noexcept
{
    if (pid == 0) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool exited = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    CloseHandle(h);
    return exited;
}

}  // namespace detail

/// A mapped, named view; closes on destruction.
class MappedView {
public:
    MappedView() = default;
    MappedView(const MappedView&) = delete;
    MappedView& operator=(const MappedView&) = delete;
    ~MappedView() { close(); }

    void close() noexcept
    {
        if (m_view) UnmapViewOfFile(m_view);
        if (m_mapping) CloseHandle(m_mapping);
        m_view = nullptr;
        m_mapping = nullptr;
        m_bytes = 0;
        m_existed = false;
    }
    [[nodiscard]] void* data() const noexcept { return m_view; }
    [[nodiscard]] size_t size() const noexcept { return m_bytes; }
    [[nodiscard]] bool isOpen() const noexcept { return m_view != nullptr; }
    /// True when the segment existed already (restart into a segment that is still held open).
    [[nodiscard]] bool existed() const noexcept { return m_existed; }

    /// CreateFileMappingW + map. An existing segment keeps its size.
    bool create(const std::wstring& name, size_t bytes, DWORD* error = nullptr)
    {
        close();
        m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       static_cast<DWORD>(static_cast<uint64_t>(bytes) >> 32),
                                       static_cast<DWORD>(bytes & 0xFFFFFFFFu), name.c_str());
        if (!m_mapping) { if (error) *error = GetLastError(); return false; }
        m_existed = (GetLastError() == ERROR_ALREADY_EXISTS);
        return mapAll(true, error);
    }
    /// OpenFileMappingW + map the whole segment. The size is what the OS reports, never a header field.
    bool open(const std::wstring& name, bool write, DWORD* error = nullptr)
    {
        close();
        m_mapping = OpenFileMappingW(write ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, FALSE, name.c_str());
        if (!m_mapping) { if (error) *error = GetLastError(); return false; }
        m_existed = true;
        return mapAll(write, error);
    }

private:
    bool mapAll(bool write, DWORD* error)
    {
        m_view = MapViewOfFile(m_mapping, write ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, 0, 0, 0);
        if (!m_view) { if (error) *error = GetLastError(); close(); return false; }
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(m_view, &mbi, sizeof(mbi)) == 0) { if (error) *error = GetLastError(); close(); return false; }
        m_bytes = mbi.RegionSize;
        return true;
    }

    HANDLE m_mapping{nullptr};
    void*  m_view{nullptr};
    size_t m_bytes{0};
    bool   m_existed{false};
};

/// The writer lock of an inbound segment: a named event that only its producer holds open. The kernel destroys it
/// with the last handle, so a crashed producer releases it and the next one restarts into the segment.
class WriterLock {
public:
    WriterLock() = default;
    WriterLock(const WriterLock&) = delete;
    WriterLock& operator=(const WriterLock&) = delete;
    ~WriterLock() { release(); }

    /// False with ERROR_ALREADY_EXISTS in `error`: another live producer holds the lock.
    bool acquire(const std::wstring& name, DWORD* error = nullptr)
    {
        release();
        m_handle = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        if (!m_handle) { if (error) *error = GetLastError(); return false; }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (error) *error = ERROR_ALREADY_EXISTS;
            release();
            return false;
        }
        return true;
    }
    void release() noexcept
    {
        if (m_handle) CloseHandle(m_handle);
        m_handle = nullptr;
    }
    [[nodiscard]] bool isHeld() const noexcept { return m_handle != nullptr; }

private:
    HANDLE m_handle{nullptr};
};

/// Producer: owns the inbound segment and the Producer bound to it; opens the return region on demand.
class ProducerSegment {
public:
    /// Creates (or restarts into) the segment for `capacity` slots and binds the Producer. `identity.qpcFrequency`
    /// and `identity.pid` are filled in when zero. WriterActive: another live producer writes this segment.
    /// SegmentFailed: the OS refused to create or map the segment. BadRate: see Identity::nominalRateMilliHz. On any
    /// failure the Producer is left unbound.
    OpenResult create(const std::string& inboundSegmentName, uint32_t capacity, Identity identity)
    {
        m_producer.close();   // before the views go: a failed recreate must not leave it bound to unmapped memory
        if (!isValidSegmentName(inboundSegmentName) || !isValidCapacity(capacity)) return OpenResult::SegmentFailed;
        if (!isValidNominalRate(identity.nominalRateMilliHz)) return OpenResult::BadRate;   // before any OS object
        if (identity.qpcFrequency == 0) identity.qpcFrequency = qpcFrequency();
        if (identity.pid == 0) identity.pid = GetCurrentProcessId();
        m_inbound.close();
        m_return.close();
        DWORD err = 0;
        if (!m_lock.acquire(detail::widen(writerLockNameFor(inboundSegmentName)), &err)) {
            return err == ERROR_ALREADY_EXISTS ? OpenResult::WriterActive : OpenResult::SegmentFailed;
        }
        m_returnName = returnNameFor(inboundSegmentName);
        if (!m_inbound.create(detail::widen(inboundSegmentName), Producer::requiredBytes(capacity))) {
            m_lock.release();
            return OpenResult::SegmentFailed;
        }
        const OpenResult r = m_producer.open(m_inbound.data(), m_inbound.size(), capacity, std::move(identity));
        if (r != OpenResult::Ok) {
            m_inbound.close();
            m_lock.release();
        }
        return r;
    }

    [[nodiscard]] Producer& producer() noexcept { return m_producer; }
    [[nodiscard]] bool restarted() const noexcept { return m_inbound.existed(); }

    /// Reads the return region (opening it lazily; absence is normal). False = no consistent view this time
    /// (region absent, not ready, or torn). Masks computed for another layout than the current one are cleared,
    /// so every slot reads as not accepted until irTactile has evaluated this layout. An Active consumer that is stale
    /// or whose process is gone sets `consumerGone`, so every accepted slot reads as demanded. False without a bound
    /// Producer (create() not called or failed).
    /// Once the region is attached, a false return still ages `out`: the layout check and `consumerGone` are applied
    /// to the view kept from the last good read, so a consumer that died mid-publication (retSeq left odd) is
    /// detected as gone, not obeyed indefinitely.
    bool pollReturn(ReturnView& out)
    {
        if (!m_producer.isOpen()) return false;
        if (!m_return.isOpen()) {
            if (!m_return.open(detail::widen(m_returnName), false)) return false;
            if (m_returnReader.attach(m_return.data(), m_return.size()) != AttachResult::Ok) {
                // Not ready yet (or malformed): try again on the next poll.
                m_return.close();
                return false;
            }
        }
        const bool fresh = m_returnReader.read(out);
        if (out.forLayoutSeq != shm::detail::acquireLoad(m_producer.publishedHeader().layoutSeq)) {
            out.accepted.clear();
            out.demanded.clear();
        }
        out.consumerGone = out.consumerState == ConsumerState::Active &&
                           (isConsumerStale(out.lastConsumeQpc, qpcNow(), qpcFrequency()) ||
                            detail::processGone(out.consumerPid));
        return fresh;
    }

private:
    WriterLock   m_lock;      // declared first: released after the views are unmapped
    MappedView   m_inbound;
    MappedView   m_return;
    Producer     m_producer;
    ReturnReader m_returnReader;
    std::string  m_returnName;
};

/// Consumer: opens an inbound segment read-only. Result mirrors the retry-vs-give-up split of stream-shm.
enum class SegmentOpen : uint32_t { Ok, NotFound, AccessDenied, Failed };

inline const char* toCString(SegmentOpen r) noexcept
{
    switch (r) {
        case SegmentOpen::Ok:           return "Ok";
        case SegmentOpen::NotFound:     return "NotFound";
        case SegmentOpen::AccessDenied: return "AccessDenied";
        case SegmentOpen::Failed:       return "Failed";
    }
    return "?";
}

class ConsumerSegment {
public:
    SegmentOpen open(const std::string& inboundSegmentName)
    {
        if (!isValidSegmentName(inboundSegmentName)) return SegmentOpen::Failed;
        DWORD err = 0;
        if (m_view.open(detail::widen(inboundSegmentName), false, &err)) return SegmentOpen::Ok;
        if (err == ERROR_FILE_NOT_FOUND) return SegmentOpen::NotFound;
        if (err == ERROR_ACCESS_DENIED) return SegmentOpen::AccessDenied;
        return SegmentOpen::Failed;
    }
    void close() noexcept { m_view.close(); }
    [[nodiscard]] bool isOpen() const noexcept { return m_view.isOpen(); }
    [[nodiscard]] const void* data() const noexcept { return m_view.data(); }
    [[nodiscard]] size_t size() const noexcept { return m_view.size(); }

private:
    MappedView m_view;
};

/// Consumer: creates the return region and publishes into it.
class ReturnSegment {
public:
    bool create(const std::string& inboundSegmentName)
    {
        if (!isValidSegmentName(inboundSegmentName)) return false;
        if (!m_view.create(detail::widen(returnNameFor(inboundSegmentName)), kReturnBytes)) return false;
        return m_writer.init(m_view.data(), m_view.size(), GetCurrentProcessId());
    }
    void close() noexcept { m_view.close(); }
    [[nodiscard]] bool isOpen() const noexcept { return m_view.isOpen(); }
    [[nodiscard]] ReturnWriter& writer() noexcept { return m_writer; }

private:
    MappedView   m_view;
    ReturnWriter m_writer;
};

}  // namespace telemetry
}  // namespace shm
}  // namespace irtactile

#endif  // IRTACTILE_SHM_TELEMETRY_WIN32_H
