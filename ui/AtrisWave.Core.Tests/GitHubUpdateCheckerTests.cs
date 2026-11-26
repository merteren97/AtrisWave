using System.Security.Cryptography;
using System.Text;
using AtrisWave.Core.Services;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class GitHubUpdateCheckerTests
{
    [Theory]
    [InlineData("0.1.0-alpha", "v0.1.0", true)]
    [InlineData("0.1.0", "v0.1.1", true)]
    [InlineData("0.1.0", "v0.2.0", true)]
    [InlineData("0.1.0", "v0.1.0", false)]
    [InlineData("0.1.0", "v0.1.0-rc1", false)]
    [InlineData("0.2.0", "v0.1.9", false)]
    public void IsNewerVersion_ComparesSemVerCorrectly(string current, string latest, bool expectedNewer)
    {
        Assert.Equal(expectedNewer, GitHubUpdateChecker.IsNewerVersion(current, latest));
    }

    [Fact]
    public void ParseReleasesJson_SkipsDraftsAndPrereleases_ExtractsStableInstallerAndChecksums()
    {
        const string json = """
            [
              {
                "tag_name": "v0.3.0",
                "draft": true,
                "prerelease": false,
                "html_url": "https://github.com/merteren97/AtrisWave/releases/tag/v0.3.0",
                "assets": []
              },
              {
                "tag_name": "v0.2.0-alpha",
                "draft": false,
                "prerelease": true,
                "html_url": "https://github.com/merteren97/AtrisWave/releases/tag/v0.2.0-alpha",
                "assets": []
              },
              {
                "tag_name": "v0.1.1",
                "draft": false,
                "prerelease": false,
                "html_url": "https://github.com/merteren97/AtrisWave/releases/tag/v0.1.1",
                "body": "Release notes here",
                "assets": [
                  {
                    "name": "AtrisWave-Setup-0.1.1-win-x64.exe",
                    "browser_download_url": "https://example.com/AtrisWave-Setup-0.1.1-win-x64.exe"
                  },
                  {
                    "name": "SHA256SUMS.txt",
                    "browser_download_url": "https://example.com/SHA256SUMS.txt"
                  }
                ]
              }
            ]
            """;

        UpdateCheckResult res = GitHubUpdateChecker.ParseReleasesJson(json, "0.1.0");
        Assert.True(res.Success);
        Assert.True(res.UpdateAvailable);
        Assert.Equal("v0.1.1", res.LatestVersion);
        Assert.False(res.IsPrerelease);
        Assert.Equal("https://example.com/AtrisWave-Setup-0.1.1-win-x64.exe", res.InstallerDownloadUrl);
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
            string sumsText = $"{expectedHex}  AtrisWave-Setup-0.1.1-win-x64.exe\n";

            IReadOnlyDictionary<string, string> parsed = AppUpdaterService.ParseSha256Sums(sumsText);
            Assert.True(parsed.TryGetValue("AtrisWave-Setup-0.1.1-win-x64.exe", out string? hash));
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
    [InlineData("https://github.com/merteren97/AtrisWave/releases/download/v0.1.0/AtrisWave-Setup.exe", true)]
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
                "v0.1.0"));

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            AppUpdaterService.DownloadAndVerifyInstallerAsync(
                "https://github.com/merteren97/AtrisWave/setup.exe",
                "https://evil.example.com/SHA256SUMS.txt",
                "v0.1.0"));
    }
}
