namespace AtrisWave.Core.Plugins;

public sealed record DiscoveredPlugin(string Name, string Format, string Path);

/// <summary>
/// Safely scans standard Windows CLAP and VST3 directories without loading untrusted DLLs into the UI process.
/// </summary>
public static class PluginScanner
{
    public static IReadOnlyList<DiscoveredPlugin> ScanStandardDirectories()
    {
        var results = new List<DiscoveredPlugin>();
        string commonProgramFiles = Environment.GetFolderPath(Environment.SpecialFolder.CommonProgramFiles);

        if (string.IsNullOrWhiteSpace(commonProgramFiles))
        {
            return results;
        }

        ScanDirectory(Path.Combine(commonProgramFiles, "CLAP"), "*.clap", "CLAP", includeDirectories: false, results);
        ScanDirectory(Path.Combine(commonProgramFiles, "VST3"), "*.vst3", "VST3", includeDirectories: true, results);
        return results;
    }

    private static void ScanDirectory(
        string rootDir,
        string pattern,
        string format,
        bool includeDirectories,
        List<DiscoveredPlugin> results)
    {
        if (!Directory.Exists(rootDir))
        {
            return;
        }

        try
        {
            foreach (string file in Directory.EnumerateFiles(rootDir, pattern, SearchOption.AllDirectories))
            {
                results.Add(new DiscoveredPlugin(Path.GetFileNameWithoutExtension(file), format, file));
            }

            if (includeDirectories)
            {
                foreach (string dir in Directory.EnumerateDirectories(rootDir, pattern, SearchOption.AllDirectories))
                {
                    results.Add(new DiscoveredPlugin(Path.GetFileNameWithoutExtension(dir), format, dir));
                }
            }
        }
        catch
        {
            // Ignore inaccessible plugin subdirectories cleanly.
        }
    }
}
