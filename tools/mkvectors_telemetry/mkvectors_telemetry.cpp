// mkvectors_telemetry - writes conformance/telemetry-shm/v1: golden sections plus manifest.json.
//
//   mkvectors_telemetry <output dir>
//
// Section bytes come from the C++ producer (scripts) with byte surgery for hostile states; the
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

std::vector<uint64_t> makeSection(const json& script)
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
        {{"op", "open"}, {"capacity", kCap}, {"simId", "demo"}, {"simName", "Demo Sim"}, {"qpcFrequency", 10000000}, {"pid", 4242}},
        {{"op", "declare"}, {"name", "core.engine.rpm"}},                  // 0
        {{"op", "declare"}, {"name", "core.vehicle.velocity"}},            // 1
        {{"op", "declare"}, {"name", "core.drivetrain.gear"}},             // 2
        {{"op", "declareLocal"}, {"name", "demo_wave"}, {"min", -1.0}, {"max", 1.0}},                      // 3
        {{"op", "declareLocal"}, {"name", "demo_bump"}, {"min", 0.0}, {"max", 1.0}, {"kind", "event"}},    // 4
        {{"op", "declare"}, {"name", "ext.tyre.temp.FL"}},                 // 5
        {{"op", "declare"}, {"name", "ext.wheel.load"}},                   // 6..13
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

    // A decode-side section: file under decode/, case referencing it.
    void section(const std::string& name, const std::string& op, const std::vector<uint64_t>& buf, size_t bytes,
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
    const auto good = makeSection(baseScript());
    const json names = json::array({"core.engine.rpm", "core.vehicle.velocity", "core.drivetrain.gear", "demo_wave",
                                    "demo_bump", "ext.tyre.temp.FL", "ext.wheel.load[0]", "ext.wheel.load[7]"});

    // --- attach -------------------------------------------------------------
    {
        json ex = {{"attach", "Ok"}, {"capacity", 64}, {"channelCount", 14}, {"simId", "demo"}, {"simName", "Demo Sim"},
                   {"carId", "car_a"}, {"carName", "Car A"}, {"layoutSeq", 2}};
        ex["descNames"] = json::array({"core.engine.rpm", "core.vehicle.velocity", "core.drivetrain.gear", "demo_wave",
                                       "demo_bump", "ext.tyre.temp.FL", "ext.wheel.load[0]", "ext.wheel.load[1]"});
        ex["descs"] = json::array({
            {{"slot", 0}, {"unit", "rpm"}, {"kind", 0}, {"flags", 0}, {"arrayCapacity", 0}},
            {{"slot", 2}, {"kind", 1}},
            {{"slot", 4}, {"kind", 2}, {"flags", 0}},
            {{"slot", 5}, {"flags", 2}},
            {{"slot", 7}, {"name", "ext.wheel.load[1]"}, {"arrayIndex", 1}, {"arrayCapacity", 8}, {"flags", 2}},
        });
        s.section("attach_valid_basic", "attach", good, total, ex);
    }

    auto mutated = [&](auto mutate) {
        auto b = good;
        mutate(b);
        return b;
    };
    auto attach = [&](const std::string& name, const std::string& result, auto mutate, json extra = json::object()) {
        s.section(name, "attach", mutated(mutate), total, {{"attach", result}}, std::move(extra));
    };

    attach("attach_magic_zero", "NotReady", [](auto& b) { poke<uint32_t>(b, 0, 0); });
    attach("attach_bad_magic", "Malformed", [](auto& b) { poke<uint32_t>(b, 0, 0x12345678); });
    attach("attach_higher_version_major", "BadVersion", [](auto& b) { poke<uint16_t>(b, 4, 2); });
    attach("attach_higher_version_minor_tolerated", "Ok", [](auto& b) { poke<uint16_t>(b, 6, 9); });
    attach("attach_nonzero_reserved_tolerated", "Ok", [](auto& b) { poke<uint32_t>(b, 52, 0xDEADBEEF); poke<uint8_t>(b, 248, 7); });
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
    attach("attach_sim_id_reserved", "Malformed", [](auto& b) { std::memcpy(reinterpret_cast<uint8_t*>(b.data()) + 56, "core\0", 5); });
    attach("attach_sim_id_invalid_chars", "Malformed", [](auto& b) { std::memcpy(reinterpret_cast<uint8_t*>(b.data()) + 56, "Bad-Id\0", 7); });
    attach("attach_sim_id_without_nul", "Malformed", [](auto& b) { std::memset(reinterpret_cast<uint8_t*>(b.data()) + 56, 'a', 24); });
    attach("attach_car_id_without_nul", "Malformed", [](auto& b) { std::memset(reinterpret_cast<uint8_t*>(b.data()) + 152, 'x', 64); });
    attach("attach_layout_seq_odd", "NotReady", [](auto& b) { poke<uint32_t>(b, 100, 3); });
    s.section("attach_sim_id_builtin_allowed", "attach",
              mutated([](auto& b) { std::memset(reinterpret_cast<uint8_t*>(b.data()) + 56, 0, 24); std::memcpy(reinterpret_cast<uint8_t*>(b.data()) + 56, "lmu", 3); }),
              total, {{"attach", "Ok"}, {"simId", "lmu"}});
    s.section("attach_sim_id_empty", "attach",
              mutated([](auto& b) { std::memset(reinterpret_cast<uint8_t*>(b.data()) + 56, 0, 24); }),
              total, {{"attach", "Ok"}, {"simId", ""}});

    // --- frame --------------------------------------------------------------
    {
        json ex = {{"result", "NewFrame"}, {"frameSeq", 2}, {"frameQpc", 2000}, {"producerState", "active"},
                   {"lastPublishQpc", 2001},
                   {"values", {{"0", 6500.0}, {"1", 42.5}, {"2", 3.0}, {"3", -0.5}, {"4", 0.75}}},
                   {"validSlots", json::array({0, 1, 2, 3, 4})}, {"eventCounts", {{"4", 1}}}};
        s.section("frame_new", "frame", good, total, ex);
        s.section("frame_no_new", "frame", good, total, {{"result", "NoNewFrame"}}, {{"lastSeen", 2}});
    }
    s.section("frame_before_first", "frame", makeSection(baseScript(false)), total, {{"result", "NoNewFrame"}, {"producerState", "active"}});
    s.section("frame_odd_frame_seq_torn", "frame", mutated([](auto& b) { poke<uint64_t>(b, 80, 3); }), total, {{"result", "Torn"}});
    s.section("frame_seq_end_mismatch_torn", "frame", mutated([](auto& b) { poke<uint64_t>(b, 104, 99); }), total, {{"result", "Torn"}});
    s.section("frame_producer_inactive", "frame",
              mutated([](auto& b) { poke<uint32_t>(b, 96, 0); }), total, {{"result", "NewFrame"}, {"producerState", "inactive"}});
    s.section("frame_producer_closed", "frame",
              mutated([](auto& b) { poke<uint32_t>(b, 96, 2); }), total, {{"result", "NewFrame"}, {"producerState", "closed"}});
    s.section("frame_event_counter_wrap", "frame",
              mutated([](auto& b) { poke<uint32_t>(b, kL.eventCountOffset + 4 * 4, 0xFFFFFFFFu); }), total,
              {{"result", "NewFrame"}, {"eventCounts", {{"4", 4294967295u}}}});
    s.section("frame_values_nan_passes_through", "frame",
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
        s.section("return_valid_active", "return", ret, kReturnBytes,
                  {{"attach", "Ok"}, {"read", true}, {"capacity", 64}, {"forLayoutSeq", 2}, {"consumerState", "active"},
                   {"lastConsumeQpc", 3000}, {"accepted", json::array({0, 1, 2, 3, 4, 5, 6})},
                   {"demanded", json::array({0, 4, 6})}});

        auto idle = ret;
        poke<uint32_t>(idle, offsetof(ReturnHeader, consumerState), 0);
        s.section("return_idle_everything_accepted_is_demanded", "return", idle, kReturnBytes,
                  {{"attach", "Ok"}, {"read", true}, {"consumerState", "idle"},
                   {"accepted", json::array({0, 1, 2, 3, 4, 5, 6})}, {"demanded", json::array({0, 1, 2, 3, 4, 5, 6})}});

        auto bad = ret; poke<uint32_t>(bad, 0, 0);
        s.section("return_magic_zero", "return", bad, kReturnBytes, {{"attach", "NotReady"}});
        bad = ret; poke<uint32_t>(bad, 0, 0x12345678);
        s.section("return_bad_magic", "return", bad, kReturnBytes, {{"attach", "Malformed"}});
        bad = ret; poke<uint16_t>(bad, 4, 2);
        s.section("return_version_major_2", "return", bad, kReturnBytes, {{"attach", "BadVersion"}});
        bad = ret; poke<uint64_t>(bad, offsetof(ReturnHeader, retSeq), 5);
        s.section("return_torn_odd_ret_seq", "return", bad, kReturnBytes, {{"attach", "Ok"}, {"read", false}});
        bad = ret; poke<uint32_t>(bad, offsetof(ReturnHeader, capacity), 100);
        s.section("return_capacity_invalid", "return", bad, kReturnBytes, {{"attach", "Malformed"}});
        bad = ret; poke<uint32_t>(bad, offsetof(ReturnHeader, consumerState), 9);
        s.section("return_unknown_consumer_state", "return", bad, kReturnBytes, {{"attach", "Malformed"}});
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
        {{"op", "open"}, {"capacity", kCap}, {"simId", "demo"}, {"simName", "Demo Sim"}, {"qpcFrequency", 10000000}, {"pid", 4343}},
        {{"op", "declare"}, {"name", "core.engine.rpm"}},
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
            {{"op", "open"}, {"capacity", kCap}, {"simId", "demo"}, {"simName", "Demo Sim"}, {"qpcFrequency", 10000000}, {"pid", 4242}},
            {{"op", "declare"}, {"name", "core.engine.rpm"}},
            {{"op", "commit"}},
        }), "encode/crashed.bin");
    }

    json manifest = {{"schema", 1}, {"api", "telemetry-shm"}, {"version", 1}, {"cases", s.cases}};
    std::ofstream(s.dir / "manifest.json") << manifest.dump(2) << "\n";
    std::printf("%zu cases written to %s\n", s.cases.size(), s.dir.string().c_str());
    return 0;
}
