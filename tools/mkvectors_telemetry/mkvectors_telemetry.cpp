// mkvectors_telemetry - writes conformance/telemetry-shm/v1: golden segments plus manifest.json.
//
//   mkvectors_telemetry <output dir>
//
// Segment bytes come from the C++ producer (scripts) with byte surgery for hostile states; the
// expectations are literals in this file, written from the spec, not computed from the binding.

#include "telemetry_script.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using nlohmann::json;
using namespace irtactile::shm::telemetry;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kCap = 64;
const Layout kL = layoutFor(kCap);

std::vector<uint64_t> makeSegment(const json& script)
{
    std::vector<uint64_t> buf((kL.totalBytes + 7) / 8, 0);
    telemetry_script::run(script, buf);
    return buf;
}

void writeFile(const fs::path& p, const std::vector<uint64_t>& buf, size_t bytes)
{
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(bytes));
}

template <class T>
void poke(std::vector<uint64_t>& buf, size_t offset, T v)
{
    std::memcpy(reinterpret_cast<uint8_t*>(buf.data()) + offset, &v, sizeof(T));
}

json baseScript(bool withFrame = true)
{
    json s = json::array({
        {{"op", "open"}, {"capacity", kCap}, {"simId", "demo"}, {"simName", "Demo Sim"}, {"qpcFrequency", 10000000}, {"pid", 4242}, {"nominalRateMilliHz", 100000}},
        {{"op", "declare"}, {"name", "std.engine.rpm"}, {"provenance", 1}},  // 0
        {{"op", "declare"}, {"name", "std.vehicle.speed"}},                // 1
        {{"op", "declare"}, {"name", "std.drivetrain.gear"}},              // 2
        {{"op", "declareLocal"}, {"name", "demo_wave"}, {"min", -1.0}, {"max", 1.0}},                      // 3
        {{"op", "declareLocal"}, {"name", "demo_bump"}, {"min", 0.0}, {"max", 1.0}, {"kind", "event"}},    // 4
        {{"op", "declare"}, {"name", "std.input.throttle"}},               // 5
        {{"op", "declare"}, {"name", "std.wheel.load"}},                   // 6..13
        {{"op", "wheels"}, {"list", json::array({2, 2})}},
        {{"op", "car"}, {"id", "car_a"}, {"name", "Car A"}},
        {{"op", "commit"}},
        {{"op", "state"}, {"state", "active"}, {"now", 1000}},
    });
    if (withFrame) {
        s.push_back({{"op", "frame"}, {"frameQpc", 2000}, {"now", 2001},
                     {"set", {{"0", 6500.0}, {"1", 42.5}, {"2", 3.0}, {"3", -0.5}}}, {"event", {{"4", 0.75}}}});
    }
    return s;
}

struct Suite {
    fs::path dir;
    json cases = json::array();

    // A decode-side segment: file under decode/, case referencing it.
    void segment(const std::string& name, const std::string& op, const std::vector<uint64_t>& buf, size_t bytes,
                 json expect, json extra = json::object())
    {
        writeFile(dir / "decode" / (name + ".bin"), buf, bytes);
        json c = {{"name", name}, {"op", op}, {"file", "decode/" + name + ".bin"}, {"expect", std::move(expect)}};
        for (auto it = extra.begin(); it != extra.end(); ++it) c[it.key()] = it.value();
        cases.push_back(std::move(c));
    }
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: mkvectors_telemetry <output dir>\n");
        return 2;
    }
    Suite s;
    s.dir = argv[1];
    fs::create_directories(s.dir / "decode");
    fs::create_directories(s.dir / "encode");

    const size_t total = kL.totalBytes;
    const auto good = makeSegment(baseScript());
    // --- attach -------------------------------------------------------------
    {
        json ex = {{"attach", "Ok"}, {"capacity", 64}, {"channelCount", 14}, {"simId", "demo"}, {"simName", "Demo Sim"},
                   {"carId", "car_a"}, {"carName", "Car A"}, {"layoutSeq", 2}, {"nominalRateMilliHz", 100000},
                   {"staleAfterMs", 100}};
        ex["wheelsPerAxle"] = json::array({2, 2, 0, 0, 0, 0, 0, 0});
        ex["descNames"] = json::array({"std.engine.rpm", "std.vehicle.speed", "std.drivetrain.gear", "demo_wave",
                                       "demo_bump", "std.input.throttle", "std.wheel.load[0]", "std.wheel.load[1]"});
        ex["descs"] = json::array({
            {{"slot", 0}, {"unit", "rpm"}, {"kind", 0}, {"flags", 2}, {"arrayCapacity", 0}, {"provenance", 1}, {"layout", 0},
             {"valid", true}, {"tableMatch", true}},
            {{"slot", 2}, {"kind", 1}, {"provenance", 0}},
            {{"slot", 4}, {"kind", 2}, {"flags", 0}, {"valid", true}},
            {{"slot", 5}, {"flags", 2}},
            {{"slot", 7}, {"name", "std.wheel.load[1]"}, {"arrayIndex", 1}, {"arrayCapacity", 8}, {"flags", 2}, {"layout", 1},
             {"valid", true}, {"tableMatch", true}},
        });
        s.segment("attach_valid_basic", "attach", good, total, ex);
    }

    auto mutated = [&](auto mutate) {
        auto b = good;
        mutate(b);
        return b;
    };
    auto attach = [&](const std::string& name, const std::string& result, auto mutate, json extra = json::object()) {
        s.segment(name, "attach", mutated(mutate), total, {{"attach", result}}, std::move(extra));
    };

    attach("attach_magic_zero", "NotReady", [](auto& b) { poke<uint32_t>(b, 0, 0); });
    attach("attach_bad_magic", "Malformed", [](auto& b) { poke<uint32_t>(b, 0, 0x12345678); });
    attach("attach_higher_version_major", "BadVersion", [](auto& b) { poke<uint16_t>(b, 4, 2); });
    attach("attach_higher_version_minor_tolerated", "Ok", [](auto& b) { poke<uint16_t>(b, 6, 9); });
    // nominalRateMilliHz at offset 52 (minor 1; reserved and zeroed in minor 0).
    auto rate = [&](const std::string& name, uint32_t milliHz, uint32_t staleAfterMs) {
        s.segment(name, "attach", mutated([&](auto& b) { poke<uint32_t>(b, 52, milliHz); }), total,
                  {{"attach", "Ok"}, {"nominalRateMilliHz", milliHz}, {"staleAfterMs", staleAfterMs}});
    };
    rate("attach_rate_zero_unknown", 0, 100);
    rate("attach_rate_min_1hz", 1000, 5000);
    rate("attach_rate_10hz", 10000, 500);
    rate("attach_rate_30hz_integer_division", 30000, 166);
    rate("attach_rate_max_1khz", 1000000, 100);
    s.segment("attach_rate_unknown_minor0_segment", "attach",
              mutated([](auto& b) { poke<uint32_t>(b, 52, 0); poke<uint16_t>(b, 6, 0); }), total,
              {{"attach", "Ok"}, {"nominalRateMilliHz", 0}, {"staleAfterMs", 100}});
    attach("attach_rate_below_minimum", "Malformed", [](auto& b) { poke<uint32_t>(b, 52, 999); });
    attach("attach_rate_above_maximum", "Malformed", [](auto& b) { poke<uint32_t>(b, 52, 1000001); });
    attach("attach_rate_max_u32", "Malformed", [](auto& b) { poke<uint32_t>(b, 52, 0xFFFFFFFFu); });
    attach("attach_header_bytes_below_minimum", "Malformed", [](auto& b) { poke<uint32_t>(b, 8, 128); });
    attach("attach_total_bytes_exceeds_mapped", "Malformed", [](auto&) {}, {{"mappedBytes", total - 8}});
    attach("attach_capacity_not_multiple_of_64", "Malformed", [](auto& b) { poke<uint32_t>(b, 16, 100); });
    attach("attach_capacity_below_minimum", "Malformed", [](auto& b) { poke<uint32_t>(b, 16, 32); });
    attach("attach_capacity_above_maximum", "Malformed", [](auto& b) { poke<uint32_t>(b, 16, 8256); });
    attach("attach_channel_count_exceeds_capacity", "Malformed", [](auto& b) { poke<uint32_t>(b, 20, 65); });
    attach("attach_desc_offset_unaligned", "Malformed", [](auto& b) { poke<uint32_t>(b, 24, kL.descOffset + 4); });
    attach("attach_desc_offset_inside_header", "Malformed", [](auto& b) { poke<uint32_t>(b, 24, 64); });
    attach("attach_values_array_overruns", "Malformed", [](auto& b) { poke<uint32_t>(b, 36, kL.totalBytes - 4); });
    attach("attach_unknown_producer_state", "Malformed", [](auto& b) { poke<uint32_t>(b, 96, 3); });
    attach("attach_sim_id_reserved", "Malformed", [](auto& b) { std::memcpy(reinterpret_cast<uint8_t*>(b.data()) + 56, "std\0", 4); });
    attach("attach_sim_id_invalid_chars", "Malformed", [](auto& b) { std::memcpy(reinterpret_cast<uint8_t*>(b.data()) + 56, "Bad-Id\0", 7); });
    attach("attach_sim_id_without_nul", "Malformed", [](auto& b) { std::memset(reinterpret_cast<uint8_t*>(b.data()) + 56, 'a', 24); });
    attach("attach_car_id_without_nul", "Malformed", [](auto& b) { std::memset(reinterpret_cast<uint8_t*>(b.data()) + 152, 'x', 64); });
    attach("attach_layout_seq_odd", "NotReady", [](auto& b) { poke<uint32_t>(b, 100, 3); });
    s.segment("attach_sim_id_builtin_allowed", "attach",
              mutated([](auto& b) { std::memset(reinterpret_cast<uint8_t*>(b.data()) + 56, 0, 24); std::memcpy(reinterpret_cast<uint8_t*>(b.data()) + 56, "lmu", 3); }),
              total, {{"attach", "Ok"}, {"simId", "lmu"}});
    s.segment("attach_sim_id_empty", "attach",
              mutated([](auto& b) { std::memset(reinterpret_cast<uint8_t*>(b.data()) + 56, 0, 24); }),
              total, {{"attach", "Ok"}, {"simId", ""}});

    // wheelsPerAxle at header offset 248.
    auto setWheels = [](std::vector<uint64_t>& b, std::initializer_list<uint8_t> w) {
        uint8_t* p = reinterpret_cast<uint8_t*>(b.data()) + offsetof(Header, wheelsPerAxle);
        std::memset(p, 0, kMaxAxles);
        size_t i = 0;
        for (uint8_t n : w) p[i++] = n;
    };
    s.segment("attach_wheel_layout_valid", "attach", mutated([&](auto& b) { setWheels(b, {2, 4, 4}); }), total,
              {{"attach", "Ok"}, {"wheelsPerAxle", json::array({2, 4, 4, 0, 0, 0, 0, 0})}});
    s.segment("attach_wheel_layout_unknown", "attach", mutated([&](auto& b) { setWheels(b, {}); }), total,
              {{"attach", "Ok"}, {"wheelsPerAxle", json::array({0, 0, 0, 0, 0, 0, 0, 0})}});
    s.segment("attach_wheel_layout_eight_axles", "attach", mutated([&](auto& b) { setWheels(b, {1, 1, 1, 1, 1, 1, 1, 8}); }), total,
              {{"attach", "Ok"}});
    attach("attach_wheel_layout_gap", "Malformed", [&](auto& b) { setWheels(b, {0, 2}); });
    attach("attach_wheel_layout_too_many_wheels", "Malformed", [&](auto& b) { setWheels(b, {9}); });
    attach("attach_wheel_layout_entry_after_zero", "Malformed", [&](auto& b) { setWheels(b, {2, 2, 0, 0, 0, 0, 0, 2}); });

    // Descriptor-level rules: the header stays valid, the descriptor is judged on its own.
    auto descOf = [&](uint32_t slot) { return static_cast<size_t>(kL.descOffset) + slot * kDescBytes; };
    s.segment("attach_desc_bad_layout", "attach",
              mutated([&](auto& b) { poke<uint8_t>(b, descOf(7) + offsetof(ChannelDesc, layout), 3); }), total,
              {{"attach", "Ok"}, {"descs", json::array({{{"slot", 7}, {"valid", false}, {"tableMatch", false}}, {{"slot", 6}, {"valid", true}}})}});
    s.segment("attach_desc_bad_provenance", "attach",
              mutated([&](auto& b) { poke<uint8_t>(b, descOf(0) + offsetof(ChannelDesc, provenance), 4); }), total,
              {{"attach", "Ok"}, {"descs", json::array({{{"slot", 0}, {"valid", false}}})}});
    s.segment("attach_desc_scalar_with_layout", "attach",
              mutated([&](auto& b) { poke<uint8_t>(b, descOf(1) + offsetof(ChannelDesc, layout), 1); }), total,
              {{"attach", "Ok"}, {"descs", json::array({{{"slot", 1}, {"valid", false}, {"tableMatch", false}}})}});
    s.segment("attach_desc_array_index_not_below_capacity", "attach",
              mutated([&](auto& b) { poke<uint16_t>(b, descOf(6) + offsetof(ChannelDesc, arrayCapacity), 4);
                                     poke<uint16_t>(b, descOf(6) + offsetof(ChannelDesc, arrayIndex), 4); }), total,
              {{"attach", "Ok"}, {"descs", json::array({{{"slot", 6}, {"valid", false}, {"tableMatch", false}}})}});
    s.segment("attach_desc_capacity_above_table", "attach",
              mutated([&](auto& b) { poke<uint16_t>(b, descOf(6) + offsetof(ChannelDesc, arrayCapacity), 9); }), total,
              {{"attach", "Ok"}, {"descs", json::array({{{"slot", 6}, {"valid", true}, {"tableMatch", false}}})}});
    s.segment("attach_desc_capacity_zero_on_table_array", "attach",
              mutated([&](auto& b) { poke<uint16_t>(b, descOf(6) + offsetof(ChannelDesc, arrayCapacity), 0); }), total,
              {{"attach", "Ok"}, {"descs", json::array({{{"slot", 6}, {"tableMatch", false}}})}});
    s.segment("attach_desc_layout_mismatch_with_table", "attach",
              mutated([&](auto& b) { poke<uint8_t>(b, descOf(6) + offsetof(ChannelDesc, layout), 2); }), total,
              {{"attach", "Ok"}, {"descs", json::array({{{"slot", 6}, {"valid", true}, {"tableMatch", false}}})}});
    {
        // A producer built against a smaller capacity: 4 of the table's 8 elements.
        const auto small = makeSegment(json::array({
            {{"op", "open"}, {"capacity", kCap}, {"simId", "demo"}, {"simName", "Demo Sim"}, {"qpcFrequency", 10000000}, {"pid", 4242}, {"nominalRateMilliHz", 100000}},
            {{"op", "declare"}, {"name", "std.suspension.deflection"}, {"count", 4}, {"provenance", 1}},
            {{"op", "wheels"}, {"list", json::array({2, 2})}},
            {{"op", "commit"}},
        }));
        s.segment("attach_desc_smaller_capacity_accepted", "attach", small, total,
                  {{"attach", "Ok"}, {"channelCount", 4},
                   {"descs", json::array({
                       {{"slot", 0}, {"name", "std.suspension.deflection[0]"}, {"arrayIndex", 0}, {"arrayCapacity", 4}, {"layout", 1},
                        {"provenance", 1}, {"valid", true}, {"tableMatch", true}},
                       {{"slot", 3}, {"name", "std.suspension.deflection[3]"}, {"arrayIndex", 3}, {"arrayCapacity", 4}, {"valid", true},
                        {"tableMatch", true}},
                   })}});
    }

    // --- frame --------------------------------------------------------------
    {
        json ex = {{"result", "NewFrame"}, {"frameSeq", 2}, {"frameQpc", 2000}, {"producerState", "active"},
                   {"lastPublishQpc", 2001},
                   {"values", {{"0", 6500.0}, {"1", 42.5}, {"2", 3.0}, {"3", -0.5}, {"4", 0.75}}},
                   {"validSlots", json::array({0, 1, 2, 3, 4})}, {"eventCounts", {{"4", 1}}}};
        s.segment("frame_new", "frame", good, total, ex);
        s.segment("frame_no_new", "frame", good, total, {{"result", "NoNewFrame"}}, {{"lastSeen", 2}});
    }
    s.segment("frame_before_first", "frame", makeSegment(baseScript(false)), total, {{"result", "NoNewFrame"}, {"producerState", "active"}});
    s.segment("frame_odd_frame_seq_torn", "frame", mutated([](auto& b) { poke<uint64_t>(b, 80, 3); }), total, {{"result", "Torn"}});
    s.segment("frame_seq_end_mismatch_torn", "frame", mutated([](auto& b) { poke<uint64_t>(b, 104, 99); }), total, {{"result", "Torn"}});
    s.segment("frame_producer_inactive", "frame",
              mutated([](auto& b) { poke<uint32_t>(b, 96, 0); }), total, {{"result", "NewFrame"}, {"producerState", "inactive"}});
    s.segment("frame_producer_closed", "frame",
              mutated([](auto& b) { poke<uint32_t>(b, 96, 2); }), total, {{"result", "NewFrame"}, {"producerState", "closed"}});
    s.segment("frame_event_counter_wrap", "frame",
              mutated([](auto& b) { poke<uint32_t>(b, kL.eventCountOffset + 4 * 4, 0xFFFFFFFFu); }), total,
              {{"result", "NewFrame"}, {"eventCounts", {{"4", 4294967295u}}}});
    s.segment("frame_values_nan_passes_through", "frame",
              mutated([](auto& b) { poke<uint32_t>(b, kL.valuesOffset + 3 * 4, 0x7FC00000u); }), total,
              {{"result", "NewFrame"}, {"frameSeq", 2}});

    // --- return region ------------------------------------------------------
    {
        std::vector<uint64_t> ret(kReturnBytes / 8, 0);
        ReturnWriter w;
        w.init(ret.data(), kReturnBytes, 777);
        std::vector<uint64_t> acc(1, 0), dem(1, 0);
        for (uint32_t b : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) acc[0] |= uint64_t{1} << b;
        for (uint32_t b : {0u, 4u, 6u}) dem[0] |= uint64_t{1} << b;
        w.publishMasks(64, 2, acc.data(), dem.data());
        w.setState(ConsumerState::Active);
        w.touch(3000);
        s.segment("return_valid_active", "return", ret, kReturnBytes,
                  {{"attach", "Ok"}, {"read", true}, {"capacity", 64}, {"forLayoutSeq", 2}, {"consumerState", "active"},
                   {"lastConsumeQpc", 3000}, {"accepted", json::array({0, 1, 2, 3, 4, 5, 6})},
                   {"demanded", json::array({0, 4, 6})}});

        auto idle = ret;
        poke<uint32_t>(idle, offsetof(ReturnHeader, consumerState), 0);
        s.segment("return_idle_everything_accepted_is_demanded", "return", idle, kReturnBytes,
                  {{"attach", "Ok"}, {"read", true}, {"consumerState", "idle"},
                   {"accepted", json::array({0, 1, 2, 3, 4, 5, 6})}, {"demanded", json::array({0, 1, 2, 3, 4, 5, 6})}});

        auto bad = ret; poke<uint32_t>(bad, 0, 0);
        s.segment("return_magic_zero", "return", bad, kReturnBytes, {{"attach", "NotReady"}});
        bad = ret; poke<uint32_t>(bad, 0, 0x12345678);
        s.segment("return_bad_magic", "return", bad, kReturnBytes, {{"attach", "Malformed"}});
        bad = ret; poke<uint16_t>(bad, 4, 2);
        s.segment("return_version_major_2", "return", bad, kReturnBytes, {{"attach", "BadVersion"}});
        bad = ret; poke<uint64_t>(bad, offsetof(ReturnHeader, retSeq), 5);
        s.segment("return_torn_odd_ret_seq", "return", bad, kReturnBytes, {{"attach", "Ok"}, {"read", false}});
        bad = ret; poke<uint32_t>(bad, offsetof(ReturnHeader, capacity), 100);
        s.segment("return_capacity_invalid", "return", bad, kReturnBytes, {{"attach", "Malformed"}});
        bad = ret; poke<uint32_t>(bad, offsetof(ReturnHeader, consumerState), 9);
        s.segment("return_unknown_consumer_state", "return", bad, kReturnBytes, {{"attach", "Malformed"}});
    }

    // --- encode -------------------------------------------------------------
    auto encode = [&](const std::string& name, const json& script, const std::string& initial = {}) {
        std::vector<uint64_t> buf((total + 7) / 8, 0);
        if (!initial.empty()) {
            std::ifstream in(s.dir / initial, std::ios::binary);
            in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(total));
        }
        telemetry_script::run(script, buf);
        const std::string file = "encode/" + name + ".bin";
        writeFile(s.dir / file, buf, total);
        json c = {{"name", "encode_" + name}, {"op", "encode"}, {"file", file}, {"bytes", total}, {"script", script}, {"expect", json::object()}};
        if (!initial.empty()) c["initial"] = initial;
        s.cases.push_back(std::move(c));
        return buf;
    };

    encode("basic", baseScript());
    encode("no_frames", baseScript(false));
    {
        json ev = baseScript(false);
        for (int i = 0; i < 3; ++i) {
            ev.push_back({{"op", "frame"}, {"frameQpc", 100 + i}, {"now", 101 + i}, {"set", {{"0", 1000.0 + i}}}, {"event", {{"4", 0.5}}}});
        }
        ev.push_back({{"op", "frame"}, {"frameQpc", 200}, {"now", 201}, {"set", {{"0", 1.0}}}});
        encode("event_counter", ev);
    }
    encode("layout_rewrite_restart", json::array({
        {{"op", "open"}, {"capacity", kCap}, {"simId", "demo"}, {"simName", "Demo Sim"}, {"qpcFrequency", 10000000}, {"pid", 4343}, {"nominalRateMilliHz", 100000}},
        {{"op", "declare"}, {"name", "std.engine.rpm"}},
        {{"op", "declareLocal"}, {"name", "other"}, {"min", 0.0}, {"max", 10.0}},
        {{"op", "car"}, {"id", "car_b"}, {"name", "Car B"}},
        {{"op", "commit"}},
    }), "encode/basic.bin");
    {
        // A predecessor died mid-rewrite and mid-frame: both seqlocks odd.
        auto crashed = good;
        poke<uint32_t>(crashed, 100, 5);
        poke<uint64_t>(crashed, 80, 7);
        writeFile(s.dir / "encode" / "crashed.bin", crashed, total);
        encode("restart_after_crash", json::array({
            {{"op", "open"}, {"capacity", kCap}, {"simId", "demo"}, {"simName", "Demo Sim"}, {"qpcFrequency", 10000000}, {"pid", 4242}, {"nominalRateMilliHz", 100000}},
            {{"op", "declare"}, {"name", "std.engine.rpm"}},
            {{"op", "commit"}},
        }), "encode/crashed.bin");
    }
    encode("wheel_layout", json::array({
        {{"op", "open"}, {"capacity", kCap}, {"simId", "demo"}, {"simName", "Demo Sim"}, {"qpcFrequency", 10000000}, {"pid", 4242}, {"nominalRateMilliHz", 100000}},
        {{"op", "declare"}, {"name", "std.engine.rpm"}, {"provenance", 1}},
        {{"op", "declare"}, {"name", "std.suspension.deflection"}, {"count", 4}, {"provenance", 2}},
        {{"op", "declare"}, {"name", "std.suspension.third"}, {"count", 2}, {"provenance", 3}},
        {{"op", "wheels"}, {"list", json::array({2, 4, 4})}},
        {{"op", "commit"}},
        {{"op", "wheels"}, {"list", json::array({1, 1})}},      // car change after the first commit: a layout rewrite
    }));
    {
        // A rate change after the first commit is a layout rewrite (layoutSeq 2 -> 4); frame data is cleared.
        json rc = baseScript();
        rc.push_back({{"op", "rate"}, {"milliHz", 10000}});
        encode("nominal_rate_change", rc);
    }

    json manifest = {{"schema", 1}, {"api", "telemetry-shm"}, {"version", 1}, {"cases", s.cases}};
    std::ofstream(s.dir / "manifest.json") << manifest.dump(2) << "\n";
    std::printf("%zu cases written to %s\n", s.cases.size(), s.dir.string().c_str());
    return 0;
}
