// C++ leg of the telemetry-shm v1 vector suite (conformance/telemetry-shm/v1, see conformance/README.md).
//
//   telemetry_conformance_runner <path to manifest.json>
//
// Case ops:
//   attach  - Reader::attach on a golden segment, compare the verdict and the snapshot
//   frame   - attach, then readFrame, compare the result and the copied frame
//   return  - ReturnReader::attach + read on a golden return region
//   encode  - run a producer script, compare the segment bytes with the golden file
// Exit code: number of failed cases (0 = pass).

#include "telemetry_script.h"

#include <cstdio>
#include <fstream>
#include <iterator>

using nlohmann::json;
using namespace irtactile::shm::telemetry;

namespace {

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

std::vector<uint64_t> aligned(const std::vector<uint8_t>& bytes, size_t sizeOverride = 0)
{
    const size_t n = sizeOverride != 0 ? sizeOverride : bytes.size();
    std::vector<uint64_t> w((n + 7) / 8, 0);
    std::memcpy(w.data(), bytes.data(), bytes.size() < n ? bytes.size() : n);
    return w;
}

const char* attachName(AttachResult r) { return toCString(r); }

const char* readName(ReadResult r)
{
    switch (r) {
        case ReadResult::NewFrame: return "NewFrame";
        case ReadResult::NoNewFrame: return "NoNewFrame";
        case ReadResult::Torn: return "Torn";
        case ReadResult::LayoutChanged: return "LayoutChanged";
        case ReadResult::LayoutBusy: return "LayoutBusy";
        case ReadResult::Detached: return "Detached";
    }
    return "?";
}

const char* producerStateName(ProducerState s)
{
    return s == ProducerState::Inactive ? "inactive" : s == ProducerState::Active ? "active" : "closed";
}
const char* consumerStateName(ConsumerState s)
{
    return s == ConsumerState::Idle ? "idle" : s == ConsumerState::Active ? "active" : "closed";
}

struct Failures {
    int count{0};
    std::string current;
    void fail(const std::string& what)
    {
        std::printf("  FAIL [%s] %s\n", current.c_str(), what.c_str());
        ++count;
    }
    template <class A, class B>
    void eq(const char* field, const A& got, const B& want)
    {
        if (!(got == want)) {
            std::string g, w;
            if constexpr (std::is_arithmetic_v<A>) g = std::to_string(got); else g = std::string(got);
            if constexpr (std::is_arithmetic_v<B>) w = std::to_string(want); else w = std::string(want);
            fail(std::string(field) + ": got " + g + ", want " + w);
        }
    }
    /// Fail on any key the harness does not act on: a misspelled or newly
    /// added expectation must not pass silently.
    void knownKeys(const json& obj, const char* what, std::initializer_list<const char*> known)
    {
        for (const auto& [k, v] : obj.items()) {
            (void)v;
            bool ok = false;
            for (const char* n : known) ok = ok || k == n;
            if (!ok) fail(std::string("unknown key in ") + what + ": " + k);
        }
    }
};

void checkAttach(const Reader& r, const json& ex, Failures& f)
{
    if (ex.contains("capacity")) f.eq("capacity", r.header().capacity, ex["capacity"].get<uint32_t>());
    if (ex.contains("channelCount")) f.eq("channelCount", r.header().channelCount, ex["channelCount"].get<uint32_t>());
    if (ex.contains("simId")) f.eq("simId", std::string(r.header().simId), ex["simId"].get<std::string>());
    if (ex.contains("simName")) f.eq("simName", std::string(r.header().simName), ex["simName"].get<std::string>());
    if (ex.contains("carId")) f.eq("carId", std::string(r.header().carId), ex["carId"].get<std::string>());
    if (ex.contains("carName")) f.eq("carName", std::string(r.header().carName), ex["carName"].get<std::string>());
    if (ex.contains("wheelsPerAxle")) {
        std::vector<uint8_t> got(r.header().wheelsPerAxle, r.header().wheelsPerAxle + kMaxAxles);
        if (got != ex["wheelsPerAxle"].get<std::vector<uint8_t>>()) f.fail("wheelsPerAxle");
    }
    if (ex.contains("layoutSeq")) f.eq("layoutSeq", r.snapshotLayoutSeq(), ex["layoutSeq"].get<uint32_t>());
    if (ex.contains("descNames")) {
        const auto& names = ex["descNames"];
        for (size_t i = 0; i < names.size(); ++i) {
            f.eq(("descNames[" + std::to_string(i) + "]").c_str(), std::string(r.descriptors()[i].name),
                 names[i].get<std::string>());
        }
    }
    if (ex.contains("descs")) {
        for (const json& d : ex["descs"]) {
            const auto& got = r.descriptors()[d.at("slot").get<size_t>()];
            const std::string tag = "desc[" + std::to_string(d["slot"].get<size_t>()) + "].";
            if (d.contains("name")) f.eq((tag + "name").c_str(), std::string(got.name), d["name"].get<std::string>());
            if (d.contains("unit")) f.eq((tag + "unit").c_str(), std::string(got.unit), d["unit"].get<std::string>());
            if (d.contains("kind")) f.eq((tag + "kind").c_str(), got.kind, d["kind"].get<uint32_t>());
            if (d.contains("flags")) f.eq((tag + "flags").c_str(), got.flags, d["flags"].get<uint32_t>());
            if (d.contains("arrayIndex")) f.eq((tag + "arrayIndex").c_str(), uint32_t{got.arrayIndex}, d["arrayIndex"].get<uint32_t>());
            if (d.contains("arrayCapacity")) f.eq((tag + "arrayCapacity").c_str(), uint32_t{got.arrayCapacity}, d["arrayCapacity"].get<uint32_t>());
            if (d.contains("provenance")) f.eq((tag + "provenance").c_str(), uint32_t{got.provenance}, d["provenance"].get<uint32_t>());
            if (d.contains("layout")) f.eq((tag + "layout").c_str(), uint32_t{got.layout}, d["layout"].get<uint32_t>());
            if (d.contains("valid")) f.eq((tag + "valid").c_str(), validateDescriptor(got) ? 1 : 0, d["valid"].get<bool>() ? 1 : 0);
            if (d.contains("tableMatch")) {
                // The base name (without [i]) is looked up in the std table; the element is the descriptor's own index.
                std::string base = got.name;
                const size_t br = base.find('[');
                if (br != std::string::npos) base.resize(br);
                const irtactile::shm::channels::ChannelDef* def = telemetry_script::findDef(base);
                const bool match = def != nullptr && matchesTable(got, *def, got.arrayIndex);
                f.eq((tag + "tableMatch").c_str(), match ? 1 : 0, d["tableMatch"].get<bool>() ? 1 : 0);
            }
        }
    }
}

void runCase(const json& c, const std::string& dir, Failures& f)
{
    const std::string op = c.at("op");
    const json& ex = c.at("expect");

    f.knownKeys(c, "case", {"name", "op", "file", "expect", "mappedBytes", "lastSeen", "bytes", "initial", "script"});
    if (op == "attach") {
        f.knownKeys(ex, "attach expectation", {"attach", "capacity", "channelCount", "simId", "simName", "carId",
                                               "carName", "wheelsPerAxle", "layoutSeq", "descNames", "descs"});
        if (ex.contains("descs")) {
            for (const json& d : ex["descs"]) {
                f.knownKeys(d, "desc expectation", {"slot", "name", "unit", "kind", "flags", "arrayIndex",
                                                    "arrayCapacity", "provenance", "layout", "valid", "tableMatch"});
            }
        }
    } else if (op == "frame") {
        f.knownKeys(ex, "frame expectation", {"attach", "result", "producerState", "lastPublishQpc", "frameSeq",
                                              "frameQpc", "values", "validSlots", "eventCounts"});
    } else if (op == "return") {
        f.knownKeys(ex, "return expectation", {"attach", "read", "capacity", "forLayoutSeq", "consumerState",
                                               "lastConsumeQpc", "accepted", "demanded"});
    } else if (op == "encode") {
        f.knownKeys(ex, "encode expectation", {});
    }

    if (op == "encode") {
        std::vector<uint64_t> buf((c.at("bytes").get<size_t>() + 7) / 8, 0);
        if (c.contains("initial")) {
            const auto init = readFile(dir + "/" + c["initial"].get<std::string>());
            std::memcpy(buf.data(), init.data(), init.size() < buf.size() * 8 ? init.size() : buf.size() * 8);
        }
        telemetry_script::run(c.at("script"), buf);
        const auto golden = readFile(dir + "/" + c.at("file").get<std::string>());
        if (golden.size() != c["bytes"].get<size_t>()) { f.fail("golden size differs from 'bytes'"); return; }
        const uint8_t* got = reinterpret_cast<const uint8_t*>(buf.data());
        for (size_t i = 0; i < golden.size(); ++i) {
            if (got[i] != golden[i]) {
                f.fail("byte " + std::to_string(i) + " differs: got " + std::to_string(got[i]) + ", want " +
                       std::to_string(golden[i]));
                return;
            }
        }
        return;
    }

    const auto blob = readFile(dir + "/" + c.at("file").get<std::string>());
    const size_t mapped = c.value("mappedBytes", blob.size());
    auto words = aligned(blob, mapped);

    if (op == "return") {
        ReturnReader rr;
        const AttachResult ar = rr.attach(words.data(), mapped);
        f.eq("attach", std::string(attachName(ar)), ex.at("attach").get<std::string>());
        if (ex.contains("read")) {
            ReturnView v;
            const bool ok = rr.read(v);
            f.eq("read", ok ? 1 : 0, ex["read"].get<bool>() ? 1 : 0);
            if (ok) {
                if (ex.contains("capacity")) f.eq("capacity", v.capacity, ex["capacity"].get<uint32_t>());
                if (ex.contains("forLayoutSeq")) f.eq("forLayoutSeq", v.forLayoutSeq, ex["forLayoutSeq"].get<uint32_t>());
                if (ex.contains("consumerState")) f.eq("consumerState", std::string(consumerStateName(v.consumerState)), ex["consumerState"].get<std::string>());
                if (ex.contains("lastConsumeQpc")) f.eq("lastConsumeQpc", v.lastConsumeQpc, ex["lastConsumeQpc"].get<uint64_t>());
                if (ex.contains("accepted")) {
                    for (uint32_t s = 0; s < v.capacity; ++s) {
                        bool want = false;
                        for (const json& e : ex["accepted"]) want = want || e.get<uint32_t>() == s;
                        if (v.isAccepted(s) != want) f.fail("accepted[" + std::to_string(s) + "]");
                    }
                }
                if (ex.contains("demanded")) {
                    for (uint32_t s = 0; s < v.capacity; ++s) {
                        bool want = false;
                        for (const json& e : ex["demanded"]) want = want || e.get<uint32_t>() == s;
                        if (v.isDemanded(s) != want) f.fail("demanded[" + std::to_string(s) + "]");
                    }
                }
            }
        }
        return;
    }

    Reader r;
    const AttachResult ar = r.attach(words.data(), mapped);
    if (op == "attach") {
        f.eq("attach", std::string(attachName(ar)), ex.at("attach").get<std::string>());
        if (ar == AttachResult::Ok) checkAttach(r, ex, f);
        return;
    }
    if (op == "frame") {
        f.eq("attach", std::string(attachName(ar)), ex.value("attach", std::string("Ok")));
        if (ar != AttachResult::Ok) return;
        Frame fr;
        const ReadResult rr = r.readFrame(fr, c.value("lastSeen", uint64_t{0}));
        f.eq("result", std::string(readName(rr)), ex.at("result").get<std::string>());
        if (ex.contains("producerState")) f.eq("producerState", std::string(producerStateName(r.producerState())), ex["producerState"].get<std::string>());
        if (ex.contains("lastPublishQpc")) f.eq("lastPublishQpc", r.lastPublishQpc(), ex["lastPublishQpc"].get<uint64_t>());
        if (rr != ReadResult::NewFrame) return;
        if (ex.contains("frameSeq")) f.eq("frameSeq", fr.frameSeq, ex["frameSeq"].get<uint64_t>());
        if (ex.contains("frameQpc")) f.eq("frameQpc", fr.frameQpc, ex["frameQpc"].get<uint64_t>());
        if (ex.contains("values")) {
            for (auto it = ex["values"].begin(); it != ex["values"].end(); ++it) {
                const size_t s = std::stoul(it.key());
                if (fr.values[s] != it.value().get<float>()) f.fail("values[" + it.key() + "]");
            }
        }
        if (ex.contains("validSlots")) {
            for (uint32_t s = 0; s < r.header().capacity; ++s) {
                bool want = false;
                for (const json& e : ex["validSlots"]) want = want || e.get<uint32_t>() == s;
                const bool got = (fr.valid[s >> 6] >> (s & 63)) & 1u;
                if (got != want) f.fail("valid[" + std::to_string(s) + "]");
            }
        }
        if (ex.contains("eventCounts")) {
            for (auto it = ex["eventCounts"].begin(); it != ex["eventCounts"].end(); ++it) {
                f.eq(("eventCount[" + it.key() + "]").c_str(), fr.eventCount[std::stoul(it.key())], it.value().get<uint32_t>());
            }
        }
        return;
    }
    f.fail("unknown op " + op);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: telemetry_conformance_runner <manifest.json>\n");
        return 2;
    }
    const std::string manifestPath = argv[1];
    const size_t slash = manifestPath.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? "." : manifestPath.substr(0, slash);

    json manifest;
    try {
        std::ifstream in(manifestPath);
        manifest = json::parse(in);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cannot read manifest: %s\n", e.what());
        return 2;
    }
    // A runner refuses a schema it does not implement.
    if (manifest.value("schema", 0) != 1) {
        std::fprintf(stderr, "unsupported manifest schema %d\n", manifest.value("schema", 0));
        return 2;
    }

    Failures f;
    int ran = 0;
    for (const json& c : manifest.at("cases")) {
        f.current = c.at("name");
        const int before = f.count;
        try {
            runCase(c, dir, f);
        } catch (const std::exception& e) {
            f.fail(std::string("exception: ") + e.what());
        }
        std::printf("%s %s\n", f.count == before ? "ok  " : "FAIL", f.current.c_str());
        ++ran;
    }
    std::printf("%d cases, %d failures\n", ran, f.count);
    return f.count;
}
