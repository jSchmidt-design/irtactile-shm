// Conformance runner - the C# leg of the vector suite
// (conformance/README.md). Drives the binding from manifest.json + the golden
// blobs, exactly as bindings/cpp/tests/conformance_runner.cpp does for C++.
//
// SPDX-License-Identifier: MIT

using System.Globalization;
using System.Text.Json;
using IrTactile.Shm.Stream;
using IrTactile.Shm.Tests.Stream;

namespace IrTactile.Shm.Tests.Conformance;

/// <summary>
/// Executes one manifest case and collects failures. Unknown ops and unknown
/// expectation keys fail the case - silently skipping an assertion is how a
/// suite goes green over a missing feature.
/// </summary>
internal static class ConformanceRunner
{
    public const int SupportedSchema = 2;

    public static List<string> RunCase(JsonElement c, string baseDir)
    {
        var failures = new List<string>();
        string caseId = c.GetProperty("id").GetString() ?? "?";
        void Fail(string msg) => failures.Add($"[{caseId}] {msg}");

        byte[] blob = ReadBlob(Path.Combine(baseDir, c.GetProperty("blob").GetString()!), Fail);
        if (blob.Length == 0) return failures;

        using var segment = new PinnedSegment(blob);
        var reader = new Reader();
        var block = new BlockView();

        ShmStreamHeader marked = default;   // set by the markHeader op
        bool markedIsSet = false;

        foreach (JsonElement step in c.GetProperty("steps").EnumerateArray())
        {
            string op = step.GetProperty("op").GetString()!;

            switch (op)
            {
            case "attach":
            {
                StreamRequest? req = null;
                if (step.TryGetProperty("request", out JsonElement rj))
                {
                    RequireKnownKeys(rj, "attach request", failures,
                        "channelCount", "sampleRateMilliHz", "samplesPerBlock", "sampleFormat");
                    req = new StreamRequest();
                    if (rj.TryGetProperty("channelCount", out JsonElement v)) req.ChannelCount = v.GetUInt32();
                    if (rj.TryGetProperty("sampleRateMilliHz", out v)) req.SampleRateMilliHz = v.GetUInt32();
                    if (rj.TryGetProperty("samplesPerBlock", out v)) req.SamplesPerBlock = v.GetUInt32();
                    if (rj.TryGetProperty("sampleFormat", out v))
                    {
                        req.SampleFormat = v.GetString() switch
                        {
                            "Float32" => SampleFormat.Float32,
                            "Int16" => SampleFormat.Int16,
                            _ => 0,
                        };
                    }
                }

                AttachResult got = reader.Attach(segment.Ptr, segment.Length, req);

                JsonElement expect = step.GetProperty("expect");
                RequireKnownKeys(expect, "attach expectation", failures,
                    "result", "retry", "fields", "mismatchDetailContains");

                string wantName = expect.GetProperty("result").GetString()!;
                if (!Enum.TryParse(wantName, out AttachResult want))
                {
                    Fail("unknown AttachResult in manifest");
                    continue;
                }
                if (got != want)
                {
                    Fail($"attach: got {got}, want {want}");
                    continue;
                }
                if (expect.TryGetProperty("retry", out JsonElement retry) &&
                    got.ShouldRetry() != retry.GetBoolean())
                    Fail("attach retryable flag mismatch");
                if (expect.TryGetProperty("mismatchDetailContains", out JsonElement md) &&
                    !reader.MismatchDetail.Contains(md.GetString()!, StringComparison.Ordinal))
                    Fail("mismatchDetail missing expected field name");
                if (expect.TryGetProperty("fields", out JsonElement fields))
                    CheckFields(reader, fields, Fail);
                break;
            }
            case "patch":
            {
                // The producer advanced the segment: the blob replaces the
                // whole buffer. Sizes must match - a patch is a rewrite, not a
                // remap.
                byte[] next = ReadBlob(Path.Combine(baseDir, step.GetProperty("blob").GetString()!), Fail);
                if (next.Length != segment.Bytes.Length)
                    Fail("patch blob size differs from the case blob");
                else
                    Array.Copy(next, segment.Bytes, next.Length);
                break;
            }
            case "rewindToOldest":
                reader.RewindToOldest();
                break;
            case "next":
            case "latest":
            {
                bool got = op == "next" ? reader.Next(block) : reader.Latest(block);
                if (step.TryGetProperty("expect", out JsonElement expect))
                {
                    if (!(expect.GetString() == "end" && !got))
                        Fail($"{op}: expected end of data, got a block");
                }
                else
                {
                    if (!got) Fail($"{op}: expected a block, got end of data");
                    else CheckBlock(block, step.GetProperty("block"), Fail);
                }
                break;
            }
            case "droppedBlocks":
                CheckU64(reader.DroppedBlocks, step.GetProperty("expect").GetUInt64(),
                         "droppedBlocks", Fail);
                break;
            case "writeSeq":
                CheckU64(reader.WriteSeq, step.GetProperty("expect").GetUInt64(),
                         "writeSeq", Fail);
                break;
            case "attached":
                if (reader.Attached != step.GetProperty("expect").GetBoolean())
                    Fail("attached flag mismatch");
                break;
            case "refreshSnapshot":
                if (reader.RefreshSnapshot() != step.GetProperty("expect").GetBoolean())
                    Fail("refreshSnapshot result mismatch");
                break;
            case "reconfigureKind":
                if (reader.LastReconfigureKind.ToString() != step.GetProperty("expect").GetString())
                    Fail("reconfigureKind mismatch");
                break;
            case "markHeader":
                // The watermark a consumer keeps so it can classify a
                // reconfigure no matter who refreshed the snapshot in between.
                marked = reader.Header;
                markedIsSet = true;
                break;
            case "reconfigureSince":
                if (!markedIsSet)
                    Fail("reconfigureSince without a preceding markHeader");
                else if (Reader.ClassifyReconfigure(marked, reader.Header).ToString() !=
                         step.GetProperty("expect").GetString())
                    Fail("reconfigureSince mismatch");
                break;
            case "header":
                CheckFields(reader, step.GetProperty("expect"), Fail);
                break;
            case "state":
            {
                ulong nowQpc = step.GetProperty("nowQpc").GetUInt64();
                if (reader.State(nowQpc).ToString() != step.GetProperty("expect").GetString())
                    Fail("stream state mismatch");
                break;
            }
            default:
                Fail("unknown op: " + op);
                break;
            }
        }

        return failures;
    }

    // =========================================================================

    private static void CheckU64(ulong got, ulong want, string what, Action<string> fail)
    {
        if (got != want) fail($"{what}: got {got}, want {want}");
    }

    private static void RequireKnownKeys(JsonElement obj, string what,
                                         List<string> failures, params string[] known)
    {
        foreach (JsonProperty p in obj.EnumerateObject())
            if (!known.Contains(p.Name))
                failures.Add($"unknown key in {what}: {p.Name}");
    }

    /// <summary>
    /// The field subset a "fields"/"header" expectation may assert. Unknown
    /// keys fail the case: a binding that cannot observe a field must not
    /// silently skip the assertion.
    /// </summary>
    private static void CheckFields(Reader r, JsonElement fields, Action<string> fail)
    {
        foreach (JsonProperty p in fields.EnumerateObject())
        {
            switch (p.Name)
            {
            case "channelCount":      CheckU64(r.ChannelCount, p.Value.GetUInt64(), p.Name, fail); break;
            case "samplesPerBlock":   CheckU64(r.SamplesPerBlock, p.Value.GetUInt64(), p.Name, fail); break;
            case "ringBlocks":        CheckU64(r.RingBlocks, p.Value.GetUInt64(), p.Name, fail); break;
            case "sampleRateMilliHz": CheckU64(r.SampleRateMilliHz, p.Value.GetUInt64(), p.Name, fail); break;
            case "rateDivisor":       CheckU64(r.RateDivisor, p.Value.GetUInt64(), p.Name, fail); break;
            case "layoutSeq":         CheckU64(r.LayoutSeq, p.Value.GetUInt64(), p.Name, fail); break;
            case "slotPayloadOffset": CheckU64(r.Header.SlotPayloadOffset, p.Value.GetUInt64(), p.Name, fail); break;
            case "slotBytes":         CheckU64(r.Header.SlotBytes, p.Value.GetUInt64(), p.Name, fail); break;
            case "channelPlaneBytes": CheckU64(r.Header.ChannelPlaneBytes, p.Value.GetUInt64(), p.Name, fail); break;
            case "writeSeq":          CheckU64(r.WriteSeq, p.Value.GetUInt64(), p.Name, fail); break;
            case "deviceName":
                if (r.DeviceName != p.Value.GetString()) fail("deviceName mismatch");
                break;
            case "sampleFormat":
                if (r.Format.ToString() != p.Value.GetString()) fail("sampleFormat mismatch");
                break;
            default:
                fail("unknown field in expectation: " + p.Name);
                break;
            }
        }
    }

    private static void CheckBlock(BlockView b, JsonElement e, Action<string> fail)
    {
        RequireKnownKeys(e, "block expectation", fail,
            "seq", "firstSampleIndex", "timestampQpc", "flags", "silentChannelMask",
            "sampleCount", "channelCount", "samples");

        if (e.TryGetProperty("seq", out JsonElement v))               CheckU64(b.Seq, v.GetUInt64(), "block.seq", fail);
        if (e.TryGetProperty("firstSampleIndex", out v))             CheckU64(b.FirstSampleIndex, v.GetUInt64(), "block.firstSampleIndex", fail);
        if (e.TryGetProperty("timestampQpc", out v))                 CheckU64(b.TimestampQpc, v.GetUInt64(), "block.timestampQpc", fail);
        if (e.TryGetProperty("flags", out v))                        CheckU64(b.Flags, v.GetUInt64(), "block.flags", fail);
        if (e.TryGetProperty("silentChannelMask", out v))            CheckU64(b.SilentChannelMask, v.GetUInt64(), "block.silentChannelMask", fail);
        if (e.TryGetProperty("sampleCount", out v))                  CheckU64((ulong)b.SampleCount, v.GetUInt64(), "block.sampleCount", fail);
        if (e.TryGetProperty("channelCount", out v))                 CheckU64((ulong)b.ChannelCount, v.GetUInt64(), "block.channelCount", fail);

        if (!e.TryGetProperty("samples", out JsonElement samples)) return;
        RequireKnownKeys(samples, "samples expectation", fail, "tolerance", "hash", "spot");
        double tol = samples.TryGetProperty("tolerance", out JsonElement t) ? t.GetDouble() : 0.0;

        if (samples.TryGetProperty("hash", out JsonElement hash))
        {
            ulong want = ParseHash(hash.GetString()!);
            ulong got = HashSamples(b);
            if (got != want)
                fail($"samples hash: got 0x{got:x16}, want {hash.GetString()}");
        }

        if (samples.TryGetProperty("spot", out JsonElement spot))
        {
            foreach (JsonElement s in spot.EnumerateArray())
            {
                int ch = s[0].GetInt32();
                int i = s[1].GetInt32();
                double want = s[2].GetDouble();
                ReadOnlySpan<float> plane = b.Channel(ch);
                if (i >= b.SampleCount || plane.IsEmpty) { fail("spot index out of range"); continue; }
                double got = plane[i];
                double d = got > want ? got - want : want - got;
                if (d > tol)
                    fail($"spot[ch={ch} i={i}]: got {got:g9}, want {want:g9} (tol {tol:g3})");
            }
        }
    }

    private static void RequireKnownKeys(JsonElement obj, string what, Action<string> fail,
                                         params string[] known)
    {
        foreach (JsonProperty p in obj.EnumerateObject())
            if (!known.Contains(p.Name))
                fail($"unknown key in {what}: {p.Name}");
    }

    private static byte[] ReadBlob(string path, Action<string> fail)
    {
        // An empty or truncated blob must be a failure, never a skip: a case
        // that executes no steps would otherwise be reported as passing.
        if (!File.Exists(path))
        {
            fail("cannot open blob " + path);
            return Array.Empty<byte>();
        }
        byte[] bytes = File.ReadAllBytes(path);
        if (bytes.Length == 0) fail("blob is empty: " + path);
        return bytes;
    }

    /// <summary>
    /// FNV-1a 64 over the LE float32 bits of the decoded samples, planar
    /// channel-major - the manifest's samplesHash convention.
    /// </summary>
    internal static ulong HashSamples(BlockView b)
    {
        ulong h = 14695981039346656037ul;
        for (int ch = 0; ch < b.ChannelCount; ++ch)
        {
            foreach (float s in b.Channel(ch))
            {
                uint u = BitConverter.SingleToUInt32Bits(s);
                for (int k = 0; k < 4; ++k)
                {
                    h ^= (u >> (8 * k)) & 0xFFu;
                    h *= 1099511628211ul;
                }
            }
        }
        return h;
    }

    internal static ulong ParseHash(string s) =>
        ulong.Parse(s.StartsWith("0x", StringComparison.Ordinal) ? s[2..] : s,
                    NumberStyles.HexNumber, CultureInfo.InvariantCulture);
}
