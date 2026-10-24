// Unit tests for irtactile::shm::stream (irtactile-shm, stream-shm v1), the shared-memory
// audio stream contract. Normative spec: spec/stream-shm/v1/README.md.
//
// Every test here runs over a plain std::vector<uint8_t> -- no real mapping,
// no Win32 object, no thread. That is the whole point of splitting
// stream binding out as dependency-free and Win32-free: the publish protocol,
// the addressing arithmetic and the reader's forward-compatibility rules are
// all testable as pure data transformations. Real-mapping /
// real-event coverage lives in integration_tests/ instead.

#define IRTACTILE_SHM_TEST_ACCESS

#include <doctest/doctest.h>

#include <irtactile/shm/stream.h>

#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace irtactile::shm::stream;

namespace {

// A minimal producer, driving the exact publish protocol steps over
// an owned buffer. Exercises the format directly, with no producer-side
// framework to hide bugs in.
class TestProducer {
public:
    explicit TestProducer(LayoutParams p, uint32_t channelsWired = 0xFFFFFFFFu)
        : m_p(p)
    {
        m_bytes.assign(static_cast<size_t>(computeTotalSize(p)), 0);
        Header& h = header();
        h.magic             = 0;   // not-ready window
        h.versionMajor      = kVersionMajor;
        h.versionMinor      = kVersionMinor;
        h.headerBytes       = static_cast<uint32_t>(sizeof(Header));
        h.totalBytes        = static_cast<uint32_t>(computeTotalSize(p));
        h.sampleFormat      = static_cast<uint32_t>(p.sampleFormat);
        h.channelLayout     = static_cast<uint32_t>(ChannelLayout::Planar);
        h.channelCount      = p.channelCount;
        h.sampleRateMilliHz = 3'000'000;
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
        h.qpcFrequency      = 10'000'000;
        h.wiredChannelMask  = channelsWired;
        h.deviceState       = static_cast<uint32_t>(DeviceState::Active);
        irtactile::shm::detail::releaseStore(h.magic, kMagic);
    }

    using Header = ShmStreamHeader;
    using Desc   = ShmBlockDesc;

    Header& header() { return *reinterpret_cast<Header*>(m_bytes.data()); }
    void*   base()    { return m_bytes.data(); }
    size_t  size() const { return m_bytes.size(); }

    /// Publish one block. Every channel's samples equal
    /// `(firstSampleIndex + i) + ch * 1000`, so a test can predict any sample.
    void publish(bool forceSilent = false)
    {
        Header& h    = header();
        uint8_t* slot = slotAt(static_cast<uint8_t*>(m_bytes.data()), h, m_writeSeq);
        auto*    desc = reinterpret_cast<Desc*>(slot);

        irtactile::shm::detail::releaseStore(desc->seq, kSlotBusy);

        uint32_t silent = 0;
        for (uint32_t ch = 0; ch < h.channelCount; ++ch) {
            uint8_t* plane = planeAt(slot, h, ch);
            bool     nz    = false;
            for (uint32_t i = 0; i < h.samplesPerBlock; ++i) {
                const float v = forceSilent ? 0.0f
                                            : static_cast<float>(m_first + i) +
                                                  static_cast<float>(ch) * 1000.0f;
                if (v != 0.0f) nz = true;
                if (h.bytesPerSample == 2) {
                    const int16_t q = floatToInt16(v / 100000.0f);
                    std::memcpy(plane + static_cast<size_t>(i) * 2, &q, 2);
                } else {
                    std::memcpy(plane + static_cast<size_t>(i) * 4, &v, 4);
                }
            }
            if (!nz) silent |= (1u << ch);
        }

        const uint32_t fullMask = h.channelCount >= 32 ? 0xFFFFFFFFu : ((1u << h.channelCount) - 1u);
        uint32_t flags = (silent == fullMask) ? static_cast<uint32_t>(BlockSilence) : 0u;
        if (m_writeSeq == 0) flags |= static_cast<uint32_t>(BlockDiscontinuity);

        desc->firstSampleIndex  = m_first;
        desc->timestampQpc      = (m_writeSeq + 1) * 1000;
        desc->flags             = flags;
        desc->silentChannelMask = silent;
        // Producer obligation: stamp the layoutSeq the payload was
        // encoded under, so a consumer that re-snapshots to a newer layout
        // rejects this block instead of decoding it with the wrong strides.
        desc->layoutEpoch       = irtactile::shm::detail::acquireLoad(h.layoutSeq);

        irtactile::shm::detail::releaseStore(desc->seq, m_writeSeq);
        irtactile::shm::detail::releaseStore(h.lastPublishQpc, desc->timestampQpc);
        irtactile::shm::detail::releaseStore(h.writeSeq, m_writeSeq + 1);

        ++m_writeSeq;
        m_first += h.samplesPerBlock;
    }

    uint64_t writeSeq() const { return m_writeSeq; }

private:
    LayoutParams         m_p;
    std::vector<uint8_t> m_bytes;
    uint64_t             m_writeSeq = 0;
    uint64_t             m_first    = 0;
};

LayoutParams defaultParams()
{
    return LayoutParams{ /*channelCount*/ 4, /*samplesPerBlock*/ 8,
                         /*ringBlocks*/ 2, SampleFormat::Float32 };
}

} // namespace

// ===========================================================================
// Size arithmetic
// ===========================================================================
TEST_CASE("computeTotalSize, slotBytes and channelPlaneBytes match hand-computed values") {
    // Literals worked out by hand from the spec (header 256, descriptor 64,
    // slots rounded up to 64), not recomputed with the formula under test.
    struct Case { LayoutParams p; uint32_t cpb; uint32_t slotBytes; uint64_t total; };
    const Case cases[] = {
        {{1, 1, 2, SampleFormat::Float32},     4,   128,    512},   // 68 -> 128
        {{8, 48, 2, SampleFormat::Float32},  192,  1600,   3456},   // already a multiple of 64
        {{32, 1, 256, SampleFormat::Int16},    2,   128,  33024},
        {{8, 768, 2, SampleFormat::Float32}, 3072, 24640, 49536},
        {{20, 128, 64, SampleFormat::Int16}, 256,  5184, 332032},
        {{3, 5, 4, SampleFormat::Int16},      10,   128,    768},   // 94 -> 128
    };
    for (const auto& c : cases) {
        CAPTURE(c.p.channelCount);
        CAPTURE(c.p.samplesPerBlock);
        CHECK(channelPlaneBytesOf(c.p) == c.cpb);
        CHECK(slotBytesOf(c.p) == c.slotBytes);
        CHECK(computeTotalSize(c.p) == c.total);
    }
}

// ===========================================================================
// Attach
// ===========================================================================
TEST_CASE("attach: zero-filled segment is NotReady and retryable") {
    std::vector<uint8_t> zeros(sizeof(ShmStreamHeader) + 4096, 0);
    Reader r;
    const AttachResult ar = r.attach(zeros.data(), zeros.size());
    CHECK(ar == AttachResult::NotReady);
    CHECK(shouldRetry(ar));
}

TEST_CASE("attach: wrong magic is Malformed and fatal") {
    std::vector<uint8_t> bytes(sizeof(ShmStreamHeader) + 4096, 0);
    auto* h = reinterpret_cast<ShmStreamHeader*>(bytes.data());
    h->magic = 0xDEADBEEFu;
    Reader r;
    const AttachResult ar = r.attach(bytes.data(), bytes.size());
    CHECK(ar == AttachResult::Malformed);
    CHECK_FALSE(shouldRetry(ar));
}

TEST_CASE("attach: higher versionMajor is BadVersion and not retryable") {
    TestProducer prod(defaultParams());
    prod.header().versionMajor = kVersionMajor + 1;
    Reader r;
    const AttachResult ar = r.attach(prod.base(), prod.size());
    CHECK(ar == AttachResult::BadVersion);
    CHECK_FALSE(shouldRetry(ar));
}

TEST_CASE("attach: higher versionMinor is tolerated") {
    TestProducer prod(defaultParams());
    prod.header().versionMinor = kVersionMinor + 5;
    Reader r;
    CHECK(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
}

TEST_CASE("attach: unknown sampleFormat / channelLayout are rejected, not defaulted") {
    {
        TestProducer prod(defaultParams());
        prod.header().sampleFormat = 99;
        Reader r;
        CHECK(r.attach(prod.base(), prod.size()) == AttachResult::BadVersion);
    }
    {
        TestProducer prod(defaultParams());
        prod.header().channelLayout = 99;
        Reader r;
        CHECK(r.attach(prod.base(), prod.size()) == AttachResult::BadVersion);
    }
}

TEST_CASE("attach: an offset past mappedBytes is Malformed") {
    TestProducer prod(defaultParams());
    Reader r;
    // Map fewer bytes than the segment claims.
    const AttachResult ar = r.attach(prod.base(), prod.size() - 1);
    CHECK(ar == AttachResult::Malformed);
}

TEST_CASE("attach: a plane-misaligned layout is Malformed") {
    // The plane-alignment guarantee ("planes are bytesPerSample-aligned") is
    // validated, not assumed: every term of the plane address must be a
    // multiple of bytesPerSample (4 here). Each subcase breaks exactly one
    // term while keeping every size relation satisfiable, which needs a
    // padded buffer -- TestProducer's is the exact live footprint.
    TestProducer prod(defaultParams());   // CPB 32, slotBytes 192, bps 4
    std::vector<uint8_t> bytes(prod.size() + 4096, 0);
    std::memcpy(bytes.data(), prod.base(), prod.size());
    auto* h = reinterpret_cast<ShmStreamHeader*>(bytes.data());
    h->totalBytes = static_cast<uint32_t>(bytes.size());

    SUBCASE("channelPlaneBytes") { h->channelPlaneBytes = 34; h->slotBytes = 256; }
    SUBCASE("slotPayloadOffset") { h->slotPayloadOffset = 66; h->slotBytes = 256; }
    SUBCASE("slotBytes")         { h->slotBytes = 194; }
    SUBCASE("slotArrayOffset")   { h->slotArrayOffset = 258; }

    Reader r;
    CHECK(r.attach(bytes.data(), bytes.size()) == AttachResult::Malformed);
}

TEST_CASE("attach: never copies more than headerBytes") {
    // A hypothetical v1.1 producer whose header grew by 64 bytes: headerBytes
    // and slotArrayOffset both move out together (as a real producer's would),
    // leaving a gap a v1.0 Reader knows nothing about between the end of its
    // own struct and where the slot array actually begins.
    LayoutParams p{2, 4, 2, SampleFormat::Float32};
    const uint32_t grownHeaderBytes  = static_cast<uint32_t>(sizeof(ShmStreamHeader)) + 64;
    const uint32_t grownSlotArrayOff = align64(grownHeaderBytes);
    const uint32_t cpb      = channelPlaneBytesOf(p);
    const uint32_t slotB    = slotBytesOf(p);
    const uint64_t total    = grownSlotArrayOff + static_cast<uint64_t>(p.ringBlocks) * slotB;

    std::vector<uint8_t> bytes(static_cast<size_t>(total), 0);
    auto* h = reinterpret_cast<ShmStreamHeader*>(bytes.data());
    h->versionMajor      = kVersionMajor;
    h->versionMinor      = kVersionMinor;
    h->headerBytes       = grownHeaderBytes;   // > sizeof(ShmStreamHeader)
    h->totalBytes        = static_cast<uint32_t>(total);
    h->sampleFormat      = static_cast<uint32_t>(p.sampleFormat);
    h->channelLayout     = static_cast<uint32_t>(ChannelLayout::Planar);
    h->channelCount      = p.channelCount;
    h->sampleRateMilliHz = 3'000'000;
    h->sourceSampleRate  = 48000;
    h->rateDivisor       = 16;
    h->samplesPerBlock   = p.samplesPerBlock;
    h->ringBlocks        = p.ringBlocks;
    h->bytesPerSample    = bytesPerSampleOf(p.sampleFormat);
    h->slotPayloadOffset = slotPayloadOffsetOf();
    h->channelArrayOffset = 0;
    h->slotArrayOffset   = grownSlotArrayOff;   // moved out with the header
    h->slotBytes         = slotB;
    h->channelPlaneBytes = cpb;
    h->qpcFrequency      = 10'000'000;
    h->deviceState       = static_cast<uint32_t>(DeviceState::Active);
    irtactile::shm::detail::releaseStore(h->magic, kMagic);

    // One block, addressed through the moved slotArrayOffset.
    uint8_t* slot = slotAt(bytes.data(), *h, uint64_t{0});
    for (uint32_t ch = 0; ch < p.channelCount; ++ch) {
        uint8_t* plane = planeAt(slot, *h, ch);
        for (uint32_t i = 0; i < p.samplesPerBlock; ++i) {
            const float v = static_cast<float>(i) + static_cast<float>(ch) * 1000.0f;
            std::memcpy(plane + static_cast<size_t>(i) * 4, &v, 4);
        }
    }
    irtactile::shm::detail::releaseStore(reinterpret_cast<ShmBlockDesc*>(slot)->seq, uint64_t{0});

    // attach() must clamp its own memcpy to min(headerBytes, sizeof(its own
    // struct)) = sizeof(ShmStreamHeader) bytes -- never sizeof(ShmStreamHeader)
    // + 64, which would read past the reader's own snapshot buffer (only an
    // ASan build sees that overrun directly).
    Reader r;
    REQUIRE(r.attach(bytes.data(), bytes.size()) == AttachResult::Ok);

    // The part a plain build can observe: the reader follows the published
    // slotArrayOffset into the gap-shifted slot array, not sizeof(its header).
    irtactile::shm::detail::releaseStore(h->writeSeq, uint64_t{1});
    BlockView b;
    REQUIRE(r.next(b));
    CHECK(b.channel(0)[0] == 0.0f);
    CHECK(b.channel(0)[3] == 3.0f);
    CHECK(b.channel(1)[0] == 1000.0f);
    CHECK(b.channel(1)[3] == 1003.0f);
}

TEST_CASE("attach: non-zero reserved[] bytes are tolerated") {
    TestProducer prod(defaultParams());
    std::memset(prod.header().reserved, 0xAB, sizeof(prod.header().reserved));
    Reader r;
    CHECK(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
}

TEST_CASE("attach: cursor starts at writeSeq, not zero") {
    TestProducer prod(defaultParams());
    for (int i = 0; i < 10; ++i) prod.publish();
    // Simulate "producer has been running for hours": jump writeSeq far ahead
    // without actually publishing that many blocks (cheap stand-in for a long
    // run -- what matters is that attach() reads it rather than assuming 0).
    irtactile::shm::detail::releaseStore(prod.header().writeSeq, uint64_t{10'000'000});

    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    BlockView b;
    CHECK_FALSE(r.next(b));          // no gap reported on the first read
    CHECK(r.droppedBlocks() == 0);
}

// ===========================================================================
// StreamRequest / Mismatch
// ===========================================================================
TEST_CASE("StreamRequest: matching fields attach Ok; a zero field is not checked") {
    TestProducer prod(defaultParams());
    StreamRequest req{4, 3'000'000, 8, SampleFormat::Float32};
    Reader r;
    CHECK(r.attach(prod.base(), prod.size(), &req) == AttachResult::Ok);

    StreamRequest partial{4, 0, 0, static_cast<SampleFormat>(0)};
    Reader r2;
    CHECK(r2.attach(prod.base(), prod.size(), &partial) == AttachResult::Ok);
}

TEST_CASE("StreamRequest: a channelCount mismatch is Mismatch, names the field, and does not retry") {
    TestProducer prod(defaultParams());
    StreamRequest req{6, 0, 0, static_cast<SampleFormat>(0)};
    Reader r;
    const AttachResult ar = r.attach(prod.base(), prod.size(), &req);
    CHECK(ar == AttachResult::Mismatch);
    CHECK_FALSE(shouldRetry(ar));
    // Both numbers, each on the right side: a log reader must be able to tell
    // which one is the segment and which one is this build.
    CHECK(std::string(r.mismatchDetail()) == "channelCount: segment has 4, this build expects 6");
}

TEST_CASE("StreamRequest: several mismatching fields are all reported") {
    TestProducer prod(defaultParams());
    StreamRequest req{6, 375'000, 128, SampleFormat::Int16};
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size(), &req) == AttachResult::Mismatch);
    const std::string detail = r.mismatchDetail();
    CHECK(detail.find("channelCount: segment has 4, this build expects 6") != std::string::npos);
    CHECK(detail.find("sampleRateMilliHz: segment has 3000000, this build expects 375000") != std::string::npos);
    CHECK(detail.find("samplesPerBlock: segment has 8, this build expects 128") != std::string::npos);
    CHECK(detail.find("sampleFormat") != std::string::npos);
}

TEST_CASE("StreamRequest: each field is checked independently") {
    TestProducer prod(defaultParams());
    {
        Reader r;
        StreamRequest req{0, 375'000, 0, static_cast<SampleFormat>(0)};
        CHECK(r.attach(prod.base(), prod.size(), &req) == AttachResult::Mismatch);
        CHECK(std::string(r.mismatchDetail()).find("sampleRateMilliHz") != std::string::npos);
    }
    {
        Reader r;
        StreamRequest req{0, 0, 128, static_cast<SampleFormat>(0)};
        CHECK(r.attach(prod.base(), prod.size(), &req) == AttachResult::Mismatch);
        CHECK(std::string(r.mismatchDetail()).find("samplesPerBlock") != std::string::npos);
    }
    {
        Reader r;
        StreamRequest req{0, 0, 0, SampleFormat::Int16};
        CHECK(r.attach(prod.base(), prod.size(), &req) == AttachResult::Mismatch);
        CHECK(std::string(r.mismatchDetail()).find("sampleFormat") != std::string::npos);
    }
}

// ===========================================================================
// Publish / consume round-trip
// ===========================================================================
TEST_CASE("attach: samplesPerBlock overrunning the channel plane is rejected") {
    // The copy bound is header.samplesPerBlock, so THIS is the check that
    // stands between a corrupt segment and a read past the mapping -- the job
    // the old per-block sampleCount clamp used to do, moved to the one seam
    // that can do it once instead of per block.
    LayoutParams p{2, 4, 2, SampleFormat::Float32};
    TestProducer prod(p);

    ShmStreamHeader& h = prod.header();
    // channelPlaneBytes still says 4 samples; the header now claims 4096.
    h.samplesPerBlock = 4096;

    Reader r;
    CHECK(r.attach(prod.base(), prod.size()) == AttachResult::Malformed);
    CHECK_FALSE(r.attached());
}

TEST_CASE("refreshSnapshot: a rewrite overrunning the channel plane detaches the reader") {
    // Same bound, reached the other way: the segment validated at attach and
    // was then rewritten with a hostile samplesPerBlock under a layoutSeq bump.
    // The reader must fail validation and detach rather than copy to the new
    // bound out of the old planes.
    LayoutParams p{2, 4, 2, SampleFormat::Float32};
    TestProducer prod(p);

    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    prod.publish();

    ShmStreamHeader& h = prod.header();
    irtactile::shm::detail::releaseStore(h.layoutSeq, irtactile::shm::detail::acquireLoad(h.layoutSeq) + 2u);
    h.samplesPerBlock = 4096;

    BlockView b;
    CHECK_FALSE(r.next(b));
    CHECK_FALSE(r.attached());
}

TEST_CASE("refreshSnapshot: a rewrite to an unknown enum detaches the reader") {
    // "Reject unknown enum values" is a per-snapshot rule, not an attach-time
    // one: a producer restart may rewrite either enum. Falling through to a
    // default is the silent-misread this rule exists to stop -
    // bytesPerSampleOf() answers 4 for any unknown format, so an unchecked
    // sampleFormat=3 would keep decoding as Float32, and Interleaved would keep
    // decoding as Planar. Both must detach instead.
    LayoutParams p{2, 4, 2, SampleFormat::Float32};

    SUBCASE("unknown sampleFormat") {
        TestProducer prod(p);
        Reader r;
        REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
        prod.publish();

        ShmStreamHeader& h = prod.header();
        irtactile::shm::detail::releaseStore(h.layoutSeq, irtactile::shm::detail::acquireLoad(h.layoutSeq) + 2u);
        h.sampleFormat = 3;   // a v1.x format this build does not know

        BlockView b;
        CHECK_FALSE(r.next(b));
        CHECK_FALSE(r.attached());
    }

    SUBCASE("reserved channelLayout") {
        TestProducer prod(p);
        Reader r;
        REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
        prod.publish();

        ShmStreamHeader& h = prod.header();
        irtactile::shm::detail::releaseStore(h.layoutSeq, irtactile::shm::detail::acquireLoad(h.layoutSeq) + 2u);
        h.channelLayout = static_cast<uint32_t>(ChannelLayout::Interleaved);

        BlockView b;
        CHECK_FALSE(r.next(b));
        CHECK_FALSE(r.attached());
    }
}

TEST_CASE("Round-trip: N blocks in, N out, sample-exact, sequence numbers contiguous") {
    TestProducer prod(LayoutParams{4, 8, 8, SampleFormat::Float32});
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);

    for (int i = 0; i < 5; ++i) prod.publish();

    BlockView b;
    uint64_t  expectedSeq = 0;
    int       count       = 0;
    while (r.next(b)) {
        CHECK(b.seq == expectedSeq);
        CHECK(b.sampleCount == 8);
        for (uint32_t ch = 0; ch < 4; ++ch) {
            const float* s = b.channel(ch);
            for (uint32_t i = 0; i < b.sampleCount; ++i) {
                const float want = static_cast<float>(b.firstSampleIndex + i) + static_cast<float>(ch) * 1000.0f;
                CHECK(s[i] == doctest::Approx(want));
            }
        }
        ++expectedSeq;
        ++count;
    }
    CHECK(count == 5);
    CHECK(r.droppedBlocks() == 0);
}

TEST_CASE("Overrun: publishing ringBlocks+10 without draining is detected and resynchronised") {
    TestProducer prod(LayoutParams{2, 4, 4, SampleFormat::Float32});   // ring 4
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);

    for (int i = 0; i < 14; ++i) prod.publish();   // ringBlocks(4) + 10

    // Readable window is ringBlocks - 1: the newest three survive, and every
    // other block is counted as dropped -- exactly, so an off-by-one in the
    // resync shows up here.
    BlockView b;
    std::vector<uint64_t> seqs;
    while (r.next(b)) seqs.push_back(b.seq);
    CHECK(seqs == std::vector<uint64_t>{11, 12, 13});
    CHECK(r.droppedBlocks() == 11);
}

TEST_CASE("Overrun boundary at ringBlocks=2: exactly one block past cursor delivers; two reports a gap") {
    {
        TestProducer prod(defaultParams());   // ring 2
        Reader r;
        REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
        prod.publish();
        BlockView b;
        CHECK(r.next(b));
        CHECK(b.seq == 0);
        CHECK_FALSE(r.next(b));
    }
    {
        TestProducer prod(defaultParams());   // ring 2
        Reader r;
        REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
        prod.publish();
        prod.publish();
        BlockView b;
        // Readable window is 1 (ringBlocks-1): the writer's in-progress slot
        // is never handed over, so exactly the newest complete block survives.
        int count = 0;
        while (r.next(b)) ++count;
        CHECK(count == 1);
        CHECK(r.droppedBlocks() == 1);
    }
}

TEST_CASE("Slot recycling: a slot reclaimed as kSlotBusy is never delivered") {
    TestProducer prod(defaultParams());   // ring 2
    prod.publish();               // seq 0 in slot 0
    prod.publish();               // seq 1 in slot 1

    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);   // cursor = 2
    prod.publish();               // seq 2 in slot 0, complete; writeSeq = 3

    // The producer has already reclaimed slot 0 for a later block while the
    // reader's view of writeSeq still puts seq 2 in range. The slot held a
    // valid seq-2 block a moment ago, so only the kSlotBusy marker stands
    // between the reader and a half-rewritten payload. kSlotBusy is
    // ~0: a reader that compared with >= instead of == would accept it.
    uint8_t* slot0 = slotAt(static_cast<uint8_t*>(prod.base()), prod.header(), uint64_t{0});
    irtactile::shm::detail::releaseStore(reinterpret_cast<ShmBlockDesc*>(slot0)->seq, kSlotBusy);

    BlockView b;
    CHECK_FALSE(r.next(b));
    CHECK(r.droppedBlocks() == 1);
}

// ===========================================================================
// layoutSeq seqlock / reconfiguration
// ===========================================================================
TEST_CASE("layoutSeq seqlock: a rewrite (odd -> fields change -> even) is picked up on the next read") {
    TestProducer prod(defaultParams());
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    prod.publish();
    BlockView b;
    CHECK(r.next(b));
    CHECK(r.channelCount() == 4);

    // A producer restart into a reused, differently-configured segment bumps
    // layoutSeq odd -> rewrite -> even. Reproduce just that bump here
    // and confirm the reader re-snapshots rather than continuing against the
    // stale layout it attached with.
    auto& h = prod.header();
    uint32_t seq = h.layoutSeq;
    irtactile::shm::detail::releaseStore(h.layoutSeq, ++seq);   // odd: rewrite in progress
    h.channelCount = 2;
    irtactile::shm::detail::releaseStore(h.layoutSeq, ++seq);   // even: consistent again

    // The re-snapshot happens on the next read cycle, not merely by polling
    // an accessor -- next()/latest() are what call refreshIfReconfigured().
    CHECK_FALSE(r.next(b));         // no new block, but the header is re-read
    CHECK(r.channelCount() == 2);   // ...and the new layout is now visible
}

TEST_CASE("layoutSeq move during copy brackets the block and counts a drop") {
    // If the producer bumps layoutSeq after the descriptor is written but
    // before the reader finishes copying the slot, the reader's post-copy
    // layoutSeq re-check must discard the block. A literal thread race cannot
    // be reproduced in a single-threaded unit test, so we simulate the
    // outcome: the snapshot layoutSeq that entered copyBlock() is stale while
    // the live header has already moved on. The block must be discarded and
    // counted as a drop.
    TestProducer prod(defaultParams());
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    prod.publish();

    auto& h = prod.header();
    irtactile::shm::detail::releaseStore(h.layoutSeq, h.layoutSeq + 2);

    // The block's own layoutEpoch is 0 and so is the injected snapshot value,
    // so the Phase-4 epoch check cannot be what rejects this: only the
    // post-copy layoutSeq bracket can.
    BlockView b;
    CHECK_FALSE(r.testCopyStepWithStaleSnapLayoutSeq(0, b));
    CHECK(r.droppedBlocks() == 1);
}

TEST_CASE("ReconfigureKind: no layout change reports None") {
    TestProducer prod(defaultParams());
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    REQUIRE(r.refreshSnapshot());
    CHECK(r.lastReconfigureKind() == Reader::ReconfigureKind::None);
}

TEST_CASE("ReconfigureKind: addressing-field change is Geometry") {
    TestProducer prod(defaultParams());
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);

    auto& h = prod.header();
    uint32_t seq = h.layoutSeq;
    irtactile::shm::detail::releaseStore(h.layoutSeq, ++seq);                       // odd: rewrite in progress
    h.channelCount = 2;
    h.slotBytes    = align64(slotPayloadOffsetOf() + h.channelCount * h.channelPlaneBytes);
    irtactile::shm::detail::releaseStore(h.layoutSeq, ++seq);                       // even: consistent

    REQUIRE(r.refreshSnapshot());
    CHECK(r.lastReconfigureKind() == Reader::ReconfigureKind::Geometry);
    CHECK(r.channelCount() == 2);
    CHECK(r.header().slotBytes == align64(slotPayloadOffsetOf() + 2u * h.channelPlaneBytes));
}

TEST_CASE("ReconfigureKind: diagnostic-only change is Cosmetic") {
    TestProducer prod(defaultParams());
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);

    auto& h = prod.header();
    uint32_t seq = h.layoutSeq;
    irtactile::shm::detail::releaseStore(h.layoutSeq, ++seq);                       // odd
    h.sourceSampleRate     = 96000;
    h.rateDivisor          = 8;
    h.diagEngineBlockSize  = 128;
    h.diagFramesPerPublish = 4;
    irtactile::shm::detail::releaseStore(h.layoutSeq, ++seq);                       // even

    REQUIRE(r.refreshSnapshot());
    CHECK(r.lastReconfigureKind() == Reader::ReconfigureKind::Cosmetic);
    CHECK(r.channelCount() == 4);
    CHECK(r.samplesPerBlock() == 8);
}

TEST_CASE("classifyReconfigure: every addressing field is Geometry, every other field Cosmetic") {
    // Field by field, so dropping one from the classifier's list is caught: a
    // missed Geometry change makes the consumer keep decoding with the old
    // strides.
    TestProducer prod(defaultParams());
    const ShmStreamHeader before = prod.header();

    using Mutate = void (*)(ShmStreamHeader&);
    const std::pair<const char*, Mutate> geometry[] = {
        {"channelCount",       [](ShmStreamHeader& h) { h.channelCount += 1; }},
        {"sampleFormat",       [](ShmStreamHeader& h) { h.sampleFormat ^= 3u; }},
        {"channelLayout",      [](ShmStreamHeader& h) { h.channelLayout += 1; }},
        {"samplesPerBlock",    [](ShmStreamHeader& h) { h.samplesPerBlock += 1; }},
        {"ringBlocks",         [](ShmStreamHeader& h) { h.ringBlocks += 1; }},
        {"bytesPerSample",     [](ShmStreamHeader& h) { h.bytesPerSample = 2; }},
        {"slotPayloadOffset",  [](ShmStreamHeader& h) { h.slotPayloadOffset += 64; }},
        {"channelArrayOffset", [](ShmStreamHeader& h) { h.channelArrayOffset = 256; }},
        {"slotArrayOffset",    [](ShmStreamHeader& h) { h.slotArrayOffset += 64; }},
        {"slotBytes",          [](ShmStreamHeader& h) { h.slotBytes += 64; }},
        {"channelPlaneBytes",  [](ShmStreamHeader& h) { h.channelPlaneBytes += 4; }},
    };
    for (const auto& [name, mutate] : geometry) {
        CAPTURE(name);
        ShmStreamHeader after = before;
        mutate(after);
        CHECK(Reader::classifyReconfigure(before, after) == Reader::ReconfigureKind::Geometry);
    }

    const std::pair<const char*, Mutate> cosmetic[] = {
        {"nothing",              [](ShmStreamHeader&) {}},   // a bare restart still reports
        {"sampleRateMilliHz",    [](ShmStreamHeader& h) { h.sampleRateMilliHz = 375'000; }},
        {"sourceSampleRate",     [](ShmStreamHeader& h) { h.sourceSampleRate = 96000; }},
        {"rateDivisor",          [](ShmStreamHeader& h) { h.rateDivisor = 8; }},
        {"diagEngineBlockSize",  [](ShmStreamHeader& h) { h.diagEngineBlockSize = 128; }},
        {"diagFramesPerPublish", [](ShmStreamHeader& h) { h.diagFramesPerPublish = 4; }},
        {"deviceName",           [](ShmStreamHeader& h) { h.deviceName[0] = 'x'; }},
        {"producerPid",          [](ShmStreamHeader& h) { h.producerPid = 77; }},
    };
    for (const auto& [name, mutate] : cosmetic) {
        CAPTURE(name);
        ShmStreamHeader after = before;
        mutate(after);
        CHECK(Reader::classifyReconfigure(before, after) == Reader::ReconfigureKind::Cosmetic);
    }
}

TEST_CASE("layoutEpoch mismatch rejects a block") {
    // A block's layoutEpoch must equal the layoutSeq that was current when
    // the payload was encoded. If the descriptor claims a different epoch,
    // the reader must discard the block instead of decoding potentially
    // stale-geometry payload.
    TestProducer prod(defaultParams());
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    prod.publish();

    auto& h = prod.header();
    uint8_t* slot = slotAt(static_cast<uint8_t*>(prod.base()), h, uint64_t{0});
    auto*    desc = reinterpret_cast<ShmBlockDesc*>(slot);

    // Corrupt the epoch without changing the snapshot's layoutSeq or the
    // descriptor seq. Only the layoutEpoch check can catch this.
    irtactile::shm::detail::releaseStore(desc->layoutEpoch, uint32_t{0xDEADBEEFu});

    BlockView b;
    CHECK_FALSE(r.next(b));
    CHECK(r.droppedBlocks() == 1);
}

TEST_CASE("wiredChannelMask is observable without a re-snapshot") {
    TestProducer prod(defaultParams(), /*channelsWired*/ 0x3u);
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    CHECK(r.wiredChannelMask() == 0x3u);

    // Changed live, with layoutSeq untouched -- this field is deliberately
    // outside the seqlock, so no re-snapshot should be required.
    const uint32_t layoutBefore = r.layoutSeq();
    irtactile::shm::detail::releaseStore(prod.header().wiredChannelMask, uint32_t{0x5});
    CHECK(r.wiredChannelMask() == 0x5u);
    CHECK(r.layoutSeq() == layoutBefore);
}

// ===========================================================================
// Masks
// ===========================================================================
TEST_CASE("silentChannelMask: one silent channel sets one bit; full silence sets BlockSilence") {
    TestProducer prod(defaultParams());
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);   // before publishing
    prod.publish(/*forceSilent*/ true);
    BlockView b;
    REQUIRE(r.next(b));
    CHECK(b.silentChannelMask == 0xFu);   // all 4 channels silent
    CHECK((b.flags & BlockSilence) != 0);
}

// ===========================================================================
// Liveness / thresholds
// ===========================================================================
TEST_CASE("Period derivation depends only on samplesPerBlock and sampleRateMilliHz") {
    TestProducer prod(defaultParams());
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    // 8 samples / 3000 Hz = 2.6666... ms
    CHECK(r.periodMs() == doctest::Approx(8.0 / 3.0));
    CHECK(r.waitTimeoutMs() == doctest::Approx(50.0));    // 4x period < 50ms floor
    CHECK(r.staleAfterMs() == doctest::Approx(100.0));    // 5x period < 100ms floor
}

TEST_CASE("Size class derivation: smallest fitting class, and the config spelling") {
    // These live in stream/layout.h rather than in the producer because
    // both the producer and irTactileServer derive segment names from them.
    // A drift between the two would resolve to different names, which is the
    // one failure mode name derivation is supposed to make impossible.
    SUBCASE("smallestSizeClassFor picks the smallest envelope that fits") {
        CHECK(smallestSizeClassFor(LayoutParams{2, 128, 2, SampleFormat::Float32}) == SizeClass::S256K);
        CHECK(smallestSizeClassFor(LayoutParams{16, 128, 32, SampleFormat::Float32}) == SizeClass::S512K);
        CHECK(smallestSizeClassFor(LayoutParams{32, 512, 8, SampleFormat::Float32}) == SizeClass::S1M);
    }

    SUBCASE("the class is sized as Float32 even for an int16 layout") {
        // Same geometry, both formats: int16 needs half the bytes but must
        // land in the same class, so switching format later keeps the name.
        const LayoutParams i16{16, 128, 32, SampleFormat::Int16};
        const LayoutParams f32{16, 128, 32, SampleFormat::Float32};
        CHECK(computeTotalSize(i16) < computeTotalSize(f32));
        CHECK(smallestSizeClassFor(i16) == smallestSizeClassFor(f32));
    }

    SUBCASE("a layout of exactly the class size fits; one sample more does not") {
        // 256 + 132 x 1984 = 262144 bytes, exactly 256 KiB: the comparison is
        // <=, so this is the largest layout that still lands in _256k.
        const LayoutParams exact{8, 60, 132, SampleFormat::Float32};
        REQUIRE(computeTotalSize(exact) == kSizeClassBytes[0]);
        CHECK(smallestSizeClassFor(exact) == SizeClass::S256K);

        const LayoutParams over{8, 61, 132, SampleFormat::Float32};
        REQUIRE(computeTotalSize(over) > kSizeClassBytes[0]);
        CHECK(smallestSizeClassFor(over) == SizeClass::S512K);
    }

    SUBCASE("each suffix parses back to its own class") {
        for (size_t i = 0; i < 3; ++i) {
            const auto cls = static_cast<SizeClass>(i);
            SizeClass parsed{};
            REQUIRE(sizeClassFromString(kSizeClassSuffix[i] + 1, parsed));  // skip the '_'
            CHECK(parsed == cls);
        }
    }

    SUBCASE("sizeClassFromString rejects anything else") {
        SizeClass cls{};
        CHECK_FALSE(sizeClassFromString(nullptr, cls));
        CHECK_FALSE(sizeClassFromString("", cls));
        CHECK_FALSE(sizeClassFromString("256K", cls));   // case-sensitive
        CHECK_FALSE(sizeClassFromString("256k ", cls));
        CHECK_FALSE(sizeClassFromString("2m", cls));
        CHECK_FALSE(sizeClassFromString("1", cls));
    }
}

TEST_CASE("The reference profile resolves to the class its documentation claims") {
    // README.md's "ship it" device profile, and the toy producer in
    // examples/reader_min.cpp: 8 ch @ 3000 Hz (divisor 16), framesPerPublish 6
    // -> 48 samples/block, ring 2, float32.
    //
    // This exists because the size class is baked into the segment name, so the
    // class this profile lands in IS the name a third party opens. README.md
    // states it literally and examples/reader_win32.cpp hardcodes it as its
    // default argument; both go stale silently if the sizing ever shifts, and
    // nothing else would catch it -- reader_win32 is compiled but not run,
    // because it needs a live producer.
    uint32_t divisor = 0;
    REQUIRE(rateDivisorFromHz(3000.0, divisor));
    CHECK(divisor == 16);

    const uint32_t samplesPerBlock = 6u * 128u / divisor;
    CHECK(samplesPerBlock == 48);

    const LayoutParams p{8, samplesPerBlock, 2, SampleFormat::Float32};
    CHECK(channelPlaneBytesOf(p) == 192);
    CHECK(slotBytesOf(p) == 1600);
    CHECK(computeTotalSize(p) == 3456);          // the figure quoted in README

    CHECK(smallestSizeClassFor(p) == SizeClass::S256K);
    CHECK(std::string(kSizeClassSuffix[static_cast<size_t>(smallestSizeClassFor(p))]) == "_256k");
}

TEST_CASE("rateDivisorFromHz covers exactly the 11 ladder rungs") {
    struct Rung { double hz; uint32_t divisor; };
    const Rung rungs[] = {
        {48000.0, 1}, {24000.0, 2}, {12000.0, 4}, {6000.0, 8},
        {3000.0, 16}, {1500.0, 32}, {750.0, 64},  {375.0, 128},
        {187.5, 256}, {93.75, 512}, {46.875, 1024},
    };

    for (const auto& r : rungs) {
        uint32_t d = 0;
        REQUIRE(rateDivisorFromHz(r.hz, d));
        CHECK(d == r.divisor);
    }

    uint32_t d = 0;
    // A config value read back through text may be off in the last digits;
    // the match tolerance is 1 mHz, not a nearest-rung snap.
    CHECK(rateDivisorFromHz(3000.0005, d));
    CHECK(d == 16);
    CHECK(rateDivisorFromHz(46.8755, d));
    CHECK(d == 1024);
    CHECK_FALSE(rateDivisorFromHz(3000.002, d));
    CHECK_FALSE(rateDivisorFromHz(2999.99, d));
    CHECK_FALSE(rateDivisorFromHz(44100.0, d));
    CHECK_FALSE(rateDivisorFromHz(0.0, d));
    CHECK_FALSE(rateDivisorFromHz(96000.0, d));
    CHECK_FALSE(rateDivisorFromHz(-48000.0, d));
}

TEST_CASE("Derived quantities are exact on hand-built ladder rungs and match Reader") {
    // The engine publishes once per 128-sample frame at 375 Hz.  Batching
    // `framesPerPublish` frames gives samplesPerBlock = 128*fpp and
    // publishHz = 375 / fpp exactly. The thresholds are 4x / 5x the period
    // with 50 / 100 ms floors; only the last rung's period is long enough to
    // lift them, so both sides of each floor are covered.
    struct Rung {
        uint32_t framesPerPublish; uint32_t samplesPerBlock; double publishHz;
        double periodMs; double waitMs; double staleMs;
    };
    const Rung rungs[] = {
        {1,  128, 375.0,   8.0 / 3.0,  50.0, 100.0},
        {2,  256, 187.5,  16.0 / 3.0,  50.0, 100.0},
        {4,  512, 93.75,  32.0 / 3.0,  50.0, 100.0},
        {8, 1024, 46.875, 64.0 / 3.0, 256.0 / 3.0, 320.0 / 3.0},
    };

    for (const auto& rung : rungs) {
        LayoutParams p{4, rung.samplesPerBlock, 2, SampleFormat::Float32};
        const uint32_t cpb   = channelPlaneBytesOf(p);
        const uint32_t slotB = slotBytesOf(p);
        const uint64_t total = computeTotalSize(p);

        std::vector<uint8_t> bytes(static_cast<size_t>(total), 0);
        auto* h = reinterpret_cast<ShmStreamHeader*>(bytes.data());
        h->versionMajor       = kVersionMajor;
        h->versionMinor       = kVersionMinor;
        h->headerBytes        = static_cast<uint32_t>(sizeof(ShmStreamHeader));
        h->totalBytes         = static_cast<uint32_t>(total);
        h->sampleFormat       = static_cast<uint32_t>(SampleFormat::Float32);
        h->channelLayout      = static_cast<uint32_t>(ChannelLayout::Planar);
        h->channelCount       = p.channelCount;
        h->sampleRateMilliHz  = 48'000'000;   // 48 kHz operative rate
        h->sourceSampleRate   = 48000;
        h->rateDivisor        = 1;
        h->samplesPerBlock    = p.samplesPerBlock;
        h->ringBlocks         = p.ringBlocks;
        h->bytesPerSample     = bytesPerSampleOf(p.sampleFormat);
        h->slotPayloadOffset  = slotPayloadOffsetOf();
        h->channelArrayOffset = 0;
        h->slotArrayOffset    = slotArrayOffsetOf();
        h->slotBytes          = slotB;
        h->channelPlaneBytes  = cpb;
        h->qpcFrequency       = 10'000'000;
        h->deviceState        = static_cast<uint32_t>(DeviceState::Active);
        irtactile::shm::detail::releaseStore(h->magic, kMagic);

        CAPTURE(rung.samplesPerBlock);
        CHECK(periodSecondsOf(*h) == doctest::Approx(rung.periodMs / 1000.0));
        CHECK(periodMsOf(*h) == doctest::Approx(rung.periodMs));
        CHECK(publishHzOf(*h) == doctest::Approx(rung.publishHz));
        CHECK(ringSecondsOf(*h) == doctest::Approx(rung.periodMs / 1000.0 * p.ringBlocks));
        CHECK(waitTimeoutMsOf(*h) == doctest::Approx(rung.waitMs));
        CHECK(staleAfterMsOf(*h) == doctest::Approx(rung.staleMs));

        Reader r;
        REQUIRE(r.attach(bytes.data(), bytes.size()) == AttachResult::Ok);
        CHECK(r.periodMs() == doctest::Approx(rung.periodMs));
        CHECK(r.publishHz() == doctest::Approx(rung.publishHz));
        CHECK(r.waitTimeoutMs() == doctest::Approx(rung.waitMs));
        CHECK(r.staleAfterMs() == doctest::Approx(rung.staleMs));
    }
}

TEST_CASE("Staleness is decided without sleeping, over a synthetic clock") {
    TestProducer prod(defaultParams());
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    prod.publish();
    BlockView b;
    REQUIRE(r.next(b));

    const uint64_t lastQpc = r.lastPublishQpc();

    // qpcFrequency 10 MHz, staleAfterMs 100 (the floor): stale from exactly
    // 1'000'000 ticks on. The boundary is inclusive.
    REQUIRE(r.header().qpcFrequency == 10'000'000);
    REQUIRE(r.staleAfterMs() == doctest::Approx(100.0));
    const uint64_t staleTicks = 1'000'000;
    CHECK(r.state(lastQpc + 1) == StreamState::Live);
    CHECK(r.state(lastQpc + staleTicks - 1) == StreamState::Live);
    CHECK(r.state(lastQpc + staleTicks) == StreamState::Stalled);

    // A caller's clock sampled just before the publish landed is not stale.
    CHECK(r.state(lastQpc - 1) == StreamState::Live);

    // Inactive, however stale -> Quiet, not Stalled.
    irtactile::shm::detail::releaseStore(prod.header().deviceState, static_cast<uint32_t>(DeviceState::Inactive));
    CHECK(r.state(lastQpc + staleTicks) == StreamState::Quiet);

    // Closed -> Closed regardless.
    irtactile::shm::detail::releaseStore(prod.header().deviceState, static_cast<uint32_t>(DeviceState::Closed));
    CHECK(r.state(lastQpc + staleTicks) == StreamState::Closed);
}

// ===========================================================================
// int16 conversion
// ===========================================================================
TEST_CASE("int16 conversion: the +/-32767 convention, rounding, and clamp rather than wrap") {
    // Full scale is 32767 both ways; -32768 is only reached by clamping.
    CHECK(floatToInt16(1.0f) == 32767);
    CHECK(floatToInt16(-1.0f) == -32767);
    CHECK(floatToInt16(0.0f) == 0);
    CHECK(int16ToFloat(32767) == 1.0f);
    CHECK(int16ToFloat(-32767) == -1.0f);
    CHECK(int16ToFloat(0) == 0.0f);

    // Round to nearest, symmetrically: 0.5 x 32767 = 16383.5.
    CHECK(floatToInt16(0.5f) == 16384);
    CHECK(floatToInt16(-0.5f) == -16384);

    // Within 1 LSB on the way back.
    for (const float v : {0.123f, -0.777f, 0.999f, -0.0001f}) {
        CAPTURE(v);
        CHECK(std::fabs(int16ToFloat(floatToInt16(v)) - v) <= 1.0f / 32767.0f);
    }

    // Beyond range: clamp, not wrap. A wrap would turn a loud transient into
    // full-scale noise of the opposite sign.
    CHECK(floatToInt16(2.0f) == 32767);
    CHECK(floatToInt16(-2.0f) == -32768);
    CHECK(floatToInt16(1e30f) == 32767);
    CHECK(floatToInt16(-1e30f) == -32768);
}

TEST_CASE("Round-trip over an int16 segment: every sample of every channel decodes to its value") {
    // 3 channels so a wrong channel stride cannot land on a plausible plane.
    TestProducer prod(LayoutParams{3, 4, 2, SampleFormat::Int16});
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    prod.publish();
    BlockView b;
    REQUIRE(r.next(b));
    REQUIRE(b.channelCount() == 3);
    REQUIRE(b.sampleCount == 4);

    // TestProducer encodes (i + ch * 1000) / 100000, so channel 0 sits within
    // a few LSB of zero and channel 2 near 0.02: a decoder that returned
    // zeros, mixed planes or got the scale wrong misses by far more than one
    // LSB.
    for (uint32_t ch = 0; ch < 3; ++ch) {
        for (uint32_t i = 0; i < 4; ++i) {
            CAPTURE(ch);
            CAPTURE(i);
            const float v = (static_cast<float>(i) + static_cast<float>(ch) * 1000.0f) / 100000.0f;
            CHECK(b.channel(ch)[i] == int16ToFloat(floatToInt16(v)));
        }
    }
    CHECK(b.channel(2)[0] == doctest::Approx(0.02f).epsilon(0.01));
}

// ===========================================================================
// Consumer never recomputes
// ===========================================================================
TEST_CASE("Consumer never recomputes: inflated slotPayloadOffset, slotBytes and channelPlaneBytes are honored") {
    // A hypothetical future producer whose descriptor grew and which pads
    // planes more generously than the formula would. Built by hand
    // (not via TestProducer, which always uses the tight formula) so the
    // header's own fields -- not any recomputation -- are what the reader
    // must follow. The padded plane also forces the reader off its
    // single-memcpy fast path onto the per-plane strided copy.
    LayoutParams p{2, 4, 2, SampleFormat::Float32};

    const uint32_t inflatedPayloadOffset = static_cast<uint32_t>(sizeof(ShmBlockDesc)) + 64;
    const uint32_t cpb                  = channelPlaneBytesOf(p) * 2;   // padded planes
    const uint32_t inflatedSlotBytes    = align64(inflatedPayloadOffset + p.channelCount * cpb);
    const uint32_t slotArrayOffset      = slotArrayOffsetOf();
    const uint64_t totalBytes =
        slotArrayOffset + static_cast<uint64_t>(p.ringBlocks) * inflatedSlotBytes;

    std::vector<uint8_t> bytes(static_cast<size_t>(totalBytes), 0);
    auto* h = reinterpret_cast<ShmStreamHeader*>(bytes.data());
    h->versionMajor      = kVersionMajor;
    h->versionMinor      = kVersionMinor;
    h->headerBytes       = static_cast<uint32_t>(sizeof(ShmStreamHeader));
    h->totalBytes        = static_cast<uint32_t>(totalBytes);
    h->sampleFormat      = static_cast<uint32_t>(p.sampleFormat);
    h->channelLayout     = static_cast<uint32_t>(ChannelLayout::Planar);
    h->channelCount      = p.channelCount;
    h->sampleRateMilliHz = 3'000'000;
    h->sourceSampleRate  = 48000;
    h->rateDivisor       = 16;
    h->samplesPerBlock   = p.samplesPerBlock;
    h->ringBlocks        = p.ringBlocks;
    h->bytesPerSample    = bytesPerSampleOf(p.sampleFormat);
    h->slotPayloadOffset = inflatedPayloadOffset;     // NOT sizeof(ShmBlockDesc)
    h->channelArrayOffset = 0;
    h->slotArrayOffset   = slotArrayOffset;
    h->slotBytes         = inflatedSlotBytes;         // NOT the tight formula
    h->channelPlaneBytes = cpb;
    h->qpcFrequency      = 10'000'000;
    h->deviceState       = static_cast<uint32_t>(DeviceState::Active);

    // Write one block using exactly the arithmetic a real producer uses:
    // slotAt/planeAt off the header's own (inflated) fields, nothing hardcoded.
    uint8_t* slot = slotAt(bytes.data(), *h, uint64_t{0});
    auto*    desc = reinterpret_cast<ShmBlockDesc*>(slot);
    for (uint32_t ch = 0; ch < h->channelCount; ++ch) {
        uint8_t* plane = planeAt(slot, *h, ch);
        for (uint32_t i = 0; i < h->samplesPerBlock; ++i) {
            const float v = static_cast<float>(i) + static_cast<float>(ch) * 1000.0f;
            std::memcpy(plane + static_cast<size_t>(i) * 4, &v, 4);
        }
    }
    desc->firstSampleIndex  = 0;
    desc->timestampQpc      = 1000;
    desc->flags             = 0;
    desc->silentChannelMask = 0;
    irtactile::shm::detail::releaseStore(desc->seq, uint64_t{0});
    irtactile::shm::detail::releaseStore(h->lastPublishQpc, desc->timestampQpc);
    irtactile::shm::detail::releaseStore(h->magic, kMagic);   // writeSeq still 0: attach first

    Reader r;
    REQUIRE(r.attach(bytes.data(), bytes.size()) == AttachResult::Ok);

    // Publish is now "observed": bump writeSeq after attach, as a real
    // producer would: attach() seeds the cursor at whatever writeSeq
    // already is, so this ordering is what makes the block "new").
    irtactile::shm::detail::releaseStore(h->writeSeq, uint64_t{1});

    BlockView b;
    REQUIRE(r.next(b));
    REQUIRE(b.sampleCount == 4);
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < 4; ++i) {
            CAPTURE(ch);
            CAPTURE(i);
            CHECK(b.channel(ch)[i] == static_cast<float>(i) + static_cast<float>(ch) * 1000.0f);
        }
    }
}
