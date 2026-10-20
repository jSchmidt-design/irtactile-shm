// IrTactile.Shm.Channels - the shared channel model: kinds, flags, name rules.
//
// SPDX-License-Identifier: MIT
//
// Normative text: spec/channels/README.md. The generated table is
// StdChannels.g.cs.

namespace IrTactile.Shm.Channels;

/// <summary>Signal kind of a channel. Describes the data, not how it is played back.</summary>
public enum SignalKind : uint
{
    /// <summary>Physical quantity sampled per frame.</summary>
    Continuous = 0,

    /// <summary>Integer-valued (gear, enum, bool).</summary>
    Discrete = 1,

    /// <summary>Impulses; valid only in frames that carry an event.</summary>
    Event = 2,
}

/// <summary>Channel flag bits. Bit 0 is reserved (producers write 0, readers ignore).</summary>
[Flags]
public enum ChannelFlags : uint
{
    None = 0,

    /// <summary>The producer may leave the channel invalid for the whole session.</summary>
    Optional = 1u << 1,
}

/// <summary>Element order and labelling of an array.</summary>
public enum ArrayLayout : byte
{
    /// <summary>Free index; also the layout of every scalar.</summary>
    Index = 0,

    /// <summary>Axle by axle front to back, left to right within an axle.</summary>
    Wheel = 1,

    /// <summary>Front to back.</summary>
    Axle = 2,
}

/// <summary>Who computed a value (descriptor field <c>provenance</c>).</summary>
public enum Provenance : byte
{
    /// <summary>Not stated; consumers read it as <see cref="Native"/>.</summary>
    Unspecified = 0,

    /// <summary>Delivered by the sim API.</summary>
    Native = 1,

    /// <summary>Exact computation from native values.</summary>
    Derived = 2,

    /// <summary>Approximation.</summary>
    Estimated = 3,
}

/// <summary>One entry of a generated channel table.</summary>
/// <param name="Name">Full name; for array entries the base name without <c>[i]</c>.</param>
/// <param name="Unit">Physical unit, free text (may be empty).</param>
/// <param name="RangeMin">Nominal range minimum (default normalization range, not a clamp).</param>
/// <param name="RangeMax">Nominal range maximum.</param>
/// <param name="Kind">Signal kind.</param>
/// <param name="Flags">Channel flags.</param>
/// <param name="ArrayCapacity">Element count for arrays, 0 for scalars.</param>
/// <param name="Layout">Element layout for arrays, <see cref="ArrayLayout.Index"/> for scalars.</param>
public readonly record struct ChannelDef(
    string Name, string Unit, float RangeMin, float RangeMax,
    SignalKind Kind, ChannelFlags Flags, int ArrayCapacity, ArrayLayout Layout = ArrayLayout.Index);

/// <summary>Name rules of the channel model.</summary>
public static class ChannelModel
{
    /// <summary>Descriptor name field size, including the terminating NUL.</summary>
    public const int NameLen = 64;

    /// <summary>Longest channel name.</summary>
    public const int NameMax = NameLen - 1;

    /// <summary>Longest owner id (sourceId / simId / pluginId).</summary>
    public const int OwnerIdMax = 24;

    /// <summary>Highest array element index representable in a name suffix.</summary>
    public const int ArrayIndexMax = 65535;

    private static readonly string[] Reserved =
    {
        "std", "sim", "plugin", "irtactile",
        "iracing", "ams2", "lmu", "rf2", "ac", "acc", "ac_evo", "ac_rally", "rbr",
    };

    private static bool IsLower(char c) => (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';

    private static bool IsLastSegment(char c) => IsLower(c) || (c >= 'A' && c <= 'Z');

    /// <summary>Owner id rule: <c>[a-z0-9_]{1,24}</c>. Says nothing about reserved ids.</summary>
    public static bool IsValidOwnerId(string id)
    {
        if (string.IsNullOrEmpty(id) || id.Length > OwnerIdMax) return false;
        foreach (char c in id)
        {
            if (!IsLower(c)) return false;
        }
        return true;
    }

    /// <summary>True for ids no external source or plugin may claim.</summary>
    public static bool IsReservedOwnerId(string id) => Array.IndexOf(Reserved, id) >= 0;

    /// <summary>Full channel-name validation; owner-id reservation is not checked.</summary>
    public static bool IsValidChannelName(string name)
    {
        if (string.IsNullOrEmpty(name) || name.Length > NameMax) return false;

        string body = name;
        if (body[body.Length - 1] == ']')
        {
            int open = body.LastIndexOf('[');
            if (open < 0) return false;
            string digits = body.Substring(open + 1, body.Length - open - 2);
            if (digits.Length == 0 || digits.Length > 5) return false;
            if (digits.Length > 1 && digits[0] == '0') return false;
            int index = 0;
            foreach (char c in digits)
            {
                if (c < '0' || c > '9') return false;
                index = (index * 10) + (c - '0');
            }
            if (index > ArrayIndexMax) return false;
            body = body.Substring(0, open);
            if (body.Length == 0) return false;
        }

        string[] segments = body.Split('.');
        for (int i = 0; i < segments.Length; i++)
        {
            string seg = segments[i];
            if (seg.Length == 0) return false;
            bool last = i == segments.Length - 1;
            foreach (char c in seg)
            {
                if (last ? !IsLastSegment(c) : !IsLower(c)) return false;
            }
        }

        string first = segments[0];
        if (first == "std") return segments.Length >= 2;
        if (first == "sim" || first == "plugin") return segments.Length >= 3;
        return false;
    }
}
