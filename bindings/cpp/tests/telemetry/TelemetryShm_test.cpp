// Unit tests for the telemetry-shm v1 binding (spec/telemetry-shm/v1): layout
// arithmetic, header validation, the producer/reader round trip over a plain
// buffer, layout rewrites, events and the return region.

#include <doctest/doctest.h>

#include <irtactile/shm/telemetry.h>

#include <cstring>
#include <string>
#include <vector>

using namespace irtactile::shm::telemetry;
namespace channels = irtactile::shm::channels;

namespace {

// 8-byte aligned storage, like a mapped segment.
struct Segment {
    explicit Segment(size_t bytes) : words((bytes + 7) / 8, 0), size(bytes) {}
    void* data() { return words.data(); }
    uint8_t* bytes() { return reinterpret_cast<uint8_t*>(words.data()); }
    Header& header() { return *reinterpret_cast<Header*>(words.data()); }
    std::vector<uint64_t> words;
    size_t size;
};

Identity identity(const char* simId = "demo")
{
    Identity id;
    id.simId = simId;
    id.simName = "Demo Sim";
    id.qpcFrequency = 10'000'000;
    id.pid = 4242;
    id.nominalRateMilliHz = 100'000;   // 100 Hz
    return id;
}

}  // namespace

TEST_CASE("header, descriptor and return header have the specified size and offsets")
{
    CHECK(sizeof(Header) == 256);
    CHECK(sizeof(ChannelDesc) == 128);
    CHECK(sizeof(ReturnHeader) == 128);
    CHECK(offsetof(Header, capacity) == 16);
    CHECK(offsetof(Header, qpcFrequency) == 40);
    CHECK(offsetof(Header, simId) == 56);
    CHECK(offsetof(Header, frameSeq) == 80);
    CHECK(offsetof(Header, lastPublishQpc) == 88);
    CHECK(offsetof(Header, producerState) == 96);
    CHECK(offsetof(Header, layoutSeq) == 100);
    CHECK(offsetof(Header, frameSeqEnd) == 104);
    CHECK(offsetof(Header, frameQpc) == 112);
    CHECK(offsetof(Header, carId) == 152);
    CHECK(offsetof(ChannelDesc, kind) == 88);
    CHECK(offsetof(ChannelDesc, arrayCapacity) == 98);
    CHECK(offsetof(ChannelDesc, provenance) == 100);
    CHECK(offsetof(ChannelDesc, layout) == 101);
    CHECK(offsetof(Header, wheelsPerAxle) == 248);
}

TEST_CASE("layoutFor keeps every array 8-byte aligned and inside totalBytes")
{
    for (uint32_t cap : {64u, 128u, 256u, 4096u, 8192u}) {
        const Layout l = layoutFor(cap);
        CHECK(l.descOffset == 256);
        CHECK(l.descOffset % 8 == 0);
        CHECK(l.validOffset % 8 == 0);
        CHECK(l.eventCountOffset % 8 == 0);
        CHECK(l.valuesOffset % 8 == 0);
        CHECK(l.valuesOffset + cap * 4 == l.totalBytes);
        // Each array ends where the next begins: no overlap, no hole.
        CHECK(l.descOffset + cap * sizeof(ChannelDesc) == l.validOffset);
        CHECK(l.validOffset + cap / 8 == l.eventCountOffset);
        CHECK(l.eventCountOffset + cap * 4 == l.valuesOffset);
    }
    CHECK(isValidCapacity(64));
    CHECK_FALSE(isValidCapacity(0));
    CHECK_FALSE(isValidCapacity(100));
    CHECK_FALSE(isValidCapacity(8256));
    CHECK(returnDemandedOffset(kMaxCapacity) + kMaxCapacity / 8 <= kReturnBytes);
}

TEST_CASE("segment names")
{
    CHECK(inboundName("demo") == "irTactile_Telemetry_demo");
    CHECK(returnNameFor("irTactile_Telemetry_demo") == "irTactile_TelemetryReturn_demo");
    CHECK(returnNameFor("MySegment") == "MySegment_Return");
    CHECK(returnNameFor("irTactile_Telemetry_") == "irTactile_TelemetryReturn_");
}

TEST_CASE("eventsSince is a modular difference")
{
    CHECK(eventsSince(5, 5) == 0);
    CHECK(eventsSince(5, 7) == 2);
    CHECK(eventsSince(0xFFFFFFFFu, 1u) == 2);
}

TEST_CASE("a fresh segment is NotReady until the first commitLayout")
{
    Segment s(Producer::requiredBytes(64));
    Reader r;
    CHECK(r.attach(s.data(), s.size) == AttachResult::NotReady);

    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    p.declare(channels::standard::Id::EngineRpm);
    CHECK(r.attach(s.data(), s.size) == AttachResult::NotReady);   // still staged
    REQUIRE(p.commitLayout());
    CHECK(r.attach(s.data(), s.size) == AttachResult::Ok);
    CHECK(r.header().layoutSeq % 2 == 0);
    CHECK(r.header().channelCount == 1);
    CHECK(std::string(r.descriptors()[0].name) == "std.engine.rpm");
}

TEST_CASE("table declarations copy the table properties; arrays declare every element")
{
    Segment s(Producer::requiredBytes(128));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 128, identity()) == OpenResult::Ok);
    const int rpm = p.declare(channels::standard::Id::EngineRpm);
    const int arr = p.declareArray(channels::standard::ArrayId::WheelLoad);
    const int local = p.declareLocal("demo_wave", "", -1.f, 1.f, channels::Kind::Continuous);
    REQUIRE(p.commitLayout());
    CHECK(rpm == 0);
    CHECK(arr == 1);
    CHECK(local == 1 + 8);

    Reader r;
    REQUIRE(r.attach(s.data(), s.size) == AttachResult::Ok);
    const auto& d = r.descriptors();
    CHECK(matchesTable(d[rpm], channels::standard::kChannels[static_cast<uint32_t>(channels::standard::Id::EngineRpm)], 0));
    CHECK(std::string(d[arr + 3].name) == "std.wheel.load[3]");
    CHECK(d[arr + 3].arrayIndex == 3);
    CHECK(d[arr + 3].arrayCapacity == 8);
    CHECK(matchesTable(d[arr + 3], channels::standard::kArrays[static_cast<uint32_t>(channels::standard::ArrayId::WheelLoad)], 3));
    CHECK(std::string(d[local].name) == "demo_wave");
}

TEST_CASE("declareLocal rejects invalid names; declarations stop at capacity")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    CHECK(p.declareLocal("", "", 0, 1, channels::Kind::Continuous) == Producer::kInvalidSlot);
    CHECK(p.declareLocal("Bad.name", "", 0, 1, channels::Kind::Continuous) == Producer::kInvalidSlot);
    CHECK(p.declareLocal("a b", "", 0, 1, channels::Kind::Continuous) == Producer::kInvalidSlot);
    CHECK(p.declareLocal("pump[3]", "bar", 0, 1, channels::Kind::Continuous) == 0);
    for (int i = 1; i < 64; ++i) {
        CHECK(p.declareLocal("c" + std::to_string(i), "", 0, 1, channels::Kind::Continuous) == i);
    }
    CHECK(p.declareLocal("overflow", "", 0, 1, channels::Kind::Continuous) == Producer::kInvalidSlot);
}

TEST_CASE("LocalChannel declarations, local arrays and SlotRange")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);

    LocalChannel pump;
    pump.name = "pump";
    pump.unit = "bar";
    pump.rangeMax = 10.f;
    pump.kind = channels::Kind::Continuous;
    pump.provenance = channels::Provenance::Derived;
    const int pumpSlot = p.declareLocal(pump);

    const SlotRange load = p.declareArrayRange(channels::standard::ArrayId::WheelLoad, 2);
    LocalChannel wheelSpeed;
    wheelSpeed.name = "wheel_speed";
    wheelSpeed.unit = "m/s";
    const SlotRange speed = p.declareLocalArray(wheelSpeed, 2, channels::ArrayLayout::Wheel);
    REQUIRE(p.commitLayout());

    CHECK(pumpSlot == 0);
    CHECK(load.isValid());
    CHECK(load[0] == 1);
    CHECK(load[1] == 2);
    CHECK(load[2] == kInvalidSlot);
    CHECK(speed[0] == 3);
    CHECK(speed[1] == 4);

    Reader r;
    REQUIRE(r.attach(s.data(), s.size) == AttachResult::Ok);
    const auto& d = r.descriptors();
    CHECK(provenanceOf(d[pumpSlot]) == channels::Provenance::Derived);
    CHECK(kindOf(d[pumpSlot]) == channels::Kind::Continuous);
    CHECK(std::string(d[load[1]].name) == "std.wheel.load[1]");
    CHECK(d[load[1]].arrayCapacity == 2);
    CHECK(std::string(d[speed[1]].name) == "wheel_speed[1]");
    CHECK(d[speed[1]].arrayIndex == 1);
    CHECK(d[speed[1]].arrayCapacity == 2);
    CHECK(layoutOf(d[speed[1]]) == channels::ArrayLayout::Wheel);
    CHECK(validateDescriptor(d[speed[1]]));
}

TEST_CASE("declareLocalArray is all or nothing")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    LocalChannel bad;
    bad.name = "Bad.name";
    CHECK_FALSE(p.declareLocalArray(bad, 2).isValid());
    LocalChannel tooMany;
    tooMany.name = "many";
    CHECK_FALSE(p.declareLocalArray(tooMany, 65).isValid());
    CHECK_FALSE(p.declareLocalArray(tooMany, 0).isValid());
    CHECK(p.channelCount() == 0);
    CHECK_FALSE(SlotRange{}.isValid());
    CHECK(SlotRange{}[0] == kInvalidSlot);
}

TEST_CASE("Frame::isValid reads the valid bits and is false outside the frame")
{
    Frame f;
    f.valid = {uint64_t{1} << 3, uint64_t{1}};
    CHECK(f.isValid(3));
    CHECK_FALSE(f.isValid(4));
    CHECK(f.isValid(64));
    CHECK_FALSE(f.isValid(128));
}

TEST_CASE("frames round-trip values, valid bits, events and QPC")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    const int rpm = p.declare(channels::standard::Id::EngineRpm);
    const int gear = p.declare(channels::standard::Id::DrivetrainGear);
    const int bump = p.declareLocal("bump", "", 0, 1, channels::Kind::Event);
    REQUIRE(p.commitLayout());
    p.setState(ProducerState::Active, 1000);

    Reader r;
    REQUIRE(r.attach(s.data(), s.size) == AttachResult::Ok);
    Frame f;
    CHECK(r.readFrame(f, 0) == ReadResult::NoNewFrame);

    p.beginFrame();
    p.set(rpm, 6500.f);
    p.event(bump, 0.75f);
    p.endFrame(/*frameQpc*/ 2000, /*now*/ 2001);

    REQUIRE(r.readFrame(f, 0) == ReadResult::NewFrame);
    CHECK(f.frameSeq == 2);
    CHECK(f.frameQpc == 2000);
    CHECK(f.values[rpm] == 6500.f);
    CHECK(f.isValid(rpm));
    CHECK_FALSE(f.isValid(gear));                   // not written this frame
    CHECK(f.eventCount[bump] == 1);
    CHECK(f.values[bump] == 0.75f);
    CHECK(r.lastPublishQpc() == 2001);
    CHECK(r.producerState() == ProducerState::Active);
    CHECK(r.readFrame(f, f.frameSeq) == ReadResult::NoNewFrame);

    p.beginFrame();
    p.set(rpm, 7000.f);
    p.endFrame(3000, 3001);
    REQUIRE(r.readFrame(f, 2) == ReadResult::NewFrame);
    CHECK_FALSE(f.isValid(bump));                   // events are valid only in the frame that carries one
    CHECK(f.eventCount[bump] == 1);                 // the counter keeps counting
    CHECK(f.values[bump] == 0.75f);                // retained even when validity is clear
}

TEST_CASE("a frame being written is Torn, never a mix")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    p.declare(channels::standard::Id::EngineRpm);
    REQUIRE(p.commitLayout());
    Reader r;
    REQUIRE(r.attach(s.data(), s.size) == AttachResult::Ok);
    p.beginFrame();   // frameSeq odd, never ended
    Frame f;
    CHECK(r.readFrame(f, 0) == ReadResult::Torn);
}

TEST_CASE("a Torn read keeps the previous frame")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    const int rpm = p.declare(channels::standard::Id::EngineRpm);
    REQUIRE(p.commitLayout());
    Reader r;
    REQUIRE(r.attach(s.data(), s.size) == AttachResult::Ok);

    p.beginFrame();
    p.set(rpm, 6500.f);
    p.endFrame(2000, 2001);
    Frame f;
    REQUIRE(r.readFrame(f, 0) == ReadResult::NewFrame);
    const uint64_t seq = f.frameSeq;

    p.beginFrame();
    p.set(rpm, 7000.f);
    p.endFrame(3000, 3001);
    s.header().frameSeqEnd -= 2;   // even frameSeq, mismatching end: the copy is taken, then rejected
    CHECK(r.readFrame(f, seq) == ReadResult::Torn);
    CHECK(f.frameSeq == seq);
    CHECK(f.frameQpc == 2000);
    CHECK(f.values[rpm] == 6500.f);
    CHECK(f.isValid(rpm));

    s.header().frameSeqEnd += 2;
    REQUIRE(r.readFrame(f, seq) == ReadResult::NewFrame);
    CHECK(f.values[rpm] == 7000.f);
}

TEST_CASE("layout rewrite: readers see LayoutChanged, then the new descriptors")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    p.declare(channels::standard::Id::EngineRpm);
    p.setCar("car_a", "Car A");
    REQUIRE(p.commitLayout());
    Reader r;
    REQUIRE(r.attach(s.data(), s.size) == AttachResult::Ok);
    CHECK(std::string(r.header().carId) == "car_a");
    const uint32_t seq0 = r.snapshotLayoutSeq();

    p.setCar("car_b", "Car B");      // committed -> layout rewrite
    Frame f;
    CHECK(r.readFrame(f, 0) == ReadResult::LayoutChanged);
    CHECK(r.refreshLayout() == AttachResult::Ok);
    CHECK(r.snapshotLayoutSeq() > seq0);
    CHECK(r.snapshotLayoutSeq() % 2 == 0);
    CHECK(std::string(r.header().carId) == "car_b");
    CHECK(std::string(r.header().carName) == "Car B");
}

TEST_CASE("strings are truncated to the field without splitting a UTF-8 sequence")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    const std::string x30(30, 'x');
    p.setCar("car", x30 + "\xC3\xA9");   // 32 bytes; the field holds 31 + NUL, byte 31 would split the 'é'
    REQUIRE(p.commitLayout());
    CHECK(std::string(s.header().carName) == x30);
}

TEST_CASE("a layout rewrite that crashed mid-way leaves an odd layoutSeq: NotReady, and a restart steps past it")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    p.declare(channels::standard::Id::EngineRpm);
    REQUIRE(p.commitLayout());
    Reader r;
    REQUIRE(r.attach(s.data(), s.size) == AttachResult::Ok);

    s.header().layoutSeq += 1;   // simulate a producer that died mid-rewrite
    CHECK(r.refreshLayout() == AttachResult::NotReady);
    const uint32_t odd = s.header().layoutSeq;
    CHECK(odd % 2 == 1);

    Producer q;                  // the restarted producer
    REQUIRE(q.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    q.declare(channels::standard::Id::EngineRpm);
    REQUIRE(q.commitLayout());
    CHECK(s.header().layoutSeq % 2 == 0);
    CHECK(s.header().layoutSeq > odd);
    CHECK(r.refreshLayout() == AttachResult::Ok);
}

TEST_CASE("a restart into an existing segment keeps the pinned fields and refuses a layout that no longer fits")
{
    Segment s(Producer::requiredBytes(64));
    {
        Producer p;
        REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
        p.declare(channels::standard::Id::EngineRpm);
        REQUIRE(p.commitLayout());
    }
    const uint32_t total = s.header().totalBytes;
    Producer q;
    REQUIRE(q.open(s.data(), s.size, 4096, identity()) == OpenResult::Ok);   // asks for more, gets what fits
    CHECK(q.capacity() == 64);
    REQUIRE(q.commitLayout());
    CHECK(s.header().totalBytes == total);
}

TEST_CASE("open rejects a segment that is too small and a bad simId")
{
    Segment tiny(100);
    Producer p;
    CHECK(p.open(tiny.data(), tiny.size, 64, identity()) == OpenResult::TooSmall);
    Segment s(Producer::requiredBytes(64));
    CHECK(p.open(s.data(), s.size, 64, identity("std")) == OpenResult::BadIdentity);
    CHECK(p.open(s.data(), s.size, 64, identity("Bad-Id")) == OpenResult::BadIdentity);
    CHECK(p.open(s.data(), s.size, 64, identity(std::string(24, 'a').c_str())) == OpenResult::BadIdentity);
    REQUIRE(p.open(s.data(), s.size, 64, identity(std::string(23, 'a').c_str())) == OpenResult::Ok);
    REQUIRE(p.commitLayout());
    CHECK(std::string(s.header().simId) == std::string(23, 'a'));
    CHECK(p.open(s.data(), s.size, 64, identity("lmu")) == OpenResult::Ok);   // a built-in simId is allowed
    CHECK(p.open(s.data(), s.size, 64, identity("")) == OpenResult::Ok);      // empty = chosen by the consumer
}

TEST_CASE("nominal rate: required at open, published in the layout, a change is a layout rewrite")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    for (uint32_t bad : {0u, 999u, 1'000'001u}) {
        Identity id = identity();
        id.nominalRateMilliHz = bad;
        CHECK(p.open(s.data(), s.size, 64, id) == OpenResult::BadRate);
        CHECK_FALSE(p.isOpen());
    }
    CHECK(std::string(toCString(OpenResult::BadRate)) == "BadRate");

    Identity id = identity();
    id.nominalRateMilliHz = 10'000;   // 10 Hz
    REQUIRE(p.open(s.data(), s.size, 64, id) == OpenResult::Ok);
    p.declare(channels::standard::Id::EngineRpm);
    REQUIRE(p.commitLayout());
    CHECK(s.header().nominalRateMilliHz == 10'000);
    CHECK(s.header().versionMinor == 1);

    Reader r;
    REQUIRE(r.attach(s.data(), s.size) == AttachResult::Ok);
    CHECK(r.staleAfterMs() == 500);

    const uint32_t seq = s.header().layoutSeq;
    CHECK_FALSE(p.setNominalRate(999));              // out of range: keeps the previous rate, no rewrite
    CHECK_FALSE(p.setNominalRate(1'000'001));
    CHECK(s.header().layoutSeq == seq);
    CHECK(p.setNominalRate(10'000));                 // unchanged: no rewrite
    CHECK(s.header().layoutSeq == seq);
    CHECK(p.setNominalRate(1'000));                  // 1 Hz
    CHECK(p.nominalRateMilliHz() == 1'000);
    CHECK(s.header().nominalRateMilliHz == 1'000);
    CHECK(s.header().layoutSeq == seq + 2);
    CHECK(r.staleAfterMs() == 500);                  // the snapshot keeps the old rate until the refresh
    REQUIRE(r.refreshLayout() == AttachResult::Ok);
    CHECK(r.staleAfterMs() == 5'000);
}

TEST_CASE("staleAfterMsFor: max(100 ms, 5 periods), 100 ms when unknown")
{
    CHECK(staleAfterMsFor(0) == 100);
    CHECK(staleAfterMsFor(1'000) == 5'000);          // 1 Hz
    CHECK(staleAfterMsFor(10'000) == 500);           // 10 Hz
    CHECK(staleAfterMsFor(50'000) == 100);           // 50 Hz: 5 periods = 100 ms
    CHECK(staleAfterMsFor(60'000) == 100);           // floor
    CHECK(staleAfterMsFor(1'000'000) == 100);        // 1 kHz
    CHECK(staleAfterMsFor(30'000) == 166);           // integer division
    CHECK_FALSE(isValidNominalRate(0));
    CHECK(isValidNominalRate(kMinNominalRateMilliHz));
    CHECK(isValidNominalRate(kMaxNominalRateMilliHz));
}

TEST_CASE("Reader::isStale follows the declared rate and only while Active")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    Identity id = identity();                        // qpcFrequency 10 MHz: 1 ms = 10'000 ticks
    id.nominalRateMilliHz = 10'000;                  // 10 Hz: stale after 500 ms
    REQUIRE(p.open(s.data(), s.size, 64, id) == OpenResult::Ok);
    REQUIRE(p.commitLayout());
    Reader r;
    REQUIRE(r.attach(s.data(), s.size) == AttachResult::Ok);

    const uint64_t t0 = 1'000'000;
    p.setState(ProducerState::Active, t0);
    CHECK_FALSE(r.isStale(t0 + 500 * 10'000));       // exactly at the threshold: not yet
    CHECK(r.isStale(t0 + 500 * 10'000 + 1));
    CHECK_FALSE(r.isStale(t0 - 1));                  // a clock behind the timestamp is not stale

    p.setState(ProducerState::Inactive, t0);
    CHECK_FALSE(r.isStale(t0 + 60'000 * 10'000));    // a stale timestamp means nothing when not Active
    p.setState(ProducerState::Closed, t0);
    CHECK_FALSE(r.isStale(t0 + 60'000 * 10'000));

    // A minor-0 producer left the field zero: the 100 ms fallback.
    s.header().nominalRateMilliHz = 0;
    s.header().layoutSeq += 2;
    REQUIRE(r.refreshLayout() == AttachResult::Ok);
    p.setState(ProducerState::Active, t0);
    CHECK(r.staleAfterMs() == 100);
    CHECK(r.isStale(t0 + 100 * 10'000 + 1));
}

TEST_CASE("validateHeader rejections")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    p.declare(channels::standard::Id::EngineRpm);
    REQUIRE(p.commitLayout());
    const Header good = s.header();
    REQUIRE(validateHeader(good, s.size) == AttachResult::Ok);

    auto with = [&](auto mutate) {
        Header h = good;
        mutate(h);
        return validateHeader(h, s.size);
    };
    CHECK(with([](Header& h) { h.magic = 0; }) == AttachResult::NotReady);
    CHECK(with([](Header& h) { h.magic = 0x12345678; }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { h.versionMajor = 2; }) == AttachResult::BadVersion);
    CHECK(with([](Header& h) { h.versionMinor = 9; }) == AttachResult::Ok);          // tolerated
    CHECK(with([](Header& h) { h.nominalRateMilliHz = 0; }) == AttachResult::Ok);           // unknown (minor 0)
    CHECK(with([](Header& h) { h.nominalRateMilliHz = 1000; }) == AttachResult::Ok);        // 1 Hz
    CHECK(with([](Header& h) { h.nominalRateMilliHz = 1000000; }) == AttachResult::Ok);     // 1 kHz
    CHECK(with([](Header& h) { h.nominalRateMilliHz = 999; }) == AttachResult::Malformed);  // rejected, not clamped
    CHECK(with([](Header& h) { h.nominalRateMilliHz = 1000001; }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { h.nominalRateMilliHz = 0xFFFFFFFFu; }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { h.headerBytes = 128; }) == AttachResult::Malformed);
    CHECK(with([&](Header& h) { h.totalBytes = static_cast<uint32_t>(s.size) + 8; }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { h.capacity = 100; }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { h.capacity = 16384; }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { h.channelCount = 65; }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { h.descOffset += 4; }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { h.descOffset = 64; }) == AttachResult::Malformed);    // inside the header
    CHECK(with([](Header& h) { h.valuesOffset = h.totalBytes - 4; }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { h.producerState = 3; }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { std::memcpy(h.simId, "std", 4); }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { std::memcpy(h.simId, "Bad-Id", 7); }) == AttachResult::Malformed);
    CHECK(with([](Header& h) { std::memset(h.carId, 'x', sizeof(h.carId)); }) == AttachResult::Malformed);
    CHECK(validateHeader(good, 64) == AttachResult::Malformed);                      // mapped smaller than totalBytes

    auto wheels = [&](std::initializer_list<uint8_t> w) {
        return with([&](Header& h) {
            std::memset(h.wheelsPerAxle, 0, sizeof(h.wheelsPerAxle));
            size_t i = 0;
            for (uint8_t n : w) h.wheelsPerAxle[i++] = n;
        });
    };
    CHECK(wheels({}) == AttachResult::Ok);                 // unknown
    CHECK(wheels({2, 2}) == AttachResult::Ok);
    CHECK(wheels({2, 4, 4}) == AttachResult::Ok);
    CHECK(wheels({1, 1, 1, 1, 1, 1, 1, 8}) == AttachResult::Ok);
    CHECK(wheels({0, 2}) == AttachResult::Malformed);      // gap
    CHECK(wheels({2, 2, 0, 2}) == AttachResult::Malformed);
    CHECK(wheels({9}) == AttachResult::Malformed);         // more than 8 wheels on an axle
}

TEST_CASE("return region round trip: masks, forLayoutSeq, consumer state")
{
    Segment ret(kReturnBytes);
    ReturnWriter w;
    REQUIRE(w.init(ret.data(), ret.size, 99));
    ReturnReader rr;
    REQUIRE(rr.attach(ret.data(), ret.size) == AttachResult::Ok);

    std::vector<uint64_t> accepted(1, 0b0111), demanded(1, 0b0101);
    w.publishMasks(64, 6, accepted.data(), demanded.data());
    w.setState(ConsumerState::Active);
    w.touch(555);

    ReturnView v;
    REQUIRE(rr.read(v));
    CHECK(v.capacity == 64);
    CHECK(v.forLayoutSeq == 6);
    CHECK(v.consumerState == ConsumerState::Active);
    CHECK(v.lastConsumeQpc == 555);
    CHECK(v.consumerPid == 99);
    CHECK(v.isAccepted(1));
    CHECK_FALSE(v.isAccepted(3));
    CHECK(v.isDemanded(2));
    CHECK_FALSE(v.isDemanded(1));

    w.setState(ConsumerState::Idle);
    REQUIRE(rr.read(v));
    CHECK(v.isDemanded(1));            // not Active: every accepted channel counts as demanded
    CHECK_FALSE(v.isDemanded(3));
}

TEST_CASE("return region: absent, torn and malformed")
{
    Segment ret(kReturnBytes);
    ReturnReader rr;
    CHECK(rr.attach(ret.data(), ret.size) == AttachResult::NotReady);   // created, header not yet written
    ReturnWriter w;
    REQUIRE(w.init(ret.data(), ret.size, 1));
    REQUIRE(rr.attach(ret.data(), ret.size) == AttachResult::Ok);

    auto* h = reinterpret_cast<ReturnHeader*>(ret.data());
    h->retSeq = 3;                                                       // writer mid-publish
    ReturnView v;
    CHECK_FALSE(rr.read(v));
    h->retSeq = 4;
    h->retSeqEnd = 4;
    CHECK(rr.read(v));

    h->consumerState = 9;
    CHECK_FALSE(rr.read(v));                                             // unknown live state: rejected on read too
    ReturnReader rr2;
    CHECK(rr2.attach(ret.data(), ret.size) == AttachResult::Malformed);
}

TEST_CASE("return region: a failed read keeps the previous view")
{
    Segment ret(kReturnBytes);
    ReturnWriter w;
    REQUIRE(w.init(ret.data(), ret.size, 7));
    ReturnReader rr;
    REQUIRE(rr.attach(ret.data(), ret.size) == AttachResult::Ok);

    std::vector<uint64_t> accepted(1, 0b0011), demanded(1, 0b0001);
    w.publishMasks(64, 4, accepted.data(), demanded.data());
    w.setState(ConsumerState::Active);
    ReturnView v;
    REQUIRE(rr.read(v));

    std::vector<uint64_t> accepted2(2, ~uint64_t{0}), demanded2(2, ~uint64_t{0});
    w.publishMasks(128, 6, accepted2.data(), demanded2.data());
    auto* h = reinterpret_cast<ReturnHeader*>(ret.data());
    h->retSeqEnd -= 2;                                                   // the masks are copied, then rejected
    CHECK_FALSE(rr.read(v));
    h->retSeqEnd += 2;
    h->consumerState = 9;                                                // the masks are copied, then rejected
    CHECK_FALSE(rr.read(v));

    CHECK(v.capacity == 64);
    CHECK(v.forLayoutSeq == 4);
    CHECK(v.accepted == accepted);
    CHECK(v.demanded == demanded);
    CHECK(v.consumerState == ConsumerState::Active);
}

TEST_CASE("return region: the offsets follow capacity, consumerPid is a live load")
{
    Segment ret(kReturnBytes);
    ReturnWriter w;
    REQUIRE(w.init(ret.data(), ret.size, 11));
    ReturnReader rr;
    REQUIRE(rr.attach(ret.data(), ret.size) == AttachResult::Ok);
    auto* h = reinterpret_cast<ReturnHeader*>(ret.data());

    std::vector<uint64_t> acc(8, ~uint64_t{0}), dem(8, 0x5);
    w.publishMasks(512, 2, acc.data(), dem.data());
    CHECK(h->demandedOffset == returnDemandedOffset(512));
    w.publishMasks(256, 4, acc.data(), dem.data());                      // producer restarted with a smaller capacity
    CHECK(h->demandedOffset == returnDemandedOffset(256));
    ReturnView v;
    REQUIRE(rr.read(v));
    CHECK(v.capacity == 256);
    CHECK(v.forLayoutSeq == 4);
    REQUIRE(v.demanded.size() == 4);
    CHECK(v.demanded[3] == 0x5);
    CHECK(v.consumerPid == 11);

    REQUIRE(w.init(ret.data(), ret.size, 12));                           // irTactile restart: the segment survives
    REQUIRE(rr.read(v));
    CHECK(v.consumerPid == 12);
    CHECK(v.capacity == 256);
}

TEST_CASE("return region: a consumer restart drops the previous consumer's state and masks")
{
    Segment ret(kReturnBytes);
    ReturnWriter w;
    REQUIRE(w.init(ret.data(), ret.size, 21));
    std::vector<uint64_t> accepted(1, 0b0111), demanded(1, 0b0001);
    w.publishMasks(64, 6, accepted.data(), demanded.data());
    w.setState(ConsumerState::Active);
    w.touch(555);

    ReturnWriter restarted;                                              // the first consumer crashed while Active
    REQUIRE(restarted.init(ret.data(), ret.size, 22));
    ReturnReader rr;
    REQUIRE(rr.attach(ret.data(), ret.size) == AttachResult::Ok);
    ReturnView v;
    REQUIRE(rr.read(v));
    CHECK(v.consumerState == ConsumerState::Idle);
    CHECK(v.lastConsumeQpc == 0);
    CHECK(v.forLayoutSeq == 0);                                          // matches no committed layout
    CHECK(v.capacity == 64);
    CHECK_FALSE(v.isAccepted(0));
    CHECK(v.consumerPid == 22);
    CHECK((reinterpret_cast<ReturnHeader*>(ret.data())->retSeq & 1u) == 0);
}

TEST_CASE("return region: a gone consumer counts as not Active")
{
    ReturnView v;
    v.consumerState = ConsumerState::Active;
    v.accepted = {0b0011};
    v.demanded = {0b0001};
    CHECK_FALSE(v.isDemanded(1));
    v.consumerGone = true;
    CHECK(v.isDemanded(1));
    CHECK_FALSE(v.isDemanded(2));                                        // still only accepted slots

    const uint64_t f = 10'000'000;
    CHECK_FALSE(isConsumerStale(5 * f, 5 * f + f, f));                   // exactly the threshold
    CHECK(isConsumerStale(5 * f, 5 * f + f + 1, f));
    CHECK(isConsumerStale(0, 5 * f, f));                                 // never consumed
    CHECK_FALSE(isConsumerStale(5 * f + 1, 5 * f, f));                   // stored after our clock read
    CHECK_FALSE(isConsumerStale(0, 5 * f, 0));
}

TEST_CASE("a closed producer ignores every call")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    const int rpm = p.declare(channels::standard::Id::EngineRpm);
    REQUIRE(p.commitLayout());
    p.close();
    CHECK_FALSE(p.isOpen());
    const Header before = s.header();
    p.setState(ProducerState::Active, 99);
    p.beginFrame();
    p.set(rpm, 1.0f);
    p.endFrame(1, 2);
    CHECK_FALSE(p.commitLayout());
    CHECK(std::memcmp(&before, &s.header(), sizeof(Header)) == 0);
}

TEST_CASE("the producer writes provenance, layout and the wheel layout")
{
    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    const int rpm = p.declare(channels::standard::Id::EngineRpm, channels::Provenance::Native);
    const int defl = p.declareArray(channels::standard::ArrayId::SuspensionDeflection, 4, channels::Provenance::Derived);
    const int third = p.declareArray(channels::standard::ArrayId::SuspensionThird, 2);
    const int local = p.declareLocal("demo_wave", "", -1.f, 1.f, channels::Kind::Continuous, channels::kFlagNone, channels::Provenance::Estimated);
    CHECK(p.declareArray(channels::standard::ArrayId::SuspensionThird, 5) == Producer::kInvalidSlot);   // above the table capacity
    p.setWheelLayout({2, 2});
    REQUIRE(p.commitLayout());

    Reader r;
    REQUIRE(r.attach(s.data(), s.size) == AttachResult::Ok);
    const auto& d = r.descriptors();
    CHECK(d[rpm].provenance == 1);
    CHECK(d[rpm].layout == 0);
    CHECK(d[defl].provenance == 2);
    CHECK(d[defl].layout == 1);
    CHECK(d[defl + 3].arrayCapacity == 4);
    CHECK(d[third].layout == 2);
    CHECK(d[third].provenance == 0);
    CHECK(d[local].provenance == 3);
    const uint8_t want[8] = {2, 2, 0, 0, 0, 0, 0, 0};
    CHECK(std::memcmp(r.header().wheelsPerAxle, want, 8) == 0);

    // A change after the first commit is a layout rewrite.
    const uint32_t seq = r.snapshotLayoutSeq();
    p.setWheelLayout({2, 4, 4});
    CHECK(r.refreshLayout() == AttachResult::Ok);
    CHECK(r.snapshotLayoutSeq() != seq);
    CHECK(r.header().wheelsPerAxle[1] == 4);
    p.setWheelLayout({9});   // invalid -> stored as unknown
    CHECK(r.refreshLayout() == AttachResult::Ok);
    CHECK(r.header().wheelsPerAxle[0] == 0);
}

TEST_CASE("validateDescriptor and matchesTable")
{
    namespace st = channels::standard;
    const channels::ChannelDef& defl = st::kArrays[static_cast<uint32_t>(st::ArrayId::SuspensionDeflection)];
    const channels::ChannelDef& rpm = st::kChannels[static_cast<uint32_t>(st::Id::EngineRpm)];

    Segment s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    const int e = p.declare(st::Id::EngineRpm);
    const int a4 = p.declareArray(st::ArrayId::SuspensionDeflection, 4);
    REQUIRE(p.commitLayout());
    Reader r;
    REQUIRE(r.attach(s.data(), s.size) == AttachResult::Ok);
    const ChannelDesc scalar = r.descriptors()[e];
    ChannelDesc d = r.descriptors()[a4 + 3];

    CHECK(validateDescriptor(scalar));
    CHECK(matchesTable(scalar, rpm, 0));
    CHECK(validateDescriptor(d));
    CHECK(matchesTable(d, defl, 3));             // capacity 4 against table 8
    CHECK_FALSE(matchesTable(d, defl, 2));       // wrong element
    ChannelDesc x = d;
    x.arrayCapacity = 8; x.arrayIndex = 7;
    CHECK(matchesTable(x, defl, 7));             // capacity 8, index 7
    x.arrayCapacity = 9;
    CHECK(validateDescriptor(x));
    CHECK_FALSE(matchesTable(x, defl, 7));       // above the table
    x = d; x.arrayCapacity = 0;
    CHECK_FALSE(matchesTable(x, defl, 3));
    x = d; x.arrayCapacity = 4; x.arrayIndex = 4;
    CHECK_FALSE(validateDescriptor(x));          // index not below capacity
    CHECK_FALSE(matchesTable(x, defl, 4));
    x = d; x.layout = 2;
    CHECK(validateDescriptor(x));
    CHECK_FALSE(matchesTable(x, defl, 3));       // layout mismatch
    x = d; x.layout = 3;
    CHECK_FALSE(validateDescriptor(x));
    x = scalar; x.layout = 1;
    CHECK_FALSE(validateDescriptor(x));          // a layout on a scalar
    x = scalar; x.provenance = 4;
    CHECK_FALSE(validateDescriptor(x));
    x = scalar; x.provenance = 3;
    CHECK(matchesTable(x, rpm, 0));              // provenance is not compared
    x = scalar; x.flags |= ~channels::kFlagOptional;
    CHECK(matchesTable(x, rpm, 0));              // reserved flag bits are ignored
    x.flags ^= channels::kFlagOptional;
    CHECK_FALSE(matchesTable(x, rpm, 0));        // the defined flag must still match
}


TEST_CASE("inbound names cannot alias return regions")
{
    CHECK(isValidSegmentName("MySegment"));
    CHECK(isValidSegmentName("irTactile_Telemetry_demo"));
    CHECK_FALSE(isValidSegmentName(""));
    CHECK_FALSE(isValidSegmentName("bad/name"));
    CHECK_FALSE(isValidSegmentName(std::string(201, 'a')));
    CHECK_FALSE(isValidSegmentName("MySegment_Return"));
    CHECK_FALSE(isValidSegmentName("irTactile_TelemetryReturn_demo"));
    CHECK_FALSE(isValidSegmentName(returnNameFor("MySegment")));
    CHECK_FALSE(isValidSegmentName(returnNameFor("irTactile_Telemetry_demo")));
}

TEST_CASE("producer rejects invalid buffers and capacities before accessing a header")
{
    Producer p;
    uint64_t tiny = 0;
    CHECK(p.open(nullptr, 4096, 64, identity()) == OpenResult::TooSmall);
    CHECK(p.open(&tiny, sizeof(tiny), 64, identity()) == OpenResult::TooSmall);
    Segment s(Producer::requiredBytes(64));
    CHECK(p.open(s.bytes() + 1, s.size - 1, 64, identity()) == OpenResult::TooSmall);
    for (uint32_t capacity : {0u, 63u, 65u, 8256u, UINT32_MAX}) {
        CHECK(p.open(s.data(), s.size, capacity, identity()) == OpenResult::TooSmall);
        CHECK_FALSE(p.commitLayout());
    }
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    REQUIRE(p.commitLayout());
    CHECK(p.open(&tiny, sizeof(tiny), 64, identity()) == OpenResult::TooSmall);
    CHECK_FALSE(p.commitLayout());
}

#ifdef _WIN32
#include <irtactile/shm/telemetry/win32.h>

TEST_CASE("ProducerSegment: a second live producer is WriterActive, the lock dies with its producer")
{
    const std::string name = "irTactile_Telemetry_test_writer_" + std::to_string(GetCurrentProcessId());
    ConsumerSegment consumer;   // keeps the mapping alive across producers, like irTactile
    {
        ProducerSegment first;
        REQUIRE(first.create(name, 64, identity()) == OpenResult::Ok);
        CHECK_FALSE(first.restarted());
        REQUIRE(consumer.open(name) == SegmentOpen::Ok);

        ProducerSegment second;
        CHECK(second.create(name, 64, identity()) == OpenResult::WriterActive);
        CHECK(first.create(name, 64, identity()) == OpenResult::Ok);   // re-creating releases its own lock first
    }
    ProducerSegment next;   // the first producer is gone, the mapping is not: a restart
    CHECK(next.create(name, 64, identity()) == OpenResult::Ok);
    CHECK(next.restarted());
}

TEST_CASE("ProducerSegment: a failed recreate leaves the Producer unbound, not on unmapped memory")
{
    const std::string pid = std::to_string(GetCurrentProcessId());
    const std::string a = "irTactile_Telemetry_test_recreate_a_" + pid;
    const std::string b = "irTactile_Telemetry_test_recreate_b_" + pid;
    ProducerSegment holder;
    REQUIRE(holder.create(b, 64, identity()) == OpenResult::Ok);

    ProducerSegment s;
    ReturnView rv;
    CHECK_FALSE(s.pollReturn(rv));                                       // never created
    REQUIRE(s.create(a, 64, identity()) == OpenResult::Ok);
    Producer& p = s.producer();
    const int rpm = p.declare(channels::standard::Id::EngineRpm);
    REQUIRE(p.commitLayout());

    CHECK(s.create(b, 64, identity()) == OpenResult::WriterActive);
    CHECK_FALSE(p.isOpen());
    p.setState(ProducerState::Active, qpcNow());                         // no-ops, not writes to the old view
    p.beginFrame();
    p.set(rpm, 1.0f);
    p.endFrame(qpcNow(), qpcNow());
    CHECK_FALSE(p.commitLayout());
    CHECK_FALSE(s.pollReturn(rv));
}

TEST_CASE("pollReturn: an Active consumer that is stale or whose process exited is gone")
{
    const std::string name = "irTactile_Telemetry_test_gone_" + std::to_string(GetCurrentProcessId());
    ProducerSegment s;
    REQUIRE(s.create(name, 64, identity()) == OpenResult::Ok);
    Producer& p = s.producer();
    p.declare(channels::standard::Id::EngineRpm);
    REQUIRE(p.commitLayout());
    const uint32_t layoutSeq = p.publishedHeader().layoutSeq;

    ReturnSegment ret;
    REQUIRE(ret.create(name));
    std::vector<uint64_t> accepted(1, 0b1), demanded(1, 0b0);
    ret.writer().publishMasks(64, layoutSeq, accepted.data(), demanded.data());
    ret.writer().setState(ConsumerState::Active);
    ret.writer().touch(qpcNow());

    ReturnView rv;
    REQUIRE(s.pollReturn(rv));
    CHECK_FALSE(rv.consumerGone);
    CHECK_FALSE(rv.isDemanded(0));

    ret.writer().touch(qpcNow() - 2 * qpcFrequency());
    REQUIRE(s.pollReturn(rv));
    CHECK(rv.consumerGone);
    CHECK(rv.isDemanded(0));

    ret.writer().setState(ConsumerState::Idle);
    REQUIRE(s.pollReturn(rv));
    CHECK_FALSE(rv.consumerGone);                                        // only an Active consumer can be gone

    CHECK_FALSE(detail::processGone(0));
    CHECK_FALSE(detail::processGone(GetCurrentProcessId()));
    wchar_t cmd[] = L"cmd.exe /c exit 0";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    WaitForSingleObject(pi.hProcess, INFINITE);
    CHECK(detail::processGone(pi.dwProcessId));                          // our handle keeps the pid from reuse
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

TEST_CASE("pollReturn: a consumer that died mid-publication (retSeq left odd) still ages into gone")
{
    const std::string name = "irTactile_Telemetry_test_oddret_" + std::to_string(GetCurrentProcessId());
    ProducerSegment s;
    REQUIRE(s.create(name, 64, identity()) == OpenResult::Ok);
    Producer& p = s.producer();
    p.declare(channels::standard::Id::EngineRpm);
    REQUIRE(p.commitLayout());

    ReturnSegment ret;
    REQUIRE(ret.create(name));
    std::vector<uint64_t> accepted(1, 0b1), demanded(1, 0b0);
    ret.writer().publishMasks(64, p.publishedHeader().layoutSeq, accepted.data(), demanded.data());
    ret.writer().setState(ConsumerState::Active);
    ret.writer().touch(qpcNow());

    ReturnView rv;
    REQUIRE(s.pollReturn(rv));
    CHECK_FALSE(rv.isDemanded(0));

    MappedView raw;   // the consumer crashes inside publishMasks(): retSeq stays odd
    REQUIRE(raw.open(detail::widen(returnNameFor(name)), true));
    auto* h = reinterpret_cast<ReturnHeader*>(raw.data());
    h->retSeq += 1;

    CHECK_FALSE(s.pollReturn(rv));                                       // no consistent view any more
    CHECK_FALSE(rv.consumerGone);                                        // but not yet stale
    CHECK_FALSE(rv.isDemanded(0));

    rv.lastConsumeQpc = qpcNow() - 2 * qpcFrequency();                   // time passes without a good read
    CHECK_FALSE(s.pollReturn(rv));
    CHECK(rv.consumerGone);
    CHECK(rv.isDemanded(0));                                             // every accepted slot, not the stale mask

    p.declare(channels::standard::Id::EngineThrottle);                   // a new layout while the region is stuck
    REQUIRE(p.commitLayout());
    CHECK_FALSE(s.pollReturn(rv));
    CHECK_FALSE(rv.isAccepted(0));                                       // the masks of the old layout are dropped
}
#endif
