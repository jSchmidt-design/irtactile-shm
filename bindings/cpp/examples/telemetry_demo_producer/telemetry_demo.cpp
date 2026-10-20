// telemetry_demo - the smallest useful telemetry-shm v1 producer.
//
// SPDX-License-Identifier: MIT
//
// Publishes at 100 Hz:
//   std.input.throttle, std.engine.rpm                             (table scalars)
//   std.suspension.deflection[0..3]                                (table array: 4 of 8 elements, layout wheel, wheels {2,2})
//   demo_wave                                                      (local channel -> sim.<simId>.demo_wave)
//   demo_bump                                                      (EVENT, one every 2 s)
// Keys: 'a' toggles Active / Inactive (sim running vs. sitting in a menu), 'q' quits.
//
// The source id comes from IRTACTILE_TELEMETRY_SOURCE (irTactile sets it when it launches this exe as a
// companion), else "demo". Once a second it prints which of its channels irTactile accepted and demanded.

#include <irtactile/shm/telemetry.h>
#include <irtactile/shm/telemetry/win32.h>

#include <conio.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace irtactile::shm::telemetry;
namespace channels = irtactile::shm::channels;

int main()
{
    const std::string section = sectionNameFromEnvironment("demo");

    Identity id;
    id.simId = "";               // empty: the consumer uses the sourceId as the namespace (sim.<sourceId>.*)
    id.simName = "telemetry_demo";

    ProducerSection s;
    if (s.create(section, 256, id) != OpenResult::Ok) {
        std::fprintf(stderr, "cannot create section %s\n", section.c_str());
        return 1;
    }
    Producer& p = s.producer();

    const int thr  = p.declare(channels::standard::Id::InputThrottle, channels::Provenance::Native);
    const int rpm  = p.declare(channels::standard::Id::EngineRpm, channels::Provenance::Native);
    const int defl = p.declareArray(channels::standard::ArrayId::SuspensionDeflection, 4, channels::Provenance::Native);
    p.setWheelLayout({2, 2});
    const int wave = p.declareLocal("demo_wave", "", -1.0f, 1.0f, channels::Kind::Continuous);
    const int bump = p.declareLocal("demo_bump", "", 0.0f, 1.0f, channels::Kind::Event);
    p.setCar("demo_car", "Demo Car");
    p.commitLayout();   // the section becomes visible here

    std::printf("telemetry_demo: section '%s'\n  a = toggle Active/Inactive, q = quit\n", section.c_str());

    bool active = true;
    p.setState(ProducerState::Active, qpcNow());

    const double freq = static_cast<double>(qpcFrequency());
    const uint64_t t0 = qpcNow();
    uint64_t nextBump = t0 + static_cast<uint64_t>(2.0 * freq);
    uint64_t nextReport = t0;
    ReturnView rv;

    while (true) {
        if (_kbhit()) {
            const int k = _getch();
            if (k == 'q') break;
            if (k == 'a') {
                active = !active;
                p.setState(active ? ProducerState::Active : ProducerState::Inactive, qpcNow());
                std::printf("producerState -> %s\n", active ? "Active" : "Inactive");
            }
        }

        const uint64_t now = qpcNow();
        const double t = static_cast<double>(now - t0) / freq;
        if (active) {
            p.beginFrame();
            p.set(rpm, static_cast<float>(3000.0 + 2500.0 * std::sin(t * 0.8)));
            p.set(thr, static_cast<float>(0.5 + 0.5 * std::sin(t * 0.3)));
            for (int i = 0; i < 4; ++i) p.set(defl + i, static_cast<float>(0.02 * std::sin(t * 2.0 + i)));
            p.set(wave, static_cast<float>(std::sin(t * 6.2831853)));
            if (now >= nextBump) {
                p.event(bump, 1.0f);
                nextBump += static_cast<uint64_t>(2.0 * freq);
            }
            p.endFrame(now, now);
        } else {
            p.setState(ProducerState::Inactive, now);   // heartbeat while idle
        }

        if (now >= nextReport) {
            nextReport = now + static_cast<uint64_t>(freq);
            if (s.pollReturn(rv)) {
                int accepted = 0, demanded = 0;
                for (uint32_t slot = 0; slot < p.channelCount(); ++slot) {
                    accepted += rv.isAccepted(slot) ? 1 : 0;
                    demanded += rv.isDemanded(slot) ? 1 : 0;
                }
                std::printf("irTactile: %s, accepted %d/%u, demanded %d (rpm:%s wave:%s)\n",
                            rv.consumerState == ConsumerState::Active ? "Active"
                            : rv.consumerState == ConsumerState::Idle ? "Idle" : "Closed",
                            accepted,
                            p.channelCount(), demanded, rv.isDemanded(rpm) ? "yes" : "no",
                            rv.isDemanded(wave) ? "yes" : "no");
            } else {
                std::printf("irTactile: not connected\n");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    p.setState(ProducerState::Closed, qpcNow());
    return 0;
}
