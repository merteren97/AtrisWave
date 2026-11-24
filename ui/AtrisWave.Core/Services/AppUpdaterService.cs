using System.Net.Http;
using System.Security.Cryptography;

namespace AtrisWave.Core.Services;

/// <summary>
/// Downloads and SHA-256 verifies AtrisWave setup installers from GitHub Releases.
/// </summary>
public static class AppUpdaterService
{
    public static bool IsTrustedGitHubUrl(string? url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out Uri? uri))
        {
            return false;
        }

        if (!string.Equals(uri.Scheme, Uri.UriSchemeHttps, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        return uri.Host.Equals("github.com", StringComparison.OrdinalIgnoreCase) ||
               uri.Host.EndsWith(".githubusercontent.com", StringComparison.OrdinalIgnoreCase);
    }

    public static IReadOnlyDictionary<string, string> ParseSha256Sums(string text)
    {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (string.IsNullOrWhiteSpace(text))
        {
            return map;
        }

        foreach (string rawLine in text.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries))
        {
            string line = rawLine.Trim();
            if (string.IsNullOrEmpty(line))
            {
                continue;
            }

            string[] parts = line.Split([' ', '\t'], StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length >= 2)
            {
                string hash = parts[0].Trim().ToLowerInvariant();
                string fileName = Path.GetFileName(parts[1].Trim().TrimStart('*'));
                if (!string.IsNullOrEmpty(fileName) && hash.Length == 64)
                {
                    map[fileName] = hash;
                }
            }
        }

        return map;
    }

    public static async Task<bool> VerifyFileSha256Async(
        string filePath,
        string expectedHexSha256,
        CancellationToken cancellationToken = default)
    {
        await using FileStream fs = File.OpenRead(filePath);
        byte[] hashBytes = await SHA256.HashDataAsync(fs, cancellationToken).ConfigureAwait(false);
        string actualHex = Convert.ToHexStringLower(hashBytes);
        return string.Equals(actualHex, expectedHexSha256.Trim(), StringComparison.OrdinalIgnoreCase);
    }

    public static async Task<string> DownloadAndVerifyInstallerAsync(
        string installerUrl,
        string? checksumsUrl,
        string versionTag,
        IProgress<double>? progress = null,
        HttpClient? httpClient = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(installerUrl);
        if (!IsTrustedGitHubUrl(installerUrl))
        {
            throw new InvalidOperationException("Untrusted installer URL; only https://github.com and https://*.githubusercontent.com are allowed.");
        }

        if (!string.IsNullOrWhiteSpace(checksumsUrl) && !IsTrustedGitHubUrl(checksumsUrl))
        {
            throw new InvalidOperationException("Untrusted checksums URL; only https://github.com and https://*.githubusercontent.com are allowed.");
        }

        string safeTag = string.Concat(versionTag.Select(c => char.IsLetterOrDigit(c) || c is '.' or '-' or '_' ? c : '_'));
        string updateDir = Path.Combine(Path.GetTempPath(), "AtrisWaveUpdates");
        Directory.CreateDirectory(updateDir);

        string assetName = Path.GetFileName(new Uri(installerUrl).AbsolutePath);
        if (string.IsNullOrWhiteSpace(assetName))
        {
            assetName = $"AtrisWave-Setup-{safeTag}.exe";
        }

        string targetPath = Path.Combine(updateDir, $"AtrisWave-Setup-{safeTag}.exe");

        bool ownsClient = httpClient is null;
        HttpClient client = httpClient ?? new HttpClient { Timeout = TimeSpan.FromMinutes(5) };

        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Get, installerUrl);
            req.Headers.UserAgent.ParseAdd("AtrisWave/2.3");

            using HttpResponseMessage resp = await client.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, cancellationToken).ConfigureAwait(false);
            resp.EnsureSuccessStatusCode();
            if (resp.RequestMessage?.RequestUri is Uri finalUri && !IsTrustedGitHubUrl(finalUri.ToString()))
            {
                throw new InvalidOperationException("Redirected installer URL is not a trusted GitHub host.");
            }

            long? totalBytes = resp.Content.Headers.ContentLength;
            await using Stream src = await resp.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
            await using (FileStream dst = File.Create(targetPath))
            {
                byte[] buffer = new byte[65536];
                long readTotal = 0;
                int read;
                while ((read = await src.ReadAsync(buffer, cancellationToken).ConfigureAwait(false)) > 0)
                {
                    await dst.WriteAsync(buffer.AsMemory(0, read), cancellationToken).ConfigureAwait(false);
                    readTotal += read;
                    if (totalBytes is > 0)
                    {
                        progress?.Report(Math.Clamp(readTotal / (double)totalBytes.Value, 0.0, 1.0));
                    }
                }
            }

            progress?.Report(1.0);

            if (!string.IsNullOrWhiteSpace(checksumsUrl))
            {
                using var sumReq = new HttpRequestMessage(HttpMethod.Get, checksumsUrl);
                sumReq.Headers.UserAgent.ParseAdd("AtrisWave/2.3");
                using HttpResponseMessage sumResp = await client.SendAsync(sumReq, cancellationToken).ConfigureAwait(false);
                sumResp.EnsureSuccessStatusCode();
                if (sumResp.RequestMessage?.RequestUri is Uri sumFinalUri && !IsTrustedGitHubUrl(sumFinalUri.ToString()))
                {
                    throw new InvalidOperationException("Redirected checksums URL is not a trusted GitHub host.");
                }

                string sumText = await sumResp.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
                IReadOnlyDictionary<string, string> sums = ParseSha256Sums(sumText);

                string? expectedHash = null;
                if (!sums.TryGetValue(assetName, out expectedHash) && sums.Count == 1)
                {
                    expectedHash = sums.Values.First();
                }

                if (string.IsNullOrWhiteSpace(expectedHash))
                {
                    File.Delete(targetPath);
                    throw new InvalidOperationException($"No SHA-256 checksum entry found for {assetName}.");
                }

                bool valid = await VerifyFileSha256Async(targetPath, expectedHash, cancellationToken).ConfigureAwait(false);
                if (!valid)
                {
                    File.Delete(targetPath);
                    throw new InvalidOperationException("SHA-256 verification failed for downloaded installer.");
                }
            }

            return targetPath;
        }
        catch
        {
            if (File.Exists(targetPath))
            {
                try { File.Delete(targetPath); } catch { }
            }
            throw;
        }
        finally
        {
            if (ownsClient)
            {
                client.Dispose();
            }
        }
    }
}
