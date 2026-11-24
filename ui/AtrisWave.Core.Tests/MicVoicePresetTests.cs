using AtrisWave.Core.Dsp;
using AtrisWave.Core.Models;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class MicVoicePresetTests
{
    [Theory]
    [InlineData(MicVoicePresetId.RawClean, false, false, false, false)]
    [InlineData(MicVoicePresetId.NaturalClear, true, false, true, false)]
    [InlineData(MicVoicePresetId.PodcastRadio, true, false, true, false)]
    [InlineData(MicVoicePresetId.DiscordGaming, true, true, true, false)]
    [InlineData(MicVoicePresetId.StudioReverb, true, false, true, true)]
    public void ApplyPreset_SetsExpectedModuleStates(
        MicVoicePresetId preset,
        bool expectedDenoise,
        bool expectedGate,
        bool expectedComp,
        bool expectedReverb)
    {
        var settings = new MicDspSettings();
        MicVoicePresets.ApplyPreset(preset, settings);

        Assert.False(settings.BypassAll);
        Assert.True(settings.LimiterEnabled);
        Assert.Equal(expectedDenoise, settings.DenoiseEnabled);
        Assert.Equal(expectedGate, settings.GateEnabled);
        Assert.Equal(expectedComp, settings.CompEnabled);
        Assert.Equal(expectedReverb, settings.ReverbEnabled);
    }
}
