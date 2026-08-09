// reader_min.cpp - the portable half, and the executable twin of the
// addressing block and the publish protocol in README.md.
//
// This file builds on Linux with GCC or Clang, which is the point: the Client
// in stream/win32.h would put a <windows.h> consumer in the public repo,
// and if the only CI example used it, a stray #include <windows.h> migrating up
// into stream/stream.h would compile clean forever. The Linux leg is the one that
// fails when the split erodes.
//
// It also demonstrates the acceptance criterion for the addressing half of the
// API: the consumer loop MULTIPLIES NOTHING. No offsets, no strides, no
// bytesPerSample, no format branch, no seq re-check. If this file ever needs a
// multiplication outside the header, the API is wrong.
//
//     c++ -std=c++17 -Wall -Wextra -I../include reader_min.cpp -o reader_min

#include <irtactile/shm/stream.h>

#include <cstdio>
#include <cstring>
#include <vector>

using namespace irtactile::shm::stream;

// ---------------------------------------------------------------------------
// A minimal producer, so this example is self-contained and can be run without
// irTactile. It exercises the same publish protocol over a plain std::vector.
// ---------------------------------------------------------------------------
class ToyProducer {
public:
    explicit ToyProducer(const LayoutParams& p)
    {
        m_bytes.assign(static_cast<size_t>(computeTotalSize(p)), 0);

        ShmStreamHeader& h = header();
        h.versionMajor         = kVersionMajor;
        h.versionMinor         = kVersionMinor;
        h.headerBytes          = static_cast<uint32_t>(sizeof(ShmStreamHeader));
        h.totalBytes           = static_cast<uint32_t>(computeTotalSize(p));
        h.sampleFormat         = static_cast<uint32_t>(p.sampleFormat);
        h.channelLayout        = static_cast<uint32_t>(ChannelLayout::Planar);
        h.channelCount         = p.channelCount;
        h.sampleRateMilliHz    = 3000u * 1000u;
        h.sourceSampleRate     = 48000;
        h.rateDivisor          = 16;
        h.samplesPerBlock      = p.samplesPerBlock;
        h.ringBlocks           = p.ringBlocks;
        h.bytesPerSample       = bytesPerSampleOf(p.sampleFormat);
        h.slotPayloadOffset    = slotPayloadOffsetOf();
        h.channelArrayOffset   = 0;                       // no channel table in v1
        h.slotArrayOffset      = slotArrayOffsetOf();
        h.slotBytes            = slotBytesOf(p);
        h.channelPlaneBytes    = channelPlaneBytesOf(p);
        h.qpcFrequency         = 10'000'000;
        std::snprintf(h.deviceName, sizeof(h.deviceName), "toy");
        h.producerPid          = 0;                       // 0 ⇒ unknown; a real producer stores its own PID
        h.layoutSeq            = 0;                       // even: readable (fresh section never needs an odd rewrite)

        // Creation ordering (normative): magic is the LAST store, released,
        // after every other header field is in place. A reader that passes the
        // magic check must never be able to snapshot a half-written header -
        // validateLayout() failing there is Malformed, which is fatal, not a
        // retry. Irrelevant for this single-threaded toy, but the order is the
        // contract, so the toy models it.
        irtactile::shm::detail::releaseStore(h.magic, kMagic);
    }

    const void* base() const { return m_bytes.data(); }
    size_t      size() const { return m_bytes.size(); }

    /// One publish. Sample i of channel c is
    /// `((m_first + i) % 100) / 100.0f + c / 1000.0f`: a sawtooth from 0 to
    /// 0.99 with a small per-channel offset, so the test pattern is non-zero
    /// but bounded and every channel reads distinctly.
    void publish()
    {
        ShmStreamHeader& h    = header();
        uint8_t*         slot = slotAt(m_bytes.data(), h, m_writeSeq);
        auto*            desc = reinterpret_cast<ShmBlockDesc*>(slot);

        // Step 2 of the publish protocol: claim the slot BEFORE a single sample
        // of the new block lands in it. Without this a reader preempted
        // mid-copy would re-check a descriptor still holding the previous
        // block's seq and return torn data.
        irtactile::shm::detail::releaseStore(desc->seq, kSlotBusy);

        uint32_t silent = 0;
        for (uint32_t ch = 0; ch < h.channelCount; ++ch) {
            uint8_t* plane   = planeAt(slot, h, ch);
            bool     nonZero = false;
            for (uint32_t i = 0; i < h.samplesPerBlock; ++i) {
                const float v = static_cast<float>((m_first + i) % 100) / 100.0f +
                                static_cast<float>(ch) / 1000.0f;
                if (v != 0.0f) nonZero = true;
                if (h.bytesPerSample == 2) {
                    const int16_t q = floatToInt16(v);
                    std::memcpy(plane + static_cast<size_t>(i) * 2, &q, 2);
                } else {
                    std::memcpy(plane + static_cast<size_t>(i) * 4, &v, 4);
                }
            }
            if (!nonZero) silent |= (1u << ch);
        }

        desc->firstSampleIndex  = m_first;
        desc->timestampQpc      = m_writeSeq * 1000;
        desc->flags             = m_writeSeq == 0 ? BlockDiscontinuity : 0u;
        desc->silentChannelMask = silent;
        // Producer obligation: stamp the layoutSeq this payload was
        // encoded under. It is 0 for this toy, which never rewrites its layout,
        // but a real producer must read it rather than assume.
        desc->layoutEpoch       = irtactile::shm::detail::acquireLoad(h.layoutSeq);
        irtactile::shm::detail::releaseStore(desc->seq, m_writeSeq);

        // lastPublishQpc first, THEN writeSeq. The two are separate stores and
        // a reader can land between them, so publishing the timestamp first
        // means it is never *older* than the block count implies.
        irtactile::shm::detail::releaseStore(h.lastPublishQpc, desc->timestampQpc);
        irtactile::shm::detail::releaseStore(h.writeSeq, m_writeSeq + 1);

        ++m_writeSeq;
        m_first += h.samplesPerBlock;
    }

    void setActive(bool active, uint32_t wiredMask)
    {
        ShmStreamHeader& h = header();
        irtactile::shm::detail::releaseStore(h.wiredChannelMask, wiredMask);
        irtactile::shm::detail::releaseStore(h.deviceState,
                             static_cast<uint32_t>(active ? DeviceState::Active
                                                          : DeviceState::Inactive));
    }

private:
    ShmStreamHeader& header() { return *reinterpret_cast<ShmStreamHeader*>(m_bytes.data()); }

    std::vector<uint8_t> m_bytes;
    uint64_t             m_writeSeq = 0;
    uint64_t             m_first    = 0;
};

int main()
{
    const LayoutParams p{ /*channelCount*/ 8, /*samplesPerBlock*/ 48,
                          /*ringBlocks*/ 2, SampleFormat::Float32 };

    ToyProducer prod(p);
    prod.setActive(true, 0x07);   // three of eight channels wired

    // What this build was written against. Any field left at zero is not
    // checked, so a consumer asserts only what it actually depends on.
    const StreamRequest expected{ /*channelCount*/ 8, /*sampleRateMilliHz*/ 3'000'000, 0,
                                  static_cast<SampleFormat>(0) };

    Reader reader;
    const AttachResult ar = reader.attach(prod.base(), prod.size(), &expected);
    if (ar != AttachResult::Ok) {
        std::printf("attach failed (retry=%d): %s\n", shouldRetry(ar) ? 1 : 0,
                    reader.mismatchDetail());
        return 1;
    }

    std::printf("attached: %u ch @ %.3f Hz, %u samples/block, period %.3f ms, wired mask 0x%02x\n",
                reader.channelCount(),
                reader.sampleRateMilliHz() / 1000.0,
                reader.samplesPerBlock(),
                reader.periodMs(),
                reader.wiredChannelMask());

    for (int round = 0; round < 4; ++round) prod.publish();

    // The whole consumer loop. It multiplies nothing.
    BlockView b;
    double    checksum = 0.0;
    unsigned  blocks   = 0;
    while (reader.next(b)) {
        for (uint32_t ch = 0; ch < reader.channelCount(); ++ch) {
            const float* s = b.channel(ch);
            for (uint32_t i = 0; i < b.sampleCount; ++i) checksum += s[i];
        }
        ++blocks;
    }

    std::printf("read %u block(s), dropped %llu, checksum %.4f\n", blocks,
                static_cast<unsigned long long>(reader.droppedBlocks()), checksum);

    // The liveness table is a pure function of the header plus a clock, so it
    // is answerable on the FIRST read rather than after a staleness window.
    // The 4 publishes stamped timestampQpc = 0, 1000, 2000, 3000; 4001 lands
    // well past the last one and well inside the 5×period liveness window.
    const uint64_t now = 4 * 1000 + 1;
    std::printf("state: %s\n",
                reader.state(now) == StreamState::Live ? "Live" : "not live");

    // A ring of 2 has a window of 1, so 4 publishes with no drain leave
    // exactly 1 block readable (the other 3 land in DroppedBlocks).
    return blocks > 0 ? 0 : 1;
}
