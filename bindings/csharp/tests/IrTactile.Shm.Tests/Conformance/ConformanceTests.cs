// Conformance tests - one theory case per manifest entry.
//
// SPDX-License-Identifier: MIT

using System.Text.Json;

namespace IrTactile.Shm.Tests.Conformance;

public class ConformanceTests
{
    /// <summary>The stream-shm v1 manifest, located by walking up from the
    /// test assembly until the conformance tree is found.</summary>
    private static string FindManifestPath()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            string candidate = Path.Combine(dir.FullName,
                "conformance", "stream-shm", "v1", "manifest.json");
            if (File.Exists(candidate)) return candidate;
            dir = dir.Parent;
        }
        throw new FileNotFoundException(
            "conformance/stream-shm/v1/manifest.json not found in any parent of " +
            AppContext.BaseDirectory);
    }

    private static readonly string ManifestPath = FindManifestPath();
    private static readonly string ManifestJson = File.ReadAllText(ManifestPath);

    public static IEnumerable<object[]> CaseIds
    {
        get
        {
            using JsonDocument doc = JsonDocument.Parse(ManifestJson);
            foreach (JsonElement c in doc.RootElement.GetProperty("cases").EnumerateArray())
                yield return new object[] { c.GetProperty("id").GetString()! };
        }
    }

    [Fact]
    public void ManifestSchema_IsSupported()
    {
        // Refuse a manifest schema this harness does not implement, rather
        // than running it with whatever ops happen to still be recognised. A
        // newer schema may have changed op semantics under names that still
        // parse.
        using JsonDocument doc = JsonDocument.Parse(ManifestJson);
        Assert.Equal(ConformanceRunner.SupportedSchema,
                     doc.RootElement.GetProperty("schema").GetInt32());
    }

    [Theory]
    [MemberData(nameof(CaseIds))]
    public void Case(string id)
    {
        using JsonDocument doc = JsonDocument.Parse(ManifestJson);
        JsonElement c = doc.RootElement.GetProperty("cases").EnumerateArray()
            .Single(e => e.GetProperty("id").GetString() == id);

        List<string> failures = ConformanceRunner.RunCase(c, Path.GetDirectoryName(ManifestPath)!);
        Assert.True(failures.Count == 0,
            $"{failures.Count} failure(s):\n" + string.Join("\n", failures));
    }
}
