using AtrisWave.Core.Models;

namespace AtrisWave.Core.Dsp;

public enum MicVoicePresetId
{
    RawClean = 0,
    NaturalClear = 1,
    PodcastRadio = 2,
    DiscordGaming = 3,
    StudioReverb = 4,
}

/// <summary>
/// Built-in one-click studio microphone voice presets.
/// </summary>
public static class MicVoicePresets
{
    public static void ApplyPreset(MicVoicePresetId preset, MicDspSettings s)
    {
        ArgumentNullException.ThrowIfNull(s);

        s.BypassAll = false;
        s.LimiterEnabled = true;
        s.LimiterCeilingDb = -0.5f;
        s.PitchEnabled = false;
        s.PitchSemitones = 0.0f;

        switch (preset)
        {
            case MicVoicePresetId.RawClean:
                s.DenoiseEnabled = false;
                s.GateEnabled = false;
                s.CompEnabled = false;
                s.EqEnabled = true;
                s.EqHpfHz = 75.0f;
                s.EqLowDb = 0.0f;
                s.EqMid1Db = 0.0f;
                s.EqMid2Db = 0.0f;
                s.EqHighDb = 0.0f;
                s.ReverbEnabled = false;
                break;

            case MicVoicePresetId.NaturalClear:
                s.DenoiseEnabled = true;
                s.DenoiseStrength = 0.25f;
                s.GateEnabled = false;
                s.EqEnabled = true;
                s.EqHpfHz = 75.0f;
                s.EqLowDb = 0.0f;
                s.EqMid1Db = 0.0f;
                s.EqMid2Hz = 3200.0f;
                s.EqMid2Db = 1.5f;
                s.EqHighDb = 0.0f;
                s.CompEnabled = true;
                s.CompThresholdDb = -20.0f;
                s.CompRatio = 2.2f;
                s.CompMakeupDb = 1.5f;
                s.ReverbEnabled = false;
                break;

            case MicVoicePresetId.PodcastRadio:
                s.DenoiseEnabled = true;
                s.DenoiseStrength = 0.20f;
                s.GateEnabled = false;
                s.EqEnabled = true;
                s.EqHpfHz = 80.0f;
                s.EqLowDb = 3.0f;
                s.EqMid1Hz = 380.0f;
                s.EqMid1Db = -2.0f;
                s.EqMid2Hz = 3200.0f;
                s.EqMid2Db = 2.0f;
                s.EqHighDb = 0.5f;
                s.CompEnabled = true;
                s.CompThresholdDb = -18.0f;
                s.CompRatio = 3.0f;
                s.CompMakeupDb = 2.5f;
                s.ReverbEnabled = false;
                break;

            case MicVoicePresetId.DiscordGaming:
                s.DenoiseEnabled = true;
                s.DenoiseStrength = 0.40f;
                s.GateEnabled = true;
                s.GateThresholdDb = -42.0f;
                s.EqEnabled = true;
                s.EqHpfHz = 95.0f;
                s.EqLowDb = 0.0f;
                s.EqMid1Db = 0.0f;
                s.EqMid2Hz = 2800.0f;
                s.EqMid2Db = 2.5f;
                s.EqHighDb = 0.0f;
                s.CompEnabled = true;
                s.CompThresholdDb = -18.0f;
                s.CompRatio = 3.0f;
                s.CompMakeupDb = 2.0f;
                s.ReverbEnabled = false;
                break;

            case MicVoicePresetId.StudioReverb:
                ApplyPreset(MicVoicePresetId.NaturalClear, s);
                s.ReverbEnabled = true;
                s.ReverbMode = 1; // StudioBooth
                s.ReverbWet = 0.18f;
                break;
        }
    }
}
