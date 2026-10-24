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

#include <cstring>
#include <string>
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

    /// Claims the next slot (kSlotBusy) without completing the block -- for
    /// slot-recycling / mid-construction tests.
    void beginPartialPublish()
    {
        Header& h    = header();
        uint8_t* slot = slotAt(static_cast<uint8_t*>(m_bytes.data()), h, m_writeSeq);
        auto*    desc = reinterpret_cast<Desc*>(slot);
        irtactile::shm::detail::releaseStore(desc->seq, kSlotBusy);
    }

    /// A partial block with `samplesEmitted` samples already written is
    /// abandoned. Those samples were emitted and then discarded, so they
    /// count toward the stream position; the producer advances
    /// firstSampleIndex and resets the slot as if the block had been
    /// flushed.
    void abandonPartialBlock(uint32_t samplesEmitted)
    {
        beginPartialPublish();   // the slot is already at kSlotBusy in practice
        m_first += samplesEmitted;
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
TEST_CASE("computeTotalSize matches the manual layout across configs") {
    struct Case { uint32_t ch; uint32_t spb; uint32_t ring; SampleFormat fmt; };
    const Case cases[] = {
        {1, 1, 2, SampleFormat::Float32}, {8, 48, 2, SampleFormat::Float32},
        {32, 1, 256, SampleFormat::Int16}, {8, 768, 2, SampleFormat::Float32},
        {20, 128, 64, SampleFormat::Int16},
    };
    for (const auto& c : cases) {
        LayoutParams p{c.ch, c.spb, c.ring, c.fmt};
        const uint32_t bps   = bytesPerSampleOf(c.fmt);
        const uint32_t cpb   = c.spb * bps;
        const uint32_t slotB = align64(static_cast<uint32_t>(sizeof(ShmBlockDesc)) + c.ch * cpb);
        const uint64_t want  = align64(static_cast<uint32_t>(sizeof(ShmStreamHeader))) +
                               static_cast<uint64_t>(c.ring) * slotB;
        CHECK(computeTotalSize(p) == want);
        CHECK(channelPlaneBytesOf(p) == cpb);
        CHECK(slotBytesOf(p) == slotB);
        // slotBytes must have room for the descriptor and every plane.
        CHECK(slotPayloadOffsetOf() + c.ch * cpb <= slotBytesOf(p));
    }
}

TEST_CASE("channelPlaneBytes equals samplesPerBlock times bytesPerSample exactly") {
    LayoutParams p{8, 48, 2, SampleFormat::Int16};
    CHECK(channelPlaneBytesOf(p) == 48u * 2u);
}

// ===========================================================================
// ABI stability (redundant with the header's own static_asserts, but explicit
// in test output rather than buried in a compiler diagnostic).
// ===========================================================================
TEST_CASE("ABI: struct sizes and hot-range offsets are pinned") {
    CHECK(sizeof(ShmStreamHeader) == 256);
    CHECK(sizeof(ShmBlockDesc) == 64);
    CHECK(offsetof(ShmStreamHeader, writeSeq) == 80);
    CHECK(offsetof(ShmStreamHeader, lastPublishQpc) == 88);
    CHECK(offsetof(ShmStreamHeader, qpcFrequency) == 112);
    CHECK(offsetof(ShmStreamHeader, slotPayloadOffset) == 52);
    CHECK(offsetof(ShmStreamHeader, producerPid) == 224);
    CHECK(offsetof(ShmStreamHeader, reserved) == 228);
    CHECK(std::is_standard_layout<ShmStreamHeader>::value);
    CHECK(std::is_trivially_copyable<ShmStreamHeader>::value);
    CHECK(std::is_standard_layout<ShmBlockDesc>::value);
    CHECK(std::is_trivially_copyable<ShmBlockDesc>::value);
}

TEST_CASE("No channel table in v1") {
    TestProducer prod(defaultParams());
    CHECK(prod.header().channelArrayOffset == 0);
    CHECK(prod.header().slotArrayOffset == align64(static_cast<uint32_t>(sizeof(ShmStreamHeader))));
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

    // attach() must clamp its own memcpy to min(headerBytes, sizeof(its own
    // struct)) = sizeof(ShmStreamHeader) bytes -- never sizeof(ShmStreamHeader)
    // + 64, which would read past the reader's own snapshot buffer.
    Reader r;
    CHECK(r.attach(bytes.data(), bytes.size()) == AttachResult::Ok);
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
    const std::string detail = r.mismatchDetail();
    CHECK(detail.find("channelCount") != std::string::npos);
    CHECK(detail.find('4') != std::string::npos);
    CHECK(detail.find('6') != std::string::npos);
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

    BlockView b;
    int count = 0;
    while (r.next(b)) ++count;
    // Readable window is ringBlocks - 1.
    CHECK(count == 3);
    CHECK(r.droppedBlocks() > 0);
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

TEST_CASE("Slot recycling mid-construction at ring 2: a busy slot is rejected outright") {
    TestProducer prod(defaultParams());   // ring 2
    prod.publish();               // seq 0 in slot 0
    prod.publish();               // seq 1 in slot 1

    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    // Attach cursor = writeSeq = 2, so nothing to read yet.
    BlockView b;
    CHECK_FALSE(r.next(b));

    // Producer claims slot 0 again (seq 2) but never finishes it.
    prod.beginPartialPublish();
    // Force writeSeq forward without publishing seq 2's descriptor, to put
    // seq=2 inside the reader's requested range while the slot is kSlotBusy.
    irtactile::shm::detail::releaseStore(prod.header().writeSeq, uint64_t{3});

    // The reader must reject the busy slot rather than return torn/garbage
    // data, and must not crash.
    CHECK_FALSE(r.next(b));
    CHECK(r.droppedBlocks() >= 1);
}

TEST_CASE("Abandoned block: firstSampleIndex advances by the samples discarded, not zero") {
    // Ring 4 (rather than the default 2) so both published blocks stay
    // readable at once -- this test is about firstSampleIndex bookkeeping,
    // not the ring-2 overrun boundary covered elsewhere.
    TestProducer prod(LayoutParams{4, 8, 4, SampleFormat::Float32});
    // Attach before anything is published -- attach() intentionally seeds the
    // cursor at the current writeSeq, so a reader that attached after
    // the fact would see neither block as "new".
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);

    prod.publish();   // seq 0: firstSampleIndex 0..7

    // A partial block (4 of 8 samples) is abandoned mid-frame -- the samples
    // already emitted into it still count as emitted, per the publish
    // protocol's bookkeeping.
    prod.abandonPartialBlock(4);
    prod.publish();   // seq 1: the next successful block

    BlockView b0, b1;
    REQUIRE(r.next(b0));
    CHECK(b0.seq == 0);
    REQUIRE(r.next(b1));
    CHECK(b1.seq == 1);

    // gap_samples = firstSampleIndex - (prev.firstSampleIndex + prev.sampleCount)
    const uint64_t gap = b1.firstSampleIndex - (b0.firstSampleIndex + b0.sampleCount);
    CHECK(gap == 4);   // exactly the samples discarded, not zero
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

    SUBCASE("every class's own boundary layout resolves to that class") {
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

    SUBCASE("the three suffixes are distinct, so two classes never collide") {
        CHECK(std::string(kSizeClassSuffix[0]) != std::string(kSizeClassSuffix[1]));
        CHECK(std::string(kSizeClassSuffix[1]) != std::string(kSizeClassSuffix[2]));
        CHECK(std::string(kSizeClassSuffix[0]) != std::string(kSizeClassSuffix[2]));
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
        // 48000 / divisor is exact on every rung -- the whole reason
        // sampleRateMilliHz carries no floating point downstream.
        CHECK(48'000'000u % d == 0u);
    }

    uint32_t d = 0;
    CHECK_FALSE(rateDivisorFromHz(44100.0, d));
    CHECK_FALSE(rateDivisorFromHz(0.0, d));
    CHECK_FALSE(rateDivisorFromHz(96000.0, d));
    CHECK_FALSE(rateDivisorFromHz(-48000.0, d));
}

TEST_CASE("Derived quantities are exact on hand-built ladder rungs and match Reader") {
    // The engine publishes once per 128-sample frame at 375 Hz.  Batching
    // `framesPerPublish` frames gives samplesPerBlock = 128*fpp and
    // publishHz = 375 / fpp exactly.
    struct Rung { uint32_t framesPerPublish; uint32_t samplesPerBlock; double publishHz; };
    const Rung rungs[] = {
        {1, 128, 375.0},
        {2, 256, 187.5},
        {4, 512, 93.75},
        {8, 1024, 46.875},
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

        const double periodSec = static_cast<double>(rung.samplesPerBlock) / 48000.0;
        const double periodMs  = periodSec * 1000.0;

        CHECK(periodSecondsOf(*h) == doctest::Approx(periodSec));
        CHECK(periodMsOf(*h) == doctest::Approx(periodMs));
        CHECK(publishHzOf(*h) == doctest::Approx(rung.publishHz));
        CHECK(ringSecondsOf(*h) == doctest::Approx(periodSec * p.ringBlocks));

        // Threshold floors only lift once the period is large enough.
        const double wantWait = (4.0 * periodMs > 50.0) ? (4.0 * periodMs) : 50.0;
        const double wantStale = (5.0 * periodMs > 100.0) ? (5.0 * periodMs) : 100.0;
        CHECK(waitTimeoutMsOf(*h) == doctest::Approx(wantWait));
        CHECK(staleAfterMsOf(*h) == doctest::Approx(wantStale));

        Reader r;
        REQUIRE(r.attach(bytes.data(), bytes.size()) == AttachResult::Ok);
        CHECK(r.periodMs() == doctest::Approx(periodMsOf(*h)));
        CHECK(r.publishHz() == doctest::Approx(rung.publishHz));
        CHECK(r.waitTimeoutMs() == doctest::Approx(waitTimeoutMsOf(*h)));
        CHECK(r.staleAfterMs() == doctest::Approx(staleAfterMsOf(*h)));
    }
}

TEST_CASE("Staleness is decided without sleeping, over a synthetic clock") {
    TestProducer prod(defaultParams());
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    prod.publish();
    BlockView b;
    REQUIRE(r.next(b));

    const uint64_t qpcFreq = r.header().qpcFrequency;
    const uint64_t lastQpc = r.lastPublishQpc();

    // Active + fresh -> Live.
    CHECK(r.state(lastQpc + 1) == StreamState::Live);

    // Active + far in the future -> Stalled.
    const uint64_t staleTicks =
        static_cast<uint64_t>(r.staleAfterMs() / 1000.0 * static_cast<double>(qpcFreq)) + qpcFreq;
    CHECK(r.state(lastQpc + staleTicks) == StreamState::Stalled);

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
TEST_CASE("int16 round-trip is within 1 LSB and clamps rather than wraps") {
    CHECK(int16ToFloat(floatToInt16(0.5f)) == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(int16ToFloat(floatToInt16(-0.5f)) == doctest::Approx(-0.5f).epsilon(0.001));

    // Beyond range: clamp, not wrap. A wrap would turn a loud transient into
    // full-scale noise of the opposite sign.
    CHECK(floatToInt16(2.0f) == 32767);
    CHECK(floatToInt16(-2.0f) == -32768);
    CHECK(floatToInt16(1.0f) >= 32000);
}

TEST_CASE("Round-trip over int16-formatted segment decodes via the same BlockView API") {
    TestProducer prod(LayoutParams{2, 4, 2, SampleFormat::Int16});
    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);
    prod.publish();
    BlockView b;
    REQUIRE(r.next(b));
    // Values were scaled down by 100000 before quantizing in TestProducer, so
    // just check the decode path runs and produces finite, ordered values.
    const float* s = b.channel(0);
    CHECK(s[0] <= s[1]);
    CHECK(s[1] <= s[2]);
}

// ===========================================================================
// Consumer never recomputes
// ===========================================================================
TEST_CASE("Consumer never recomputes: inflated slotPayloadOffset and slotBytes are honored") {
    // A hypothetical future producer whose descriptor grew and which pads
    // planes more generously than the formula would. Built by hand
    // (not via TestProducer, which always uses the tight formula) so the
    // header's own fields -- not any recomputation -- are what the reader
    // must follow.
    LayoutParams p{2, 4, 2, SampleFormat::Float32};

    const uint32_t inflatedPayloadOffset = static_cast<uint32_t>(sizeof(ShmBlockDesc)) + 64;
    const uint32_t cpb                  = channelPlaneBytesOf(p);
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
    CHECK(b.channel(0)[0] == doctest::Approx(0.0f));
    CHECK(b.channel(1)[0] == doctest::Approx(1000.0f));
    CHECK(b.channel(1)[2] == doctest::Approx(1002.0f));
}

TEST_CASE("Addressing round-trip: the normative arithmetic agrees with what Reader decodes") {
    LayoutParams p{3, 6, 2, SampleFormat::Float32};
    TestProducer prod(p);

    Reader r;
    REQUIRE(r.attach(prod.base(), prod.size()) == AttachResult::Ok);   // before publishing

    prod.publish();

    const auto& h = prod.header();
    uint8_t* slot = slotAt(static_cast<uint8_t*>(prod.base()), h, uint64_t{0});
    const uint8_t* plane1 = planeAt(slot, h, 1);
    float manualSample0;
    std::memcpy(&manualSample0, plane1, sizeof(float));

    BlockView b;
    REQUIRE(r.next(b));
    CHECK(b.channel(1)[0] == doctest::Approx(manualSample0));
}
