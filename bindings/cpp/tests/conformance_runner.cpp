// bindings/cpp/tests/conformance_runner.cpp - runs the stream-shm conformance
// vectors against the C++ binding, exactly as a foreign-language binding's
// harness would: from manifest.json and the golden blobs alone.
//
// SPDX-License-Identifier: MIT
//
//   conformance_runner <path/to/conformance/stream-shm/v1/manifest.json>
//
// Exit code 0 when every case passes, 1 otherwise. The op semantics are
// documented in conformance/README.md; this file is the reference harness
// other bindings' runners are written against.

#include <irtactile/shm/stream.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace irtactile::shm::stream;
using nlohmann::json;

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Runner state
// ---------------------------------------------------------------------------

int         g_failures = 0;
int         g_checks   = 0;
std::string g_caseId;

void fail(const std::string& msg)
{
    ++g_failures;
    std::printf("  FAIL [%s] %s\n", g_caseId.c_str(), msg.c_str());
}

void check(bool ok, const std::string& msg)
{
    ++g_checks;
    if (!ok) fail(msg);
}

void checkU64(uint64_t got, uint64_t want, const char* what)
{
    ++g_checks;
    if (got != want) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s: got %llu, want %llu", what,
                      static_cast<unsigned long long>(got),
                      static_cast<unsigned long long>(want));
        fail(buf);
    }
}

bool attachResultFromString(const std::string& s, AttachResult& out)
{
    if (s == "Ok")         { out = AttachResult::Ok;         return true; }
    if (s == "NotReady")   { out = AttachResult::NotReady;   return true; }
    if (s == "BadVersion") { out = AttachResult::BadVersion; return true; }
    if (s == "Malformed")  { out = AttachResult::Malformed;  return true; }
    if (s == "Mismatch")   { out = AttachResult::Mismatch;   return true; }
    return false;
}

const char* reconfigureKindName(Reader::ReconfigureKind k)
{
    switch (k) {
        case Reader::ReconfigureKind::None:     return "None";
        case Reader::ReconfigureKind::Geometry: return "Geometry";
        case Reader::ReconfigureKind::Cosmetic: return "Cosmetic";
    }
    return "?";
}

const char* streamStateName(StreamState s)
{
    switch (s) {
        case StreamState::Live:    return "Live";
        case StreamState::Quiet:   return "Quiet";
        case StreamState::Stalled: return "Stalled";
        case StreamState::Closed:  return "Closed";
    }
    return "?";
}

/// FNV-1a 64 over the LE float32 bits of the decoded samples, planar
/// channel-major - the manifest's samplesHash convention.
uint64_t hashSamples(const BlockView& b)
{
    uint64_t h = 14695981039346656037ull;
    for (uint32_t ch = 0; ch < b.channelCount(); ++ch) {
        const float* s = b.channel(ch);
        for (uint32_t i = 0; i < b.sampleCount; ++i) {
            uint32_t u;
            std::memcpy(&u, &s[i], 4);
            for (int k = 0; k < 4; ++k) {
                h ^= (u >> (8 * k)) & 0xFFu;
                h *= 1099511628211ull;
            }
        }
    }
    return h;
}

/// Fail the case on any key the harness does not implement. Silently ignoring
/// an expectation is how a suite goes green over a missing feature, so every
/// object the manifest hands us is checked against the keys we act on.
void requireKnownKeys(const json& obj, const char* what,
                      std::initializer_list<const char*> known)
{
    for (const auto& [k, v] : obj.items()) {
        (void)v;
        bool ok = false;
        for (const char* n : known)
            if (k == n) { ok = true; break; }
        if (!ok) fail(std::string("unknown key in ") + what + ": " + k);
    }
}

/// The field subset a "fields"/"header" expectation may assert. Unknown keys
/// fail the case: a binding that cannot observe a field must not silently
/// skip the assertion.
void checkFields(const Reader& r, const json& fields)
{
    for (const auto& [k, v] : fields.items()) {
        if      (k == "channelCount")      checkU64(r.channelCount(), v.get<uint64_t>(), k.c_str());
        else if (k == "samplesPerBlock")   checkU64(r.samplesPerBlock(), v.get<uint64_t>(), k.c_str());
        else if (k == "ringBlocks")        checkU64(r.ringBlocks(), v.get<uint64_t>(), k.c_str());
        else if (k == "sampleRateMilliHz") checkU64(r.sampleRateMilliHz(), v.get<uint64_t>(), k.c_str());
        else if (k == "rateDivisor")       checkU64(r.rateDivisor(), v.get<uint64_t>(), k.c_str());
        else if (k == "layoutSeq")         checkU64(r.layoutSeq(), v.get<uint64_t>(), k.c_str());
        else if (k == "slotPayloadOffset") checkU64(r.header().slotPayloadOffset, v.get<uint64_t>(), k.c_str());
        else if (k == "slotBytes")         checkU64(r.header().slotBytes, v.get<uint64_t>(), k.c_str());
        else if (k == "channelPlaneBytes") checkU64(r.header().channelPlaneBytes, v.get<uint64_t>(), k.c_str());
        else if (k == "writeSeq")          checkU64(r.writeSeq(), v.get<uint64_t>(), k.c_str());
        else if (k == "deviceName")        check(r.deviceName() == v.get<std::string>(),
                                                 "deviceName mismatch");
        else if (k == "sampleFormat") {
            const char* got = r.sampleFormat() == SampleFormat::Float32 ? "Float32"
                              : r.sampleFormat() == SampleFormat::Int16 ? "Int16" : "?";
            check(v.get<std::string>() == got, "sampleFormat mismatch");
        } else {
            fail("unknown field in expectation: " + k);
        }
    }
}

void checkBlock(const BlockView& b, const json& e)
{
    requireKnownKeys(e, "block expectation",
                     {"seq", "firstSampleIndex", "timestampQpc", "flags", "silentChannelMask",
                      "sampleCount", "channelCount", "samples"});

    if (e.contains("seq"))               checkU64(b.seq, e["seq"].get<uint64_t>(), "block.seq");
    if (e.contains("firstSampleIndex"))  checkU64(b.firstSampleIndex, e["firstSampleIndex"].get<uint64_t>(), "block.firstSampleIndex");
    if (e.contains("timestampQpc"))      checkU64(b.timestampQpc, e["timestampQpc"].get<uint64_t>(), "block.timestampQpc");
    if (e.contains("flags"))             checkU64(b.flags, e["flags"].get<uint64_t>(), "block.flags");
    if (e.contains("silentChannelMask")) checkU64(b.silentChannelMask, e["silentChannelMask"].get<uint64_t>(), "block.silentChannelMask");
    if (e.contains("sampleCount"))       checkU64(b.sampleCount, e["sampleCount"].get<uint64_t>(), "block.sampleCount");
    if (e.contains("channelCount"))      checkU64(b.channelCount(), e["channelCount"].get<uint64_t>(), "block.channelCount");

    if (!e.contains("samples")) return;
    const json& samples = e["samples"];
    requireKnownKeys(samples, "samples expectation", {"tolerance", "hash", "spot"});
    const double tol = samples.value("tolerance", 0.0);

    if (samples.contains("hash")) {
        const uint64_t want = std::strtoull(samples["hash"].get<std::string>().c_str(), nullptr, 16);
        const uint64_t got  = hashSamples(b);
        ++g_checks;
        if (got != want) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "samples hash: got 0x%016llx, want %s",
                          static_cast<unsigned long long>(got),
                          samples["hash"].get<std::string>().c_str());
            fail(buf);
        }
    }

    if (samples.contains("spot")) {
        for (const json& s : samples["spot"]) {
            const uint32_t ch   = s[0].get<uint32_t>();
            const uint32_t i    = s[1].get<uint32_t>();
            const double   want = s[2].get<double>();
            ++g_checks;
            const float* plane = b.channel(ch);
            if (!plane || i >= b.sampleCount) { fail("spot index out of range"); continue; }
            const double got = plane[i];
            const double d   = got > want ? got - want : want - got;
            if (d > tol) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "spot[ch=%u i=%u]: got %.9g, want %.9g (tol %.3g)",
                              ch, i, got, want, tol);
                fail(buf);
            }
        }
    }
}

std::vector<uint8_t> readBlob(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        fail("cannot open blob " + path.string());
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(f.tellg()));
    // An empty or truncated blob must be a failure, never a skip: a case that
    // executes no steps would otherwise be reported as passing.
    if (bytes.empty()) {
        fail("blob is empty: " + path.string());
        return {};
    }
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<size_t>(f.gcount()) != bytes.size()) {
        fail("short read on blob " + path.string());
        return {};
    }
    return bytes;
}

void runCase(const json& c, const fs::path& base)
{
    g_caseId = c["id"].get<std::string>();
    std::vector<uint8_t> buf = readBlob(base / c["blob"].get<std::string>());
    if (buf.empty()) return;

    Reader    reader;
    BlockView block;

    ShmStreamHeader marked{};        // set by the markHeader op
    bool            markedIsSet = false;

    for (const json& step : c["steps"]) {
        const std::string op = step["op"].get<std::string>();

        if (op == "attach") {
            StreamRequest req;
            const StreamRequest* preq = nullptr;
            if (step.contains("request")) {
                const json& rj = step["request"];
                requireKnownKeys(rj, "attach request",
                                 {"channelCount", "sampleRateMilliHz", "samplesPerBlock",
                                  "sampleFormat"});
                req.channelCount      = rj.value("channelCount", 0u);
                req.sampleRateMilliHz = rj.value("sampleRateMilliHz", 0u);
                req.samplesPerBlock   = rj.value("samplesPerBlock", 0u);
                if (rj.contains("sampleFormat")) {
                    const std::string f = rj["sampleFormat"];
                    req.sampleFormat    = f == "Float32" ? SampleFormat::Float32
                                        : f == "Int16"   ? SampleFormat::Int16
                                                         : static_cast<SampleFormat>(0);
                }
                preq = &req;
            }
            const AttachResult got = reader.attach(buf.data(), buf.size(), preq);

            const json& expect = step["expect"];
            requireKnownKeys(expect, "attach expectation",
                             {"result", "retry", "fields", "mismatchDetailContains"});
            AttachResult want;
            if (!attachResultFromString(expect["result"].get<std::string>(), want)) {
                fail("unknown AttachResult in manifest");
                continue;
            }
            ++g_checks;
            if (got != want) {
                fail(std::string("attach: got ") + toCString(got) + ", want " + toCString(want));
                continue;
            }
            if (expect.contains("retry"))
                check(shouldRetry(got) == expect["retry"].get<bool>(),
                      "attach retryable flag mismatch");
            if (expect.contains("mismatchDetailContains"))
                check(std::string(reader.mismatchDetail())
                              .find(expect["mismatchDetailContains"].get<std::string>()) !=
                          std::string::npos,
                      "mismatchDetail missing expected field name");
            if (expect.contains("fields")) checkFields(reader, expect["fields"]);
        } else if (op == "patch") {
            // The producer advanced the segment: the blob replaces the whole
            // buffer. Sizes must match - a patch is a rewrite, not a remap.
            std::vector<uint8_t> next = readBlob(base / step["blob"].get<std::string>());
            check(next.size() == buf.size(), "patch blob size differs from the case blob");
            if (next.size() == buf.size()) std::memcpy(buf.data(), next.data(), buf.size());
        } else if (op == "rewindToOldest") {
            reader.rewindToOldest();
        } else if (op == "next" || op == "latest") {
            const bool got = op == "next" ? reader.next(block) : reader.latest(block);
            if (step.contains("expect")) {
                check(step["expect"] == "end" && !got, op + ": expected end of data, got a block");
            } else {
                check(got, op + ": expected a block, got end of data");
                if (got) checkBlock(block, step["block"]);
            }
        } else if (op == "droppedBlocks") {
            checkU64(reader.droppedBlocks(), step["expect"].get<uint64_t>(), "droppedBlocks");
        } else if (op == "writeSeq") {
            checkU64(reader.writeSeq(), step["expect"].get<uint64_t>(), "writeSeq");
        } else if (op == "attached") {
            check(reader.attached() == step["expect"].get<bool>(), "attached flag mismatch");
        } else if (op == "refreshSnapshot") {
            check(reader.refreshSnapshot() == step["expect"].get<bool>(),
                  "refreshSnapshot result mismatch");
        } else if (op == "reconfigureKind") {
            check(reconfigureKindName(reader.lastReconfigureKind()) ==
                      step["expect"].get<std::string>(),
                  "reconfigureKind mismatch");
        } else if (op == "markHeader") {
            // The watermark a consumer keeps so it can classify a reconfigure
            // no matter who refreshed the snapshot in between.
            marked       = reader.header();
            markedIsSet  = true;
        } else if (op == "reconfigureSince") {
            if (!markedIsSet) {
                fail("reconfigureSince without a preceding markHeader");
            } else {
                check(reconfigureKindName(
                          Reader::classifyReconfigure(marked, reader.header())) ==
                          step["expect"].get<std::string>(),
                      "reconfigureSince mismatch");
            }
        } else if (op == "header") {
            checkFields(reader, step["expect"]);
        } else if (op == "state") {
            const uint64_t nowQpc = step["nowQpc"].get<uint64_t>();
            check(streamStateName(reader.state(nowQpc)) == step["expect"].get<std::string>(),
                  "stream state mismatch");
        } else {
            fail("unknown op: " + op);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: conformance_runner <path/to/manifest.json>\n");
        return 2;
    }

    const fs::path manifestPath = argv[1];
    std::ifstream f(manifestPath);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", manifestPath.string().c_str());
        return 2;
    }

    json manifest;
    try {
        manifest = json::parse(f);
    } catch (const json::exception& e) {
        std::fprintf(stderr, "manifest JSON error: %s\n", e.what());
        return 2;
    }

    // Refuse a manifest schema this harness does not implement, rather than
    // running it with whatever ops happen to still be recognised. A newer
    // schema may have changed op semantics under names that still parse.
    constexpr int kSupportedSchema = 2;
    const int     schema           = manifest.value("schema", 0);
    if (schema != kSupportedSchema) {
        std::fprintf(stderr, "manifest schema %d is not supported (this runner implements %d)\n",
                     schema, kSupportedSchema);
        return 2;
    }

    const fs::path base = manifestPath.parent_path();
    const json&    cases = manifest["cases"];

    int passed = 0;
    for (const json& c : cases) {
        const int before = g_failures;
        runCase(c, base);
        if (g_failures == before) {
            ++passed;
            std::printf("  ok   %s\n", c["id"].get<std::string>().c_str());
        }
    }

    std::printf("%s: %d/%zu cases, %d checks, %d failures\n",
                passed == static_cast<int>(cases.size()) ? "PASS" : "FAIL", passed, cases.size(),
                g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
