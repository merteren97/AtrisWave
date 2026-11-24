using AtrisWave.Core.Dsp;
using AtrisWave.Core.Models;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class AutoEqParserTests
{
    [Fact]
    public void ParseAndApply_MapsPreampAndPkLscHscFiltersCorrectly()
    {
        const string sample = """
            Preamp: -6.2 dB
            Filter 1: ON LSC Fc 120 Hz Gain 3.5 dB Q 0.71
            Filter 2: ON PK Fc 450 Hz Gain -4.0 dB Q 1.40
            Filter 3: ON PK Fc 3200 Hz Gain 2.8 dB Q 1.20
            Filter 4: ON HSC Fc 8000 Hz Gain 4.5 dB Q 0.71
            """;

        AutoEqProfile profile = AutoEqParser.Parse(sample);
        Assert.Equal(-6.2f, profile.PreampDb, precision: 2);
        Assert.Equal(4, profile.Filters.Count);

        var settings = new MicDspSettings { EqEnabled = false };
        AutoEqParser.ApplyTo(profile, settings);

        Assert.True(settings.EqEnabled);
        Assert.Equal(3.5f, settings.EqLowDb, precision: 2);
        Assert.Equal(4.5f, settings.EqHighDb, precision: 2);
        Assert.Equal(450.0f, settings.EqMid1Hz, precision: 1);
        Assert.Equal(-4.0f, settings.EqMid1Db, precision: 2);
        Assert.Equal(3200.0f, settings.EqMid2Hz, precision: 1);
        Assert.Equal(2.8f, settings.EqMid2Db, precision: 2);
    }
}
