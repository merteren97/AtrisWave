using System.Xml.Linq;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class LocalizationParityTests
{
    private static readonly XNamespace XamlNs = "http://schemas.microsoft.com/winfx/2006/xaml";

    [Fact]
    public void EnglishAndTurkishResourceDictionaries_HaveIdenticalKeys()
    {
        string uiRoot = FindUiSolutionRoot();
        string enPath = Path.Combine(uiRoot, "AtrisWave.UI", "Resources", "Strings.en.xaml");
        string trPath = Path.Combine(uiRoot, "AtrisWave.UI", "Resources", "Strings.tr.xaml");

        Assert.True(File.Exists(enPath), $"Missing English resource dictionary at {enPath}");
        Assert.True(File.Exists(trPath), $"Missing Turkish resource dictionary at {trPath}");

        List<string> enKeys = LoadKeys(enPath);
        List<string> trKeys = LoadKeys(trPath);

        Assert.NotEmpty(enKeys);
        Assert.Equal(enKeys.Count, enKeys.Distinct(StringComparer.Ordinal).Count());
        Assert.Equal(trKeys.Count, trKeys.Distinct(StringComparer.Ordinal).Count());

        var enSet = new HashSet<string>(enKeys, StringComparer.Ordinal);
        var trSet = new HashSet<string>(trKeys, StringComparer.Ordinal);

        string[] missingInTr = enSet.Except(trSet).OrderBy(k => k, StringComparer.Ordinal).ToArray();
        string[] extraInTr = trSet.Except(enSet).OrderBy(k => k, StringComparer.Ordinal).ToArray();

        Assert.True(missingInTr.Length == 0, $"Keys missing in Strings.tr.xaml: {string.Join(", ", missingInTr)}");
        Assert.True(extraInTr.Length == 0, $"Extra keys in Strings.tr.xaml: {string.Join(", ", extraInTr)}");
    }

    private static List<string> LoadKeys(string path)
    {
        XDocument doc = XDocument.Load(path);
        return doc.Root!
            .Elements()
            .Select(e => e.Attribute(XamlNs + "Key")?.Value)
            .Where(k => !string.IsNullOrEmpty(k))
            .Cast<string>()
            .ToList();
    }

    private static string FindUiSolutionRoot()
    {
        DirectoryInfo? dir = new(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "AtrisWave.sln")))
            {
                return dir.FullName;
            }

            dir = dir.Parent;
        }

        throw new DirectoryNotFoundException("Could not locate ui/AtrisWave.sln from test base directory.");
    }
}
