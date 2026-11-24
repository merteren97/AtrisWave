using System.Net.Http;
using System.Text.Json;

namespace AtrisWave.Core.Services;

public sealed record UpdateCheckResult(bool Success, bool UpdateAvailable, string LatestVersion, string ReleaseUrl, string? Error = null);

/// <summary>
/// Checks GitHub Releases for newer AtrisWave versions using SemVer comparison.
/// </summary>
public static class GitHubUpdateChecker
{
    private const string LatestReleaseUrl = "https://api.github.com/repos/merteren97/AtrisWave/releases/latest";

    public static async Task<UpdateCheckResult> CheckForUpdatesAsync(
        string currentVersion,
        HttpClient? httpClient = null,
        CancellationToken cancellationToken = default)
    {
        bool ownsClient = httpClient is null;
        HttpClient client = httpClient ?? new HttpClient { Timeout = TimeSpan.FromSeconds(5) };

        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, LatestReleaseUrl);
            req.Headers.UserAgent.ParseAdd("AtrisWave/2.1");

            using HttpResponseMessage resp = await client.SendAsync(req, cancellationToken).ConfigureAwait(false);
            if (!resp.IsSuccessStatusCode)
            {
                return new UpdateCheckResult(false, false, currentVersion, string.Empty, $"HTTP {(int)resp.StatusCode}");
            }

            string json = await resp.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement root = doc.RootElement;

            string tag = root.TryGetProperty("tag_name", out JsonElement tagEl) && tagEl.ValueKind == JsonValueKind.String
                ? tagEl.GetString() ?? currentVersion
                : currentVersion;

            string htmlUrl = root.TryGetProperty("html_url", out JsonElement urlEl) && urlEl.ValueKind == JsonValueKind.String
                ? urlEl.GetString() ?? string.Empty
                : string.Empty;

            bool newer = IsNewerVersion(currentVersion, tag);
            return new UpdateCheckResult(true, newer, tag, htmlUrl);
        }
        catch (Exception ex)
        {
            return new UpdateCheckResult(false, false, currentVersion, string.Empty, ex.Message);
        }
        finally
        {
            if (ownsClient)
            {
                client.Dispose();
            }
        }
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

        // Stable release (empty prerelease) > prerelease of same Major.Minor.Patch
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
