using AtrisWave.Core.Services;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class GitHubUpdateCheckerTests
{
    [Theory]
    [InlineData("2.1.0-alpha", "v2.1.0", true)]
    [InlineData("2.1.0-alpha", "v2.2.0", true)]
    [InlineData("2.1.0", "v2.1.1", true)]
    [InlineData("2.1.0", "v2.1.0", false)]
    [InlineData("2.1.0", "v2.1.0-rc1", false)]
    [InlineData("2.2.0", "v2.1.9", false)]
    public void IsNewerVersion_ComparesSemVerCorrectly(string current, string latest, bool expectedNewer)
    {
        Assert.Equal(expectedNewer, GitHubUpdateChecker.IsNewerVersion(current, latest));
    }
}
