using System.Globalization;
using System.Text.RegularExpressions;
using AtrisWave.Core.Models;

namespace AtrisWave.Core.Dsp;

public sealed record AutoEqFilter(string Type, float FrequencyHz, float GainDb, float Q);

public sealed record AutoEqProfile(float PreampDb, IReadOnlyList<AutoEqFilter> Filters);

/// <summary>
/// Parses standard AutoEq ParametricEQ <c>.txt</c> files and maps them onto <see cref="MicDspSettings"/>.
/// </summary>
public static partial class AutoEqParser
{
    [GeneratedRegex(@"^Preamp:\s*([+-]?\d+(?:\.\d+)?)\s*dB", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex PreampRegex();

    [GeneratedRegex(@"^Filter\s+\d+:\s*ON\s+(PK|LSC|HSC)\s+Fc\s+(\d+(?:\.\d+)?)\s*Hz\s+Gain\s+([+-]?\d+(?:\.\d+)?)\s*dB\s+Q\s+(\d+(?:\.\d+)?)", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex FilterRegex();

    public static AutoEqProfile Parse(string content)
    {
        ArgumentNullException.ThrowIfNull(content);

        float preampDb = 0.0f;
        var filters = new List<AutoEqFilter>();

        foreach (string rawLine in content.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries))
        {
            string line = rawLine.Trim();
            Match preampMatch = PreampRegex().Match(line);
            if (preampMatch.Success &&
                float.TryParse(preampMatch.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out float preamp))
            {
                preampDb = preamp;
                continue;
            }

            Match filterMatch = FilterRegex().Match(line);
            if (filterMatch.Success &&
                float.TryParse(filterMatch.Groups[2].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out float freq) &&
                float.TryParse(filterMatch.Groups[3].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out float gain) &&
                float.TryParse(filterMatch.Groups[4].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out float q))
            {
                filters.Add(new AutoEqFilter(filterMatch.Groups[1].Value.ToUpperInvariant(), freq, gain, q));
            }
        }

        return new AutoEqProfile(preampDb, filters);
    }

    public static void ApplyTo(AutoEqProfile profile, MicDspSettings settings)
    {
        ArgumentNullException.ThrowIfNull(profile);
        ArgumentNullException.ThrowIfNull(settings);

        AutoEqFilter? lowShelf = profile.Filters.FirstOrDefault(f => f.Type == "LSC");
        AutoEqFilter? highShelf = profile.Filters.FirstOrDefault(f => f.Type == "HSC");

        List<AutoEqFilter> peaks = profile.Filters
            .Where(f => f.Type == "PK")
            .OrderByDescending(f => Math.Abs(f.GainDb))
            .Take(2)
            .OrderBy(f => f.FrequencyHz)
            .ToList();

        settings.EqEnabled = true;
        settings.EqLowDb = lowShelf is not null ? Math.Clamp(lowShelf.GainDb, -15.0f, 15.0f) : 0.0f;
        settings.EqHighDb = highShelf is not null ? Math.Clamp(highShelf.GainDb, -15.0f, 15.0f) : 0.0f;

        if (peaks.Count >= 1)
        {
            settings.EqMid1Hz = Math.Clamp(peaks[0].FrequencyHz, 150.0f, 2000.0f);
            settings.EqMid1Db = Math.Clamp(peaks[0].GainDb, -15.0f, 15.0f);
        }

        if (peaks.Count >= 2)
        {
            settings.EqMid2Hz = Math.Clamp(peaks[1].FrequencyHz, 1000.0f, 8000.0f);
            settings.EqMid2Db = Math.Clamp(peaks[1].GainDb, -15.0f, 15.0f);
        }
    }
}
