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

// 8-byte aligned storage, like a mapped section.
struct Section {
    explicit Section(size_t bytes) : words((bytes + 7) / 8, 0), size(bytes) {}
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
    }
    CHECK(isValidCapacity(64));
    CHECK_FALSE(isValidCapacity(0));
    CHECK_FALSE(isValidCapacity(100));
    CHECK_FALSE(isValidCapacity(8256));
    CHECK(returnDemandedOffset(kMaxCapacity) + kMaxCapacity / 8 <= kReturnBytes);
}

TEST_CASE("section names")
{
    CHECK(inboundName("demo") == "irTactile_Telemetry_demo");
    CHECK(returnNameFor("irTactile_Telemetry_demo") == "irTactile_TelemetryReturn_demo");
    CHECK(returnNameFor("MySection") == "MySection_Return");
    CHECK(returnNameFor("irTactile_Telemetry_") == "irTactile_Telemetry__Return");
}

TEST_CASE("eventsSince is a modular difference")
{
    CHECK(eventsSince(5, 5) == 0);
    CHECK(eventsSince(5, 7) == 2);
    CHECK(eventsSince(0xFFFFFFFFu, 1u) == 2);
}

TEST_CASE("a fresh section is NotReady until the first commitLayout")
{
    Section s(Producer::requiredBytes(64));
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
    Section s(Producer::requiredBytes(128));
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
    Section s(Producer::requiredBytes(64));
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

TEST_CASE("frames round-trip values, valid bits, events and QPC")
{
    Section s(Producer::requiredBytes(64));
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
    CHECK((f.valid[0] >> rpm & 1u) == 1);
    CHECK((f.valid[0] >> gear & 1u) == 0);          // not written this frame
    CHECK(f.eventCount[bump] == 1);
    CHECK(f.values[bump] == 0.75f);
    CHECK(r.lastPublishQpc() == 2001);
    CHECK(r.producerState() == ProducerState::Active);
    CHECK(r.readFrame(f, f.frameSeq) == ReadResult::NoNewFrame);

    p.beginFrame();
    p.set(rpm, 7000.f);
    p.endFrame(3000, 3001);
    REQUIRE(r.readFrame(f, 2) == ReadResult::NewFrame);
    CHECK((f.valid[0] >> bump & 1u) == 0);          // events are valid only in the frame that carries one
    CHECK(f.eventCount[bump] == 1);                 // the counter keeps counting
}

TEST_CASE("a frame being written is Torn, never a mix")
{
    Section s(Producer::requiredBytes(64));
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

TEST_CASE("layout rewrite: readers see LayoutChanged, then the new descriptors")
{
    Section s(Producer::requiredBytes(64));
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
    Section s(Producer::requiredBytes(64));
    Producer p;
    REQUIRE(p.open(s.data(), s.size, 64, identity()) == OpenResult::Ok);
    const std::string x30(30, 'x');
    p.setCar("car", x30 + "\xC3\xA9");   // 32 bytes; the field holds 31 + NUL, byte 31 would split the 'é'
    REQUIRE(p.commitLayout());
    CHECK(std::string(s.header().carName) == x30);
}

TEST_CASE("a layout rewrite that crashed mid-way leaves an odd layoutSeq: NotReady, and a restart steps past it")
{
    Section s(Producer::requiredBytes(64));
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

TEST_CASE("a restart into an existing section keeps the pinned fields and refuses a layout that no longer fits")
{
    Section s(Producer::requiredBytes(64));
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

TEST_CASE("open rejects a section that is too small and a bad simId")
{
    Section tiny(100);
    Producer p;
    CHECK(p.open(tiny.data(), tiny.size, 64, identity()) == OpenResult::TooSmall);
    Section s(Producer::requiredBytes(64));
    CHECK(p.open(s.data(), s.size, 64, identity("std")) == OpenResult::BadIdentity);
    CHECK(p.open(s.data(), s.size, 64, identity("Bad-Id")) == OpenResult::BadIdentity);
    CHECK(p.open(s.data(), s.size, 64, identity("lmu")) == OpenResult::Ok);   // a built-in simId is allowed
    CHECK(p.open(s.data(), s.size, 64, identity("")) == OpenResult::Ok);      // empty = sourceId
}

TEST_CASE("validateHeader rejections")
{
    Section s(Producer::requiredBytes(64));
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
    CHECK(with([](Header& h) { h.reserved0 = 9; }) == AttachResult::Ok);
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
    Section ret(kReturnBytes);
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
    Section ret(kReturnBytes);
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
    ReturnReader rr2;
    CHECK(rr2.attach(ret.data(), ret.size) == AttachResult::Malformed);
}

TEST_CASE("the producer writes provenance, layout and the wheel layout")
{
    Section s(Producer::requiredBytes(64));
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

    Section s(Producer::requiredBytes(64));
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
}
