using System.Security.Cryptography;
using System.Text;
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

    [Fact]
    public void ParseReleasesJson_ExtractsPrereleaseInstallerAndChecksums()
    {
        const string json = """
            [
              {
                "tag_name": "v2.5.0-alpha",
                "draft": true,
                "prerelease": true,
                "html_url": "https://github.com/merteren97/AtrisWave/releases/tag/v2.5.0-alpha",
                "assets": []
              },
              {
                "tag_name": "v2.4.0-alpha",
                "draft": false,
                "prerelease": true,
                "html_url": "https://github.com/merteren97/AtrisWave/releases/tag/v2.4.0-alpha",
                "body": "Release notes here",
                "assets": [
                  {
                    "name": "AtrisWave-Setup-2.4.0-alpha.exe",
                    "browser_download_url": "https://example.com/AtrisWave-Setup-2.4.0-alpha.exe"
                  },
                  {
                    "name": "SHA256SUMS.txt",
                    "browser_download_url": "https://example.com/SHA256SUMS.txt"
                  }
                ]
              }
            ]
            """;

        UpdateCheckResult res = GitHubUpdateChecker.ParseReleasesJson(json, "2.3.0-alpha");
        Assert.True(res.Success);
        Assert.True(res.UpdateAvailable);
        Assert.Equal("v2.4.0-alpha", res.LatestVersion);
        Assert.True(res.IsPrerelease);
        Assert.Equal("https://example.com/AtrisWave-Setup-2.4.0-alpha.exe", res.InstallerDownloadUrl);
        Assert.Equal("https://example.com/SHA256SUMS.txt", res.ChecksumsDownloadUrl);
        Assert.Equal("Release notes here", res.ReleaseNotes);
    }

    [Fact]
    public async Task AppUpdaterService_ParsesAndVerifiesSha256Sums()
    {
        string tempFile = Path.Combine(Path.GetTempPath(), $"atriswave_sha_test_{Guid.NewGuid():N}.bin");
        byte[] payload = Encoding.UTF8.GetBytes("atriswave-installer-payload");
        await File.WriteAllBytesAsync(tempFile, payload);

        try
        {
            string expectedHex = Convert.ToHexStringLower(SHA256.HashData(payload));
            string sumsText = $"{expectedHex}  AtrisWave-Setup-2.4.0-alpha.exe\n";

            IReadOnlyDictionary<string, string> parsed = AppUpdaterService.ParseSha256Sums(sumsText);
            Assert.True(parsed.TryGetValue("AtrisWave-Setup-2.4.0-alpha.exe", out string? hash));
            Assert.Equal(expectedHex, hash);

            Assert.True(await AppUpdaterService.VerifyFileSha256Async(tempFile, expectedHex));
            Assert.False(await AppUpdaterService.VerifyFileSha256Async(tempFile, new string('0', 64)));
        }
        finally
        {
            if (File.Exists(tempFile))
            {
                File.Delete(tempFile);
            }
        }
    }

    [Theory]
    [InlineData("https://github.com/merteren97/AtrisWave/releases/download/v2.3.0-alpha/AtrisWave-Setup.exe", true)]
    [InlineData("https://objects.githubusercontent.com/github-production-release-asset/foo.exe", true)]
    [InlineData("http://github.com/merteren97/AtrisWave/foo.exe", false)]
    [InlineData("https://evil.example.com/AtrisWave-Setup.exe", false)]
    [InlineData("https://notgithub.com/AtrisWave-Setup.exe", false)]
    public void IsTrustedGitHubUrl_EnforcesHttpsAndGitHubHosts(string url, bool expectedTrusted)
    {
        Assert.Equal(expectedTrusted, AppUpdaterService.IsTrustedGitHubUrl(url));
    }

    [Fact]
    public async Task DownloadAndVerifyInstallerAsync_RejectsUntrustedUrls()
    {
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            AppUpdaterService.DownloadAndVerifyInstallerAsync(
                "http://github.com/merteren97/AtrisWave/setup.exe",
                null,
                "v2.3.0-alpha"));

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            AppUpdaterService.DownloadAndVerifyInstallerAsync(
                "https://github.com/merteren97/AtrisWave/setup.exe",
                "https://evil.example.com/SHA256SUMS.txt",
                "v2.3.0-alpha"));
    }
}
