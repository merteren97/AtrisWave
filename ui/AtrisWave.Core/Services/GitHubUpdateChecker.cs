using System.Net.Http;
using System.Text.Json;

namespace AtrisWave.Core.Services;

public sealed record UpdateCheckResult(
    bool Success,
    bool UpdateAvailable,
    string LatestVersion,
    string ReleaseUrl,
    string? InstallerDownloadUrl = null,
    string? ChecksumsDownloadUrl = null,
    string? ReleaseNotes = null,
    bool IsPrerelease = false,
    string? Error = null);

/// <summary>
/// Checks GitHub Releases (stable releases only) for newer AtrisWave versions using SemVer comparison.
/// </summary>
public static class GitHubUpdateChecker
{
    private const string ReleasesApiUrl = "https://api.github.com/repos/merteren97/AtrisWave/releases?per_page=10";

    public static async Task<UpdateCheckResult> CheckForUpdatesAsync(
        string currentVersion,
        HttpClient? httpClient = null,
        CancellationToken cancellationToken = default)
    {
        bool ownsClient = httpClient is null;
        HttpClient client = httpClient ?? new HttpClient { Timeout = TimeSpan.FromSeconds(5) };

        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, ReleasesApiUrl);
            req.Headers.UserAgent.ParseAdd("AtrisWave/0.1");
            req.Headers.Accept.ParseAdd("application/vnd.github+json");

            using HttpResponseMessage resp = await client.SendAsync(req, cancellationToken).ConfigureAwait(false);
            if (!resp.IsSuccessStatusCode)
            {
                return new UpdateCheckResult(false, false, currentVersion, string.Empty, Error: $"HTTP {(int)resp.StatusCode}");
            }

            string json = await resp.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            return ParseReleasesJson(json, currentVersion);
        }
        catch (Exception ex)
        {
            return new UpdateCheckResult(false, false, currentVersion, string.Empty, Error: ex.Message);
        }
        finally
        {
            if (ownsClient)
            {
                client.Dispose();
            }
        }
    }

    public static UpdateCheckResult ParseReleasesJson(string json, string currentVersion)
    {
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement root = doc.RootElement;

        if (root.ValueKind != JsonValueKind.Array || root.GetArrayLength() == 0)
        {
            return new UpdateCheckResult(true, false, currentVersion, string.Empty);
        }

        string? bestTag = null;
        string bestUrl = string.Empty;
        string? bestInstallerUrl = null;
        string? bestChecksumsUrl = null;
        string? bestNotes = null;
        bool bestPrerelease = false;

        foreach (JsonElement release in root.EnumerateArray())
        {
            if (release.TryGetProperty("draft", out JsonElement draftEl) && draftEl.ValueKind == JsonValueKind.True)
            {
                continue;
            }

            if (release.TryGetProperty("prerelease", out JsonElement skipPreEl) && skipPreEl.ValueKind == JsonValueKind.True)
            {
                continue;
            }

            string? tag = release.TryGetProperty("tag_name", out JsonElement tagEl) && tagEl.ValueKind == JsonValueKind.String
                ? tagEl.GetString()
                : null;
            if (string.IsNullOrWhiteSpace(tag))
            {
                continue;
            }

            if (bestTag is not null && !IsNewerVersion(bestTag, tag))
            {
                continue;
            }

            bestTag = tag;
            bestUrl = release.TryGetProperty("html_url", out JsonElement urlEl) && urlEl.ValueKind == JsonValueKind.String
                ? urlEl.GetString() ?? string.Empty
                : string.Empty;
            bestNotes = release.TryGetProperty("body", out JsonElement bodyEl) && bodyEl.ValueKind == JsonValueKind.String
                ? bodyEl.GetString()
                : null;
            bestPrerelease = release.TryGetProperty("prerelease", out JsonElement preEl) && preEl.ValueKind == JsonValueKind.True;
            bestInstallerUrl = null;
            bestChecksumsUrl = null;

            if (release.TryGetProperty("assets", out JsonElement assetsEl) && assetsEl.ValueKind == JsonValueKind.Array)
            {
                foreach (JsonElement asset in assetsEl.EnumerateArray())
                {
                    string name = asset.TryGetProperty("name", out JsonElement nameEl) ? nameEl.GetString() ?? string.Empty : string.Empty;
                    string dlUrl = asset.TryGetProperty("browser_download_url", out JsonElement dlEl) ? dlEl.GetString() ?? string.Empty : string.Empty;

                    if (name.StartsWith("AtrisWave-Setup-", StringComparison.OrdinalIgnoreCase) &&
                        name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                    {
                        bestInstallerUrl = dlUrl;
                    }
                    else if (bestInstallerUrl is null && name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                    {
                        bestInstallerUrl = dlUrl;
                    }
                    else if (name.Equals("SHA256SUMS.txt", StringComparison.OrdinalIgnoreCase))
                    {
                        bestChecksumsUrl = dlUrl;
                    }
                }
            }
        }

        if (bestTag is null)
        {
            return new UpdateCheckResult(true, false, currentVersion, string.Empty);
        }

        bool newer = IsNewerVersion(currentVersion, bestTag);
        return new UpdateCheckResult(
            Success: true,
            UpdateAvailable: newer,
            LatestVersion: bestTag,
            ReleaseUrl: bestUrl,
            InstallerDownloadUrl: bestInstallerUrl,
            ChecksumsDownloadUrl: bestChecksumsUrl,
            ReleaseNotes: bestNotes,
            IsPrerelease: bestPrerelease);
    }

    public static bool IsNewerVersion(string current, string latest)
    {
        if (!TryParseSemVer(current, out var cur) || !TryParseSemVer(latest, out var lat))
        {
            return false;
        }

        if (lat.Major != cur.Major) return lat.Major > cur.Major;
        if (lat.Minor != cur.Minor) return lat.Minor > cur.Minor;
        if (lat.Patch != cur.Patch) return lat.Patch > cur.Patch;

        if (string.IsNullOrEmpty(cur.Prerelease) && !string.IsNullOrEmpty(lat.Prerelease)) return false;
        if (!string.IsNullOrEmpty(cur.Prerelease) && string.IsNullOrEmpty(lat.Prerelease)) return true;

        return string.Compare(lat.Prerelease, cur.Prerelease, StringComparison.OrdinalIgnoreCase) > 0;
    }

    private static bool TryParseSemVer(string raw, out (int Major, int Minor, int Patch, string Prerelease) result)
    {
        result = default;
        if (string.IsNullOrWhiteSpace(raw))
        {
            return false;
        }

        string s = raw.Trim();
        if (s.StartsWith('v') || s.StartsWith('V'))
        {
            s = s[1..];
        }

        string prerelease = string.Empty;
        int dash = s.IndexOf('-');
        if (dash >= 0)
        {
            prerelease = s[(dash + 1)..];
            s = s[..dash];
        }

        string[] parts = s.Split('.');
        if (parts.Length < 2 ||
            !int.TryParse(parts[0], out int major) ||
            !int.TryParse(parts[1], out int minor))
        {
            return false;
        }

        int patch = parts.Length >= 3 && int.TryParse(parts[2], out int p) ? p : 0;
        result = (major, minor, patch, prerelease);
        return true;
    }
}
