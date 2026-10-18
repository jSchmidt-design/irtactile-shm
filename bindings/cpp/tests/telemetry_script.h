// Scripted producer calls for the telemetry-shm vector suite (conformance/telemetry-shm/v1).
// Shared by the vector generator (tools/mkvectors_telemetry) and the C++ conformance runner: a
// script is a JSON array of ops that drive a Producer over a byte buffer; the resulting bytes are
// the golden section.
//
// Ops: open{capacity,simId,simName,qpcFrequency,pid} · declare{name} · declareLocal{name,unit,min,max,kind,flags}
//      car{id,name} · commit · state{state,now} · frame{frameQpc,now,set:{slot:v},event:{slot:amp}}
//      raw{offset,u32|u64|u8: value}   (byte surgery, used by the generator for torn / hostile states)

#ifndef IRTACTILE_TESTS_TELEMETRY_SCRIPT_H
#define IRTACTILE_TESTS_TELEMETRY_SCRIPT_H

#include <nlohmann/json.hpp>

#include <irtactile/shm/telemetry.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace telemetry_script {

using nlohmann::json;
namespace tele = irt::telemetry;
namespace sh = irtactile::shm::telemetry;

inline const tele::ChannelDef* findDef(const std::string& name)
{
    for (const auto& d : tele::core::kChannels) if (name == d.name) return &d;
    for (const auto& d : tele::ext::kChannels) if (name == d.name) return &d;
    for (const auto& d : tele::ext::kArrays) if (name == d.name) return &d;
    return nullptr;
}

inline tele::Kind kindOf(const std::string& s)
{
    if (s == "continuous") return tele::Kind::Continuous;
    if (s == "discrete") return tele::Kind::Discrete;
    if (s == "event") return tele::Kind::Event;
    throw std::runtime_error("unknown kind " + s);
}

inline sh::ProducerState stateOf(const std::string& s)
{
    if (s == "inactive") return sh::ProducerState::Inactive;
    if (s == "active") return sh::ProducerState::Active;
    if (s == "closed") return sh::ProducerState::Closed;
    throw std::runtime_error("unknown state " + s);
}

/// Runs `script` against `buffer` (8-byte aligned storage of the section size). The buffer may hold a
/// previous section (restart).
inline void run(const json& script, std::vector<uint64_t>& buffer)
{
    sh::Producer producer;
    void* base = buffer.data();
    const size_t bytes = buffer.size() * 8;
    for (const json& op : script) {
        const std::string kind = op.at("op");
        if (kind == "open") {
            sh::Identity id;
            id.simId = op.value("simId", "");
            id.simName = op.value("simName", "");
            id.qpcFrequency = op.value("qpcFrequency", uint64_t{10'000'000});
            id.pid = op.value("pid", uint32_t{4242});
            if (producer.open(base, bytes, op.at("capacity"), std::move(id)) != sh::OpenResult::Ok) {
                throw std::runtime_error("open failed");
            }
        } else if (kind == "declare") {
            const tele::ChannelDef* d = findDef(op.at("name"));
            if (!d || producer.declare(*d) == sh::Producer::kInvalidSlot) throw std::runtime_error("declare failed");
        } else if (kind == "declareLocal") {
            if (producer.declareLocal(op.at("name").get<std::string>(), op.value("unit", ""), op.value("min", 0.0f),
                                      op.value("max", 1.0f), kindOf(op.value("kind", "continuous")),
                                      op.value("flags", 0u)) == sh::Producer::kInvalidSlot) {
                throw std::runtime_error("declareLocal failed");
            }
        } else if (kind == "car") {
            producer.setCar(op.at("id").get<std::string>(), op.value("name", ""));
        } else if (kind == "commit") {
            producer.commitLayout();
        } else if (kind == "state") {
            producer.setState(stateOf(op.at("state")), op.value("now", uint64_t{0}));
        } else if (kind == "frame") {
            producer.beginFrame();
            if (op.contains("set")) {
                for (auto it = op["set"].begin(); it != op["set"].end(); ++it) {
                    producer.set(std::stoi(it.key()), it.value().get<float>());
                }
            }
            if (op.contains("event")) {
                for (auto it = op["event"].begin(); it != op["event"].end(); ++it) {
                    producer.event(std::stoi(it.key()), it.value().get<float>());
                }
            }
            producer.endFrame(op.value("frameQpc", uint64_t{0}), op.value("now", uint64_t{0}));
        } else if (kind == "raw") {
            auto* p = reinterpret_cast<uint8_t*>(base) + op.at("offset").get<size_t>();
            if (op.contains("u8")) { const uint8_t v = op["u8"]; std::memcpy(p, &v, 1); }
            if (op.contains("u16")) { const uint16_t v = op["u16"]; std::memcpy(p, &v, 2); }
            if (op.contains("u32")) { const uint32_t v = op["u32"]; std::memcpy(p, &v, 4); }
            if (op.contains("u64")) { const uint64_t v = op["u64"]; std::memcpy(p, &v, 8); }
            if (op.contains("fill")) std::memset(p, op["fill"].get<int>(), op.at("len").get<size_t>());
        } else {
            throw std::runtime_error("unknown op " + kind);
        }
    }
}

}  // namespace telemetry_script

#endif  // IRTACTILE_TESTS_TELEMETRY_SCRIPT_H
