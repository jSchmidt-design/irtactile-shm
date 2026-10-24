// tools/mkvectors/mkvectors.cpp - emits the stream-shm v1 conformance vectors:
// golden binary segments (decode/*.bin) plus manifest.json describing what a
// correct binding must do with them. See conformance/README.md.
//
// SPDX-License-Identifier: MIT
//
// Producer-side only. Segments are built with layout.h's arithmetic, and the
// expected values in the manifest are computed from what was written - never
// by running a reader - so a binding bug cannot bless itself.
//
// Usage:
//   mkvectors <output-dir>          (e.g. conformance/stream-shm/v1)
//
// Writes <output-dir>/decode/*.bin and <output-dir>/manifest.json. Both are
// committed to the repo; regenerating and diffing is the drift check.

#include <irtactile/shm/stream/layout.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace irtactile::shm::stream;
using nlohmann::ordered_json;

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Segment construction - producer side, layout.h arithmetic only
// ---------------------------------------------------------------------------

struct Sec {
    std::vector<uint8_t> bytes;
    ShmStreamHeader& h() { return *reinterpret_cast<ShmStreamHeader*>(bytes.data()); }
};

/// A valid v1.0 segment for `p`, magic written, nothing published.
Sec makeSegment(const LayoutParams& p)
{
    Sec s;
    s.bytes.assign(static_cast<size_t>(computeTotalSize(p)), 0);
    ShmStreamHeader& h  = s.h();
    h.magic             = kMagic;
    h.versionMajor      = kVersionMajor;
    h.versionMinor      = kVersionMinor;
    h.headerBytes       = static_cast<uint32_t>(sizeof(ShmStreamHeader));
    h.totalBytes        = static_cast<uint32_t>(s.bytes.size());
    h.sampleFormat      = static_cast<uint32_t>(p.sampleFormat);
    h.channelLayout     = static_cast<uint32_t>(ChannelLayout::Planar);
    h.channelCount      = p.channelCount;
    h.sampleRateMilliHz = 3'000'000;   // 3000 Hz: 48000 / divisor 16
    h.sourceSampleRate  = 48000;
    h.rateDivisor       = 16;
    h.samplesPerBlock   = p.samplesPerBlock;
    h.ringBlocks        = p.ringBlocks;
    h.bytesPerSample    = bytesPerSampleOf(p.sampleFormat);
    h.slotPayloadOffset = slotPayloadOffsetOf();
    h.channelArrayOffset = 0;
    h.slotArrayOffset   = slotArrayOffsetOf();
    h.slotBytes         = slotBytesOf(p);
    h.channelPlaneBytes = channelPlaneBytesOf(p);
    h.diagEngineBlockSize  = 128;
    h.diagFramesPerPublish = 6;
    h.qpcFrequency      = 10'000'000;
    h.wiredChannelMask  = p.channelCount >= 32 ? 0xFFFFFFFFu : ((1u << p.channelCount) - 1u);
    h.deviceState       = static_cast<uint32_t>(DeviceState::Active);
    h.producerPid       = 0;
    auto setStr = [](char* dst, size_t cap, const char* src) {
        const size_t n = std::strlen(src);
        std::memcpy(dst, src, n < cap ? n + 1 : cap);
    };
    setStr(h.deviceGuid, sizeof(h.deviceGuid), "{00000000-0000-0000-0000-000000000001}");
    setStr(h.deviceName, sizeof(h.deviceName), "irTactile Vector Device");
    return s;
}

/// Publishes one block following the protocol (kSlotBusy -> payload ->
/// descriptor fields -> seq, then writeSeq/lastPublishQpc) and returns the
/// samples a correct decoder must produce, planar channel-major.
///
/// Float32 segments carry the canonical pattern sample(ch,i) = fsi + i +
/// ch*1000 (exactly representable). Int16 segments take explicit raw values
/// via `rawInt16` (channelCount x samplesPerBlock, planar).
std::vector<float> publishBlock(Sec& s, uint64_t seq, uint64_t fsi, uint32_t flags,
                                const std::vector<int16_t>* rawInt16 = nullptr)
{
    ShmStreamHeader& h    = s.h();
    uint8_t*         slot = slotAt(s.bytes.data(), h, seq);
    auto*            desc = reinterpret_cast<ShmBlockDesc*>(slot);

    desc->seq = kSlotBusy;

    std::vector<float> decoded(static_cast<size_t>(h.channelCount) * h.samplesPerBlock);
    for (uint32_t ch = 0; ch < h.channelCount; ++ch) {
        uint8_t* plane = planeAt(slot, h, ch);
        for (uint32_t i = 0; i < h.samplesPerBlock; ++i) {
            const size_t k = static_cast<size_t>(ch) * h.samplesPerBlock + i;
            if (rawInt16) {
                const int16_t v = (*rawInt16)[k];
                std::memcpy(plane + static_cast<size_t>(i) * 2, &v, 2);
                decoded[k] = int16ToFloat(v);
            } else {
                const float v = static_cast<float>(fsi + i) + static_cast<float>(ch) * 1000.0f;
                std::memcpy(plane + static_cast<size_t>(i) * 4, &v, 4);
                decoded[k] = v;
            }
        }
    }

    desc->firstSampleIndex  = fsi;
    desc->timestampQpc      = (seq + 1) * 1000;
    desc->flags             = flags;
    desc->silentChannelMask = 0;
    desc->layoutEpoch       = h.layoutSeq;
    std::memset(desc->reserved, 0, sizeof(desc->reserved));
    desc->seq               = seq;

    h.lastPublishQpc = desc->timestampQpc;
    h.writeSeq       = seq + 1;
    return decoded;
}

// ---------------------------------------------------------------------------
// Expected-sample hashing - the manifest's compact full-payload check.
// FNV-1a 64 over the little-endian IEEE-754 bits of the decoded float32
// samples, planar channel-major. Documented in conformance/README.md.
// ---------------------------------------------------------------------------

uint64_t fnv1a64(const std::vector<float>& samples)
{
    uint64_t h = 14695981039346656037ull;
    for (const float f : samples) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        for (int k = 0; k < 4; ++k) {
            h ^= (u >> (8 * k)) & 0xFFu;
            h *= 1099511628211ull;
        }
    }
    return h;
}

std::string hashString(const std::vector<float>& samples)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%016llx",
                  static_cast<unsigned long long>(fnv1a64(samples)));
    return buf;
}

// ---------------------------------------------------------------------------
// Case/file plumbing
// ---------------------------------------------------------------------------

void writeFile(const fs::path& path, const void* data, size_t size)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!f) {
        std::fprintf(stderr, "mkvectors: cannot write %s\n", path.string().c_str());
        std::exit(1);
    }
}

/// One expected block. Spots are (channel, index) pairs checked against
/// `tolerance`; the hash covers every sample bit-exactly.
ordered_json blockExpect(uint64_t seq, uint64_t fsi, uint32_t flags,
                         uint32_t channelCount, uint32_t sampleCount,
                         const std::vector<float>&                           decoded,
                         const std::vector<std::pair<uint32_t, uint32_t>>&   spots,
                         double tolerance, bool includeHash)
{
    ordered_json samples;
    samples["tolerance"] = tolerance;
    if (includeHash) samples["hash"] = hashString(decoded);
    samples["spot"] = ordered_json::array();
    for (const auto& [ch, i] : spots)
        samples["spot"].push_back(
            {ch, i, static_cast<double>(decoded[static_cast<size_t>(ch) * sampleCount + i])});

    ordered_json j;
    j["seq"]               = seq;
    j["firstSampleIndex"]  = fsi;
    j["timestampQpc"]      = (seq + 1) * 1000;
    j["flags"]             = flags;
    j["silentChannelMask"] = 0;
    j["channelCount"]      = channelCount;
    j["sampleCount"]       = sampleCount;
    j["samples"]           = std::move(samples);
    return j;
}

/// The default spot set: both ends of channel 0, plus two points on the
/// outer channels so a planar/interleaved mix-up cannot pass.
std::vector<std::pair<uint32_t, uint32_t>> defaultSpots(uint32_t channelCount, uint32_t sampleCount)
{
    return {{0, 0}, {0, sampleCount - 1}, {1, 0}, {channelCount - 1, sampleCount - 1}};
}

ordered_json expectResult(const char* result, bool retry, ordered_json extra = ordered_json::object())
{
    ordered_json e = {{"result", result}, {"retry", retry}};
    e.update(extra);
    return e;
}

ordered_json attachStep(ordered_json expect, ordered_json request = nullptr)
{
    ordered_json s;
    s["op"] = "attach";
    if (!request.is_null()) s["request"] = std::move(request);
    s["expect"] = std::move(expect);
    return s;
}

/// Resize a segment and keep totalBytes honest, so a vector that grows the
/// slot array does not accidentally also break `totalBytes <= mappedBytes`.
void setSize(Sec& s, size_t n)
{
    s.bytes.resize(n, 0);
    s.h().totalBytes = static_cast<uint32_t>(n);
}

struct Emitter {
    fs::path                 outDir;
    std::vector<ordered_json> cases;

    /// Writes `sec` as decode/<name>.bin and returns the manifest-relative path.
    std::string blob(const std::string& name, const Sec& sec)
    {
        writeFile(outDir / "decode" / (name + ".bin"), sec.bytes.data(), sec.bytes.size());
        return "decode/" + name + ".bin";
    }

    std::string blobBytes(const std::string& name, const std::vector<uint8_t>& bytes)
    {
        writeFile(outDir / "decode" / (name + ".bin"), bytes.data(), bytes.size());
        return "decode/" + name + ".bin";
    }

    void addCase(const std::string& id, const std::string& summary,
                 const std::string& blobPath, const std::vector<ordered_json>& steps)
    {
        ordered_json c;
        c["id"]      = id;
        c["summary"] = summary;
        c["blob"]    = blobPath;
        c["steps"]   = steps;
        cases.push_back(std::move(c));
    }
};

/// A one-rule-invalid attach vector: take a valid segment, break exactly one
/// layout-validation rule, expect Malformed. The blob file name is the case id
/// with '.' -> '_'.
///
/// Each mutation is deliberately isolated - it grows the segment rather than
/// letting a second rule fire as collateral - because the point of these
/// vectors is to catch a binding that implements *some* of the validation.
/// A case violating two rules still passes on a binding that checks only one.
template <class Mutate>
void addMalformed(Emitter& em, const LayoutParams& p, const std::string& id,
                  const std::string& summary, Mutate mutate)
{
    Sec s = makeSegment(p);
    mutate(s);
    std::string file = id;
    for (char& c : file)
        if (c == '.') c = '_';
    em.addCase(id, summary, em.blob(file, s),
               {attachStep(expectResult("Malformed", false))});
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: mkvectors <output-dir>\n");
        return 2;
    }

    Emitter em;
    em.outDir = argv[1];
    fs::create_directories(em.outDir / "decode");

    // Geometries used below. All tiny on purpose: a vector blob is exactly
    // totalBytes, and small segments keep the committed suite readable.
    const LayoutParams g4{4, 8, 2, SampleFormat::Float32};   // 640 B
    const LayoutParams g2{2, 4, 2, SampleFormat::Float32};   // 512 B

    const uint32_t kDiscontinuity = static_cast<uint32_t>(BlockDiscontinuity);

    // =======================================================================
    // Attach vectors - one blob, one attach step.
    // =======================================================================

    // --- zero-filled: header not written yet -------------------------------
    {
        const std::string b = em.blobBytes("attach_zero_filled", std::vector<uint8_t>(4096, 0));
        em.addCase("attach.zero_filled",
                   "zero-filled segment is NotReady and retryable",
                   b, {attachStep(expectResult("NotReady", true))});
    }

    // --- wrong magic --------------------------------------------------------
    {
        std::vector<uint8_t> bytes(4096, 0);
        auto* h = reinterpret_cast<ShmStreamHeader*>(bytes.data());
        h->magic = 0xDEADBEEFu;
        const std::string b = em.blobBytes("attach_bad_magic", bytes);
        em.addCase("attach.bad_magic",
                   "wrong magic is Malformed and fatal (not retryable)",
                   b, {attachStep(expectResult("Malformed", false))});
    }

    // --- higher versionMajor ------------------------------------------------
    {
        Sec s = makeSegment(g4);
        s.h().versionMajor = kVersionMajor + 1;
        em.addCase("attach.higher_version_major",
                   "a versionMajor the binding does not know is BadVersion",
                   em.blob("attach_higher_version_major", s),
                   {attachStep(expectResult("BadVersion", false))});
    }

    // --- higher versionMinor: tolerated -------------------------------------
    {
        Sec s = makeSegment(g4);
        s.h().versionMinor = kVersionMinor + 5;
        em.addCase("attach.higher_version_minor",
                   "a higher versionMinor is tolerated (additives live behind runtime offsets)",
                   em.blob("attach_higher_version_minor", s),
                   {attachStep(expectResult("Ok", false,
                        {{"fields",
                          {{"channelCount", 4},
                           {"samplesPerBlock", 8},
                           {"ringBlocks", 2},
                           {"sampleRateMilliHz", 3000000},
                           {"sampleFormat", "Float32"}}}}))});
    }

    // --- unknown enum values are rejected, never defaulted ------------------
    {
        Sec s = makeSegment(g4);
        s.h().sampleFormat = 99;
        em.addCase("attach.unknown_sample_format",
                   "unknown sampleFormat is rejected (BadVersion), not defaulted",
                   em.blob("attach_unknown_sample_format", s),
                   {attachStep(expectResult("BadVersion", false))});
    }
    {
        Sec s = makeSegment(g4);
        s.h().channelLayout = 99;
        em.addCase("attach.unknown_channel_layout",
                   "unknown channelLayout is rejected (BadVersion), not defaulted",
                   em.blob("attach_unknown_channel_layout", s),
                   {attachStep(expectResult("BadVersion", false))});
    }
    {
        Sec s = makeSegment(g4);
        s.h().channelLayout = static_cast<uint32_t>(ChannelLayout::Interleaved);  // 2: known, reserved
        em.addCase("attach.reserved_channel_layout",
                   "channelLayout=2 (Interleaved) is reserved: no v1 producer emits it; reject",
                   em.blob("attach_reserved_channel_layout", s),
                   {attachStep(expectResult("BadVersion", false))});
    }

    // --- an offset past mappedBytes ------------------------------------------
    {
        Sec s = makeSegment(g4);
        s.bytes.resize(s.bytes.size() - 1);   // claims totalBytes, delivers fewer
        em.addCase("attach.offset_past_mapped",
                   "a segment smaller than its header's totalBytes is Malformed",
                   em.blob("attach_offset_past_mapped", s),
                   {attachStep(expectResult("Malformed", false))});
    }

    // --- hostile samplesPerBlock: the v1 form of the descriptor-count clamp --
    {
        Sec s = makeSegment(g2);
        s.h().samplesPerBlock = 4096;   // planes still say 4 samples
        em.addCase("attach.hostile_samples_per_block",
                   "samplesPerBlock overrunning the channel plane is Malformed - the copy bound "
                   "is validated once at attach, not clamped per block",
                   em.blob("attach_hostile_samples_per_block", s),
                   {attachStep(expectResult("Malformed", false))});
    }

    // --- the rest of the layout-validation rules -----------------------------
    //
    // A wrong offset is the dangerous class of error in a hand-written binding:
    // no exception, no crash, plausible-looking float garbage. Each of
    // these breaks exactly one rule - the first below is the sole exception,
    // and says why - so a binding that implements only a subset of the
    // validation fails the specific case it is missing rather than riding on a
    // neighbouring check.

    // The one vector that cannot be isolated: a reader clamps its snapshot copy
    // to headerBytes, so a headerBytes of 32 also zeroes every field past byte
    // 32. Both readings reject - a clamping binding on the zeroed fields, a
    // binding that always copies 256 bytes on the headerBytes rule itself.
    addMalformed(em, g4, "attach.header_bytes_below_minimum",
                 "headerBytes smaller than the v1 minimum (64) is Malformed: the header cannot "
                 "contain the fields the protocol needs",
                 [](Sec& s) { s.h().headerBytes = 32; });

    addMalformed(em, g4, "attach.channel_count_out_of_range",
                 "channelCount above the normative maximum (32) is Malformed, not clamped",
                 [](Sec& s) {
                     // Grow the slots so the planes still fit: the only rule
                     // broken is the channel-count limit itself.
                     s.h().channelCount = 33;
                     s.h().slotBytes    = align64(slotPayloadOffsetOf() + 33 * 32);
                     setSize(s, slotArrayOffsetOf() + size_t{2} * s.h().slotBytes);
                 });

    addMalformed(em, g4, "attach.ring_blocks_below_minimum",
                 "ringBlocks below the normative minimum (2) is Malformed: ring 1 offers the "
                 "reader no protection at all",
                 [](Sec& s) { s.h().ringBlocks = 1; });

    addMalformed(em, g4, "attach.zero_samples_per_block",
                 "samplesPerBlock == 0 is Malformed: the publish period is undefined",
                 [](Sec& s) { s.h().samplesPerBlock = 0; });

    addMalformed(em, g4, "attach.zero_sample_rate",
                 "sampleRateMilliHz == 0 is Malformed: period, wait timeout and staleness are "
                 "all derived from it",
                 [](Sec& s) { s.h().sampleRateMilliHz = 0; });

    addMalformed(em, g4, "attach.rate_divisor_out_of_range",
                 "rateDivisor outside 1..1024 is Malformed",
                 [](Sec& s) { s.h().rateDivisor = 2048; });

    addMalformed(em, g4, "attach.bytes_per_sample_mismatch",
                 "bytesPerSample disagreeing with sampleFormat is Malformed - a binding must not "
                 "pick one and ignore the other",
                 [](Sec& s) { s.h().bytesPerSample = 2; });   // segment is Float32

    addMalformed(em, g4, "attach.payload_overlaps_descriptor",
                 "slotPayloadOffset inside the descriptor (< 64) is Malformed: plane 0 would "
                 "alias the block descriptor",
                 [](Sec& s) { s.h().slotPayloadOffset = 32; });

    // The plane-alignment guarantee is what lets a binding build a typed view
    // over mapped memory - in Rust a *safety* requirement, not an optimisation.
    // All four terms of the plane address carry it, so all four get a vector.

    addMalformed(em, g4, "attach.unaligned_slot_array_offset",
                 "slotArrayOffset not a multiple of bytesPerSample is Malformed: every plane in "
                 "the segment would be misaligned",
                 [](Sec& s) {
                     setSize(s, 704);
                     s.h().slotArrayOffset = 258;
                 });

    addMalformed(em, g4, "attach.unaligned_slot_bytes",
                 "slotBytes not a multiple of bytesPerSample is Malformed: alignment would "
                 "alternate from slot to slot",
                 [](Sec& s) {
                     setSize(s, 644);
                     s.h().slotBytes = 194;
                 });

    addMalformed(em, g4, "attach.unaligned_payload_offset",
                 "slotPayloadOffset not a multiple of bytesPerSample is Malformed",
                 [](Sec& s) {
                     setSize(s, 768);
                     s.h().slotPayloadOffset = 66;
                     s.h().slotBytes         = 256;
                 });

    addMalformed(em, g4, "attach.unaligned_channel_plane",
                 "channelPlaneBytes not a multiple of bytesPerSample is Malformed: alignment "
                 "would alternate from channel to channel",
                 [](Sec& s) {
                     setSize(s, 768);
                     s.h().channelPlaneBytes = 34;
                     s.h().slotBytes         = 256;
                 });

    addMalformed(em, g4, "attach.slot_bytes_below_planes",
                 "slotBytes smaller than slotPayloadOffset + channelCount x channelPlaneBytes is "
                 "Malformed: the last plane would run into the next slot",
                 [](Sec& s) { s.h().slotBytes = 128; });   // needs 192

    addMalformed(em, g4, "attach.slot_array_inside_header",
                 "slotArrayOffset below headerBytes is Malformed: slot 0 would overlap the header",
                 [](Sec& s) { s.h().slotArrayOffset = 192; });

    addMalformed(em, g4, "attach.slot_array_overruns_section",
                 "the composite bound, not just the parts: slotArrayOffset, ringBlocks and "
                 "slotBytes are each in range while slotArrayOffset + ringBlocks x slotBytes "
                 "runs past the mapping",
                 [](Sec& s) { s.h().ringBlocks = 4; });   // 256 + 4x192 = 1024 > 640

    // --- a segment caught mid-rewrite ----------------------------------------
    {
        // The seqlock's whole point: layoutSeq odd means the producer is
        // partway through a header rewrite, and the fields do not agree with
        // each other yet. Here samplesPerBlock has already moved to 16 while
        // channelCount and channelPlaneBytes still describe the old layout.
        //
        // A binding that ignores the odd bit reads that torn state and returns
        // Malformed (the plane no longer holds samplesPerBlock samples) - a
        // fatal answer to a transient condition. The correct answer is
        // NotReady: wait, and the completed rewrite attaches cleanly.
        Sec torn = makeSegment(g4);
        torn.h().layoutSeq       = 1;
        torn.h().samplesPerBlock = 16;

        Sec done = makeSegment(g4);
        done.h().layoutSeq         = 2;
        done.h().channelCount      = 2;
        done.h().samplesPerBlock   = 16;
        done.h().channelPlaneBytes = 64;
        done.h().slotBytes         = align64(slotPayloadOffsetOf() + 2 * 64);

        em.addCase("attach.rewrite_in_progress",
                   "a segment whose layoutSeq is odd is NotReady and retryable, never read torn; "
                   "the completed rewrite then attaches with the new layout",
                   em.blob("attach_rewrite_in_progress_torn", torn),
                   {attachStep(expectResult("NotReady", true)),
                    {{"op", "patch"}, {"blob", em.blob("attach_rewrite_in_progress_done", done)}},
                    attachStep(expectResult("Ok", false,
                        {{"fields",
                          {{"channelCount", 2},
                           {"samplesPerBlock", 16},
                           {"layoutSeq", 2},
                           {"channelPlaneBytes", 64},
                           {"slotBytes", done.h().slotBytes}}}}))});
    }

    // --- non-zero reserved[] bytes are tolerated -----------------------------
    {
        Sec s = makeSegment(g4);
        std::memset(s.h().reserved, 0xAB, sizeof(s.h().reserved));
        s.h().reserved1 = 0xCDCDCDCDu;
        em.addCase("attach.nonzero_reserved_tolerated",
                   "non-zero reserved bytes are ignored, not rejected",
                   em.blob("attach_nonzero_reserved_tolerated", s),
                   {attachStep(expectResult("Ok", false))});
    }

    // --- a grown v1.1 header is tolerated -------------------------------------
    {
        // A hypothetical v1.1 producer whose header grew by 64 bytes:
        // headerBytes and slotArrayOffset move out together. The reader must
        // clamp its snapshot copy to its own struct size and follow the
        // runtime slotArrayOffset, never assume it.
        const uint32_t grownHeader = static_cast<uint32_t>(sizeof(ShmStreamHeader)) + 64;
        Sec s = makeSegment(g2);
        Sec t;
        t.bytes.assign(static_cast<size_t>(align64(grownHeader)) +
                           static_cast<size_t>(g2.ringBlocks) * slotBytesOf(g2),
                       0);
        std::memcpy(t.bytes.data(), s.bytes.data(), sizeof(ShmStreamHeader));
        t.h().headerBytes     = grownHeader;   // > sizeof(ShmStreamHeader)
        t.h().totalBytes      = static_cast<uint32_t>(t.bytes.size());
        t.h().slotArrayOffset = align64(grownHeader);
        em.addCase("attach.grown_header_tolerated",
                   "a larger future header (headerBytes > 256) is tolerated: clamp the snapshot, "
                   "follow slotArrayOffset",
                   em.blob("attach_grown_header_tolerated", t),
                   {attachStep(expectResult("Ok", false, {{"fields", {{"slotPayloadOffset", 64}}}}))});
    }

    // --- StreamRequest mismatch ----------------------------------------------
    {
        Sec s = makeSegment(g4);
        em.addCase("attach.request_mismatch",
                   "a StreamRequest field the segment cannot satisfy is Mismatch, not retryable",
                   em.blob("attach_request_mismatch", s),
                   {attachStep(expectResult("Mismatch", false,
                                            {{"mismatchDetailContains", "channelCount"}}),
                               {{"channelCount", 6}})});
    }

    // =======================================================================
    // Read vectors - attach, then scripted reads against golden blocks.
    // =======================================================================

    // --- the happy path: five blocks, sample-exact ---------------------------
    {
        const LayoutParams g8{4, 8, 8, SampleFormat::Float32};   // 1792 B
        Sec s = makeSegment(g8);
        std::vector<ordered_json> steps;
        steps.push_back(attachStep(expectResult("Ok", false)));
        steps.push_back({{"op", "rewindToOldest"}});
        for (uint64_t seq = 0; seq < 5; ++seq) {
            const uint64_t fsi = seq * g8.samplesPerBlock;
            const uint32_t flags = seq == 0 ? kDiscontinuity : 0u;
            const std::vector<float> decoded = publishBlock(s, seq, fsi, flags);
            steps.push_back({{"op", "next"},
                             {"block", blockExpect(seq, fsi, flags, g8.channelCount,
                                                   g8.samplesPerBlock, decoded,
                                                   defaultSpots(g8.channelCount, g8.samplesPerBlock),
                                                   0.0, true)}});
        }
        steps.push_back({{"op", "next"}, {"expect", "end"}});
        steps.push_back({{"op", "droppedBlocks"}, {"expect", 0}});
        em.addCase("read.float32_blocks",
                   "five float32 blocks decode sample-exact, in order, with no drops",
                   em.blob("read_float32_blocks", s), steps);
    }

    // --- latest(): newest-wins, and skipping is not dropping -----------------
    {
        const LayoutParams g8{4, 8, 8, SampleFormat::Float32};   // 1792 B
        Sec a = makeSegment(g8);                                  // nothing published
        Sec b = a;
        std::vector<float> d4;
        for (uint64_t seq = 0; seq < 5; ++seq)
            d4 = publishBlock(b, seq, seq * g8.samplesPerBlock, seq == 0 ? kDiscontinuity : 0u);
        Sec c = b;
        std::vector<float> d7;
        for (uint64_t seq = 5; seq < 8; ++seq)
            d7 = publishBlock(c, seq, seq * g8.samplesPerBlock, 0u);

        // The distinction next() cannot make: latest() is lossy *by contract*,
        // so the blocks it steps over are not gaps and must not reach
        // droppedBlocks. A binding that implements latest() as "next() in a
        // loop" fails the counter assertion.
        em.addCase("read.latest_wins",
                   "latest() returns the newest complete block and skips the rest; skipped "
                   "blocks are not counted as drops, and a caught-up reader reports end",
                   em.blob("read_latest_a", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "latest"}, {"expect", "end"}},
                    {{"op", "patch"}, {"blob", em.blob("read_latest_b", b)}},
                    {{"op", "latest"},
                     {"block", blockExpect(4, 32, 0, g8.channelCount, g8.samplesPerBlock, d4,
                                           defaultSpots(g8.channelCount, g8.samplesPerBlock),
                                           0.0, true)}},
                    {{"op", "latest"}, {"expect", "end"}},
                    {{"op", "patch"}, {"blob", em.blob("read_latest_c", c)}},
                    {{"op", "latest"},
                     {"block", blockExpect(7, 56, 0, g8.channelCount, g8.samplesPerBlock, d7,
                                           defaultSpots(g8.channelCount, g8.samplesPerBlock),
                                           0.0, true)}},
                    {{"op", "next"}, {"expect", "end"}},
                    {{"op", "droppedBlocks"}, {"expect", 0}}});
    }

    // --- attach cursor starts at writeSeq, not zero --------------------------
    {
        Sec s = makeSegment(g4);
        for (uint64_t seq = 0; seq < 10; ++seq)
            publishBlock(s, seq, seq * g4.samplesPerBlock, seq == 0 ? kDiscontinuity : 0u);
        // "Producer has been running for hours": attach must read writeSeq,
        // not assume 0 - otherwise the first next() reports a phantom gap.
        s.h().writeSeq = 10'000'000;
        em.addCase("read.cursor_starts_at_write_seq",
                   "attach seeds the cursor at writeSeq: no phantom gap on the first read",
                   em.blob("read_cursor_starts_at_write_seq", s),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "writeSeq"}, {"expect", 10000000}},
                    {{"op", "next"}, {"expect", "end"}},
                    {{"op", "droppedBlocks"}, {"expect", 0}}});
    }

    // --- overrun boundary at ringBlocks=2 ------------------------------------
    {
        Sec a = makeSegment(g4);   // nothing published
        Sec b = makeSegment(g4);
        const std::vector<float> decoded = publishBlock(b, 0, 0, kDiscontinuity);

        em.addCase("read.overrun_ring2_one_past_cursor",
                   "ring 2, one block past the cursor: it delivers",
                   em.blob("read_overrun_ring2_one_past_a", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "patch"}, {"blob", em.blob("read_overrun_ring2_one_past_b", b)}},
                    {{"op", "next"},
                     {"block", blockExpect(0, 0, kDiscontinuity, g4.channelCount, g4.samplesPerBlock,
                                           decoded, defaultSpots(g4.channelCount, g4.samplesPerBlock),
                                           0.0, true)}},
                    {{"op", "next"}, {"expect", "end"}},
                    {{"op", "droppedBlocks"}, {"expect", 0}}});
    }
    {
        Sec a = makeSegment(g4);
        Sec b = makeSegment(g4);
        publishBlock(b, 0, 0, kDiscontinuity);
        const std::vector<float> decoded = publishBlock(b, 1, 8, 0);

        // Window is ringBlocks-1 = 1: block 0 is already gone; block 1 delivers.
        em.addCase("read.overrun_ring2_two_past_cursor",
                   "ring 2, two blocks past the cursor: exactly the newest survives; the gap is counted",
                   em.blob("read_overrun_ring2_two_past_a", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "patch"}, {"blob", em.blob("read_overrun_ring2_two_past_b", b)}},
                    {{"op", "next"},
                     {"block", blockExpect(1, 8, 0, g4.channelCount, g4.samplesPerBlock, decoded,
                                           defaultSpots(g4.channelCount, g4.samplesPerBlock),
                                           0.0, true)}},
                    {{"op", "next"}, {"expect", "end"}},
                    {{"op", "droppedBlocks"}, {"expect", 1}}});
    }

    // --- a busy slot is rejected outright --------------------------------------
    {
        Sec a = makeSegment(g4);
        publishBlock(a, 0, 0, kDiscontinuity);
        publishBlock(a, 1, 8, 0);

        Sec b = a;
        // Producer claims slot 0 for seq 2 but never finishes it, and writeSeq
        // moves past it (a stalled/mid-construction publish the reader must
        // not hand out as data).
        auto* desc     = reinterpret_cast<ShmBlockDesc*>(slotAt(b.bytes.data(), b.h(), 2));
        desc->seq      = kSlotBusy;
        b.h().writeSeq = 3;

        em.addCase("read.slot_busy_ring2",
                   "a slot still carrying kSlotBusy is rejected, not decoded; the loss is counted",
                   em.blob("read_slot_busy_ring2_a", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "next"}, {"expect", "end"}},   // cursor == writeSeq == 2
                    {{"op", "patch"}, {"blob", em.blob("read_slot_busy_ring2_b", b)}},
                    {{"op", "next"}, {"expect", "end"}},
                    {{"op", "droppedBlocks"}, {"expect", 1}}});
    }

    // --- abandoned block: firstSampleIndex advances by the discarded samples --
    {
        const LayoutParams g4r4{4, 8, 4, SampleFormat::Float32};   // ring 4: 1024 B
        Sec s = makeSegment(g4r4);
        const std::vector<float> d0 = publishBlock(s, 0, 0, kDiscontinuity);
        // A partial block (4 of 8 samples) is abandoned; the next published
        // block's firstSampleIndex has advanced by the discarded samples too.
        const std::vector<float> d1 = publishBlock(s, 1, 12, 0);

        em.addCase("read.abandoned_block",
                   "an abandoned partial block advances firstSampleIndex by the samples discarded "
                   "(gap = 12 - (0 + 8) = 4), not by zero",
                   em.blob("read_abandoned_block", s),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "rewindToOldest"}},
                    {{"op", "next"},
                     {"block", blockExpect(0, 0, kDiscontinuity, g4r4.channelCount,
                                           g4r4.samplesPerBlock, d0,
                                           defaultSpots(g4r4.channelCount, g4r4.samplesPerBlock),
                                           0.0, true)}},
                    {{"op", "next"},
                     {"block", blockExpect(1, 12, 0, g4r4.channelCount, g4r4.samplesPerBlock, d1,
                                           defaultSpots(g4r4.channelCount, g4r4.samplesPerBlock),
                                           0.0, true)}},
                    {{"op", "next"}, {"expect", "end"}},
                    {{"op", "droppedBlocks"}, {"expect", 0}}});
    }

    // --- layoutSeq seqlock: a rewrite is picked up -----------------------------
    {
        Sec a = makeSegment(g4);   // layoutSeq 0, 4 channels
        Sec b = a;
        ShmStreamHeader& h = b.h();
        // odd -> rewrite -> even; a geometry change done properly (slotBytes
        // recomputed, totalBytes pinned: computeTotalSize(new) <= totalBytes).
        h.layoutSeq    = 2;
        h.channelCount = 2;
        h.slotBytes    = align64(slotPayloadOffsetOf() + 2 * channelPlaneBytesOf(g4));

        em.addCase("read.seqlock_geometry",
                   "a layoutSeq rewrite (odd -> fields change -> even) is picked up on the next "
                   "refresh; an addressing change classifies as Geometry",
                   em.blob("read_seqlock_geometry_a", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "header"}, {"expect", {{"channelCount", 4}, {"layoutSeq", 0}}}},
                    {{"op", "patch"}, {"blob", em.blob("read_seqlock_geometry_b", b)}},
                    {{"op", "refreshSnapshot"}, {"expect", true}},
                    {{"op", "reconfigureKind"}, {"expect", "Geometry"}},
                    {{"op", "header"},
                     {"expect",
                      {{"channelCount", 2}, {"layoutSeq", 2}, {"slotBytes", h.slotBytes}}}}});
    }
    {
        Sec a = makeSegment(g4);
        Sec b = a;
        ShmStreamHeader& h     = b.h();
        h.layoutSeq            = 2;
        h.sourceSampleRate     = 96000;
        h.rateDivisor          = 8;
        h.diagEngineBlockSize  = 64;
        h.diagFramesPerPublish = 3;

        em.addCase("read.seqlock_cosmetic",
                   "a diagnostic-only rewrite classifies as Cosmetic; geometry accessors are unchanged",
                   em.blob("read_seqlock_cosmetic_a", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "patch"}, {"blob", em.blob("read_seqlock_cosmetic_b", b)}},
                    {{"op", "refreshSnapshot"}, {"expect", true}},
                    {{"op", "reconfigureKind"}, {"expect", "Cosmetic"}},
                    {{"op", "header"}, {"expect", {{"channelCount", 4}, {"rateDivisor", 8}}}}});
    }

    // --- a rewrite that never completes: the retry is bounded --------------------
    {
        // The other half of the seqlock, on an already-attached reader. The
        // producer has raised layoutSeq to odd and stopped there (crashed
        // mid-rewrite, or wedged). A correct reader re-reads a bounded number
        // of times and then gives up - it must not spin forever, and it must
        // not snapshot the odd state.
        Sec a = makeSegment(g4);
        publishBlock(a, 0, 0, kDiscontinuity);
        Sec b = a;
        b.h().layoutSeq = 1;   // rewrite in progress, and it never finishes

        em.addCase("read.seqlock_retry_bound",
                   "a layoutSeq left odd is never snapshotted: the bounded retry gives up and "
                   "detaches rather than spinning or reading the torn header",
                   em.blob("read_seqlock_retry_bound_a", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "header"}, {"expect", {{"channelCount", 4}, {"layoutSeq", 0}}}},
                    {{"op", "patch"}, {"blob", em.blob("read_seqlock_retry_bound_b", b)}},
                    {{"op", "refreshSnapshot"}, {"expect", false}},
                    {{"op", "attached"}, {"expect", false}},
                    {{"op", "next"}, {"expect", "end"}}});
    }

    // --- a hostile rewrite detaches the reader ---------------------------------
    {
        Sec a = makeSegment(g2);
        publishBlock(a, 0, 0, kDiscontinuity);
        Sec b = a;
        b.h().layoutSeq       = 2;
        b.h().samplesPerBlock = 4096;   // planes still say 4

        em.addCase("read.reconfigure_detach",
                   "a rewrite that fails validation (samplesPerBlock overruns the plane) detaches "
                   "the reader rather than copying to the new bound",
                   em.blob("read_reconfigure_detach_a", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "patch"}, {"blob", em.blob("read_reconfigure_detach_b", b)}},
                    {{"op", "next"}, {"expect", "end"}},
                    {{"op", "attached"}, {"expect", false}}});
    }

    // --- a rewrite to an unknown enum detaches too -------------------------------
    //
    // "Reject unknown enum values" (versioning.md, reader rule 3) is a
    // per-snapshot rule, not an attach-time one. Both cases below pass every
    // geometry check - only the enum is wrong - so a binding that checks the
    // enums at attach and never again decodes them under the wrong
    // interpretation and reports success. That is exactly the silent misread
    // the rule exists to prevent, which is why each gets its own vector.
    {
        Sec a = makeSegment(g2);
        publishBlock(a, 0, 0, kDiscontinuity);
        Sec b = a;
        b.h().layoutSeq    = 2;
        b.h().sampleFormat = 3;   // a v1.x format this build does not know

        em.addCase("read.reconfigure_unknown_sample_format",
                   "a rewrite to an unknown sampleFormat detaches the reader; bytesPerSample still "
                   "agrees with Float32, so nothing but the enum check can catch it",
                   em.blob("read_reconfigure_unknown_sample_format_a", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "patch"},
                     {"blob", em.blob("read_reconfigure_unknown_sample_format_b", b)}},
                    {{"op", "next"}, {"expect", "end"}},
                    {{"op", "attached"}, {"expect", false}}});
    }
    {
        Sec a = makeSegment(g2);
        publishBlock(a, 0, 0, kDiscontinuity);
        Sec b = a;
        b.h().layoutSeq     = 2;
        b.h().channelLayout = static_cast<uint32_t>(ChannelLayout::Interleaved);

        em.addCase("read.reconfigure_reserved_channel_layout",
                   "a rewrite to channelLayout=2 (Interleaved, reserved) detaches the reader rather "
                   "than decoding interleaved samples with planar strides",
                   em.blob("read_reconfigure_reserved_channel_layout_a", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "patch"},
                     {"blob", em.blob("read_reconfigure_reserved_channel_layout_b", b)}},
                    {{"op", "next"}, {"expect", "end"}},
                    {{"op", "attached"}, {"expect", false}}});
    }

    // --- a reconfigure consumed by next() is still classifiable -----------------
    //
    // The trap every binding with a Client-style wrapper walks into. The
    // reader's "last reconfigure kind" describes its most recent refresh, and
    // next() refreshes on EVERY read cycle - so a rewrite landing mid-drain is
    // consumed by next(), and a wrapper that asks afterwards is told None and
    // reports a geometry change as a cosmetic restart ("layout unchanged"),
    // whereupon the consumer skips resizing everything it sized from the old
    // layout.
    //
    // The fix a binding must implement is a per-observer watermark: keep a copy
    // of the header snapshot and classify the current one against it, which is
    // answerable no matter who refreshed in between. The last two steps are the
    // trap and the fix, back to back.
    {
        Sec a = makeSegment(g4);
        publishBlock(a, 0, 0, kDiscontinuity);
        Sec b = a;
        ShmStreamHeader& h = b.h();
        h.layoutSeq    = 2;
        h.channelCount = 2;
        h.slotBytes    = align64(slotPayloadOffsetOf() + 2 * channelPlaneBytesOf(g4));

        em.addCase("read.reconfigure_consumed_by_next",
                   "a geometry rewrite consumed by next()'s internal refresh is no longer reported "
                   "by the reader's last-reconfigure-kind, but is still classifiable against a "
                   "header watermark taken before it",
                   em.blob("read_reconfigure_consumed_by_next_a", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "markHeader"}},
                    {{"op", "patch"}, {"blob", em.blob("read_reconfigure_consumed_by_next_b", b)}},
                    // next() refreshes before it looks at writeSeq, so it
                    // consumes the rewrite even with no block to return.
                    {{"op", "next"}, {"expect", "end"}},
                    {{"op", "reconfigureKind"}, {"expect", "Geometry"}},
                    // ...and now the latch is spent. This is the trap:
                    {{"op", "refreshSnapshot"}, {"expect", true}},
                    {{"op", "reconfigureKind"}, {"expect", "None"}},
                    // ...while the watermark still answers correctly:
                    {{"op", "reconfigureSince"}, {"expect", "Geometry"}}});
    }

    // --- layoutEpoch mismatch rejects the block ---------------------------------
    {
        Sec s = makeSegment(g4);
        publishBlock(s, 0, 0, kDiscontinuity);
        auto* desc        = reinterpret_cast<ShmBlockDesc*>(slotAt(s.bytes.data(), s.h(), 0));
        desc->layoutEpoch = 0xDEADBEEFu;   // header layoutSeq is 0

        em.addCase("read.layout_epoch_mismatch",
                   "a block whose layoutEpoch differs from the snapshot's layoutSeq is discarded, "
                   "not decoded with the wrong strides",
                   em.blob("read_layout_epoch_mismatch", s),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "rewindToOldest"}},
                    {{"op", "next"}, {"expect", "end"}},
                    {{"op", "droppedBlocks"}, {"expect", 1}}});
    }

    // --- inflated offsets are honored: the consumer never recomputes -----------
    {
        // A hypothetical future producer with a larger descriptor and padded
        // planes. Every address comes from the header's own fields; a binding
        // that recomputes any of them from the v1 formulas decodes garbage.
        Sec s = makeSegment(g2);
        const uint32_t inflatedPayloadOffset = 128;                          // NOT sizeof(ShmBlockDesc)
        const uint32_t inflatedPlaneBytes    = channelPlaneBytesOf(g2) + 16; // padded planes
        const uint32_t inflatedSlotBytes     = align64(128 + g2.channelCount * inflatedPlaneBytes);
        const uint32_t inflatedTotal         = slotArrayOffsetOf() + g2.ringBlocks * inflatedSlotBytes;
        {
            ShmStreamHeader& h  = s.h();
            h.slotPayloadOffset = inflatedPayloadOffset;
            h.channelPlaneBytes = inflatedPlaneBytes;
            h.slotBytes         = inflatedSlotBytes;
            h.totalBytes        = inflatedTotal;
        }
        s.bytes.resize(inflatedTotal, 0);

        const std::vector<float> decoded = publishBlock(s, 0, 0, kDiscontinuity);
        Sec a = s;
        a.h().writeSeq = 0;   // published payload, not yet visible: attach first

        em.addCase("read.inflated_offsets_honored",
                   "slotPayloadOffset, slotBytes and channelPlaneBytes larger than the v1 formula "
                   "gives are honored as published; nothing is recomputed",
                   em.blob("read_inflated_offsets_a", a),
                   {attachStep(expectResult("Ok", false,
                        {{"fields",
                          {{"slotPayloadOffset", inflatedPayloadOffset},
                           {"slotBytes", inflatedSlotBytes},
                           {"channelPlaneBytes", inflatedPlaneBytes}}}})),
                    {{"op", "patch"}, {"blob", em.blob("read_inflated_offsets_b", s)}},
                    {{"op", "next"},
                     {"block", blockExpect(0, 0, kDiscontinuity, g2.channelCount, g2.samplesPerBlock,
                                           decoded, defaultSpots(g2.channelCount, g2.samplesPerBlock),
                                           0.0, true)}},
                    {{"op", "droppedBlocks"}, {"expect", 0}}});
    }

    // --- int16 payload: +/-32767 convention, no wrap ------------------------------
    {
        const LayoutParams gi16{2, 4, 2, SampleFormat::Int16};
        Sec s = makeSegment(gi16);
        // Raw int16 values, planar: ch0 then ch1. Covers zero, mid-scale,
        // full-scale both signs, and raw -32768, which decodes to -1.0000305
        // (marginally outside [-1,1] - the documented asymmetry).
        const std::vector<int16_t> raw = {
            0, 16384, -16384, 32767,        // ch0
            -32768, -32767, 12345, -12345,  // ch1
        };
        const std::vector<float> decoded = publishBlock(s, 0, 0, kDiscontinuity, &raw);

        std::vector<std::pair<uint32_t, uint32_t>> allSpots;
        for (uint32_t ch = 0; ch < gi16.channelCount; ++ch)
            for (uint32_t i = 0; i < gi16.samplesPerBlock; ++i)
                allSpots.emplace_back(ch, i);

        // Tolerance 1e-5: tighter than 1/3 LSB (1 LSB = 3.05e-5), loose
        // enough for v*(1/32767) vs v/32767 to differ by an ulp. No hash:
        // the int16 decode is tolerance-defined, not bit-defined.
        em.addCase("read.int16_round_trip",
                   "int16 decodes via the +/-32767 convention within 1 LSB; raw -32768 yields "
                   "-1.0000305 (clamped encode, asymmetric decode - never a wrap)",
                   em.blob("read_int16_round_trip", s),
                   {attachStep(expectResult("Ok", false, {{"fields", {{"sampleFormat", "Int16"}}}})),
                    {{"op", "rewindToOldest"}},
                    {{"op", "next"},
                     {"block", blockExpect(0, 0, kDiscontinuity, gi16.channelCount,
                                           gi16.samplesPerBlock, decoded, allSpots, 1e-5, false)}},
                    {{"op", "droppedBlocks"}, {"expect", 0}}});
    }

    // --- liveness over a synthetic clock ----------------------------------------
    {
        Sec a = makeSegment(g4);
        publishBlock(a, 0, 0, kDiscontinuity);   // lastPublishQpc = 1000
        Sec b = a;
        b.h().deviceState = static_cast<uint32_t>(DeviceState::Inactive);
        Sec c = a;
        c.h().deviceState = static_cast<uint32_t>(DeviceState::Closed);

        // staleAfterMs floors at 100 ms; qpcFrequency is 10^7, so staleness
        // begins 1,000,000 ticks after the last publish (qpc 1000).
        em.addCase("read.liveness_states",
                   "Active+stale is Stalled; Inactive and Closed are legitimate quiet however stale",
                   em.blob("read_liveness_active", a),
                   {attachStep(expectResult("Ok", false)),
                    {{"op", "state"}, {"nowQpc", 1001}, {"expect", "Live"}},
                    {{"op", "state"}, {"nowQpc", 2001000}, {"expect", "Stalled"}},
                    {{"op", "patch"}, {"blob", em.blob("read_liveness_inactive", b)}},
                    {{"op", "state"}, {"nowQpc", 2001000}, {"expect", "Quiet"}},
                    {{"op", "patch"}, {"blob", em.blob("read_liveness_closed", c)}},
                    {{"op", "state"}, {"nowQpc", 2001000}, {"expect", "Closed"}}});
    }

    // =======================================================================
    // Manifest
    // =======================================================================

    ordered_json manifest;
    manifest["suite"]         = "stream-shm";
    manifest["formatVersion"] = "1.0";
    manifest["schema"]        = 2;
    manifest["generatedBy"]   = "tools/mkvectors - do not hand-edit; regenerate and diff";
    manifest["conventions"]   = {
          {"endianness", "little"},
          {"blobPaths", "relative to this manifest's directory"},
          {"sampleOrder", "planar channel-major: all of channel 0's samples, then channel 1, ..."},
          {"samplesHash", "FNV-1a 64 (offset 14695981039346656037, prime 1099511628211) over the "
                          "little-endian IEEE-754 bit patterns of the decoded float32 samples, "
                          "in sampleOrder"},
    };
    manifest["cases"] = std::move(em.cases);

    const std::string text = manifest.dump(2) + "\n";
    writeFile(em.outDir / "manifest.json", text.data(), text.size());

    std::printf("mkvectors: %zu cases -> %s\n", manifest["cases"].size(),
                em.outDir.string().c_str());
    return 0;
}
