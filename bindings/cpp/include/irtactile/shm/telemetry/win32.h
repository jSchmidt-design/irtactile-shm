// irtactile/shm/telemetry/win32.h - the Windows half of telemetry-shm v1:
// named sections for the producer (creates the inbound region, opens the
// return region), and for the consumer (opens the inbound region, creates the
// return region), plus QPC and the environment contract.
//
// SPDX-License-Identifier: MIT
//
//     // producer
//     ProducerSection s;
//     s.create(sectionNameFromEnvironment("demo"), 256, {"", "My Sim"});
//     Producer& p = s.producer();
//     p.declare(core::Id::EngineRpm);  p.commitLayout();
//     ...
//     ReturnView rv; if (s.pollReturn(rv)) { /* rv.isAccepted(slot) */ }

#ifndef IRTACTILE_SHM_TELEMETRY_WIN32_H
#define IRTACTILE_SHM_TELEMETRY_WIN32_H

#include "producer.h"
#include "reader.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <string>

namespace irtactile {
namespace shm {
namespace telemetry {

inline constexpr const char* kEnvSource  = "IRTACTILE_TELEMETRY_SOURCE";
inline constexpr const char* kEnvSection = "IRTACTILE_TELEMETRY_SECTION";

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

namespace detail {

inline std::string getEnv(const char* name)
{
    char buf[256];
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}
inline std::wstring widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end());   // section names are ASCII
}

}  // namespace detail

/// Inbound section name for this process: `IRTACTILE_TELEMETRY_SECTION`, else derived from
/// `IRTACTILE_TELEMETRY_SOURCE`, else from `defaultSourceId`.
inline std::string sectionNameFromEnvironment(const std::string& defaultSourceId)
{
    std::string section = detail::getEnv(kEnvSection);
    if (!section.empty()) return section;
    std::string source = detail::getEnv(kEnvSource);
    return inboundName(source.empty() ? defaultSourceId : source);
}

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
    }
    [[nodiscard]] void* data() const noexcept { return m_view; }
    [[nodiscard]] size_t size() const noexcept { return m_bytes; }
    [[nodiscard]] bool isOpen() const noexcept { return m_view != nullptr; }
    /// True when the section existed already (restart into a section that is still held open).
    [[nodiscard]] bool existed() const noexcept { return m_existed; }

    /// CreateFileMappingW + map. An existing section keeps its size.
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
    /// OpenFileMappingW + map the whole section. The size is what the OS reports, never a header field.
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

/// Producer: owns the inbound section and the Producer bound to it; opens the return region on demand.
class ProducerSection {
public:
    /// Creates (or restarts into) the section for `capacity` slots and binds the Producer. `identity.qpcFrequency`
    /// and `identity.pid` are filled in when zero.
    OpenResult create(const std::string& inboundSectionName, uint32_t capacity, Identity identity)
    {
        if (identity.qpcFrequency == 0) identity.qpcFrequency = qpcFrequency();
        if (identity.pid == 0) identity.pid = GetCurrentProcessId();
        m_returnName = returnNameFor(inboundSectionName);
        if (!m_inbound.create(detail::widen(inboundSectionName), Producer::requiredBytes(capacity))) {
            return OpenResult::TooSmall;
        }
        return m_producer.open(m_inbound.data(), m_inbound.size(), capacity, std::move(identity));
    }

    [[nodiscard]] Producer& producer() noexcept { return m_producer; }
    [[nodiscard]] bool restarted() const noexcept { return m_inbound.existed(); }

    /// Reads the return region (opening it lazily; absence is normal). False = nothing new to report.
    bool pollReturn(ReturnView& out)
    {
        if (!m_return.isOpen()) {
            if (!m_return.open(detail::widen(m_returnName), false)) return false;
            if (m_returnReader.attach(m_return.data(), m_return.size()) != AttachResult::Ok) {
                // Not ready yet (or malformed): try again on the next poll.
                m_return.close();
                return false;
            }
        }
        return m_returnReader.read(out);
    }

private:
    MappedView   m_inbound;
    MappedView   m_return;
    Producer     m_producer;
    ReturnReader m_returnReader;
    std::string  m_returnName;
};

/// Consumer: opens an inbound section read-only. Result mirrors the retry-vs-give-up split of stream-shm.
enum class SectionOpen : uint32_t { Ok, NotFound, AccessDenied, Failed };

class ConsumerSection {
public:
    SectionOpen open(const std::string& inboundSectionName)
    {
        DWORD err = 0;
        if (m_view.open(detail::widen(inboundSectionName), false, &err)) return SectionOpen::Ok;
        if (err == ERROR_FILE_NOT_FOUND) return SectionOpen::NotFound;
        if (err == ERROR_ACCESS_DENIED) return SectionOpen::AccessDenied;
        return SectionOpen::Failed;
    }
    void close() noexcept { m_view.close(); }
    [[nodiscard]] bool isOpen() const noexcept { return m_view.isOpen(); }
    [[nodiscard]] const void* data() const noexcept { return m_view.data(); }
    [[nodiscard]] size_t size() const noexcept { return m_view.size(); }

private:
    MappedView m_view;
};

/// Consumer: creates the return region and publishes into it.
class ReturnSection {
public:
    bool create(const std::string& inboundSectionName)
    {
        if (!m_view.create(detail::widen(returnNameFor(inboundSectionName)), kReturnBytes)) return false;
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
