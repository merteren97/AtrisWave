using System.Text.Json.Serialization;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AtrisWave.Core.Models;

/// <summary>
/// Observable Dual-Path Microphone DSP configuration with clean studio defaults.
/// </summary>
public sealed partial class MicDspSettings : ObservableObject
{
    [ObservableProperty] private bool _directMonitor = true;
    [ObservableProperty] private bool _monitorProcessed = true;
    [ObservableProperty] private bool _bypassAll = false;
    [ObservableProperty] private float _monitorGain = 1.0f;

    [ObservableProperty] private bool _denoiseEnabled = false;
    [ObservableProperty] private float _denoiseStrength = 0.25f;

    [ObservableProperty] private bool _gateEnabled = false;
    [ObservableProperty] private float _gateThresholdDb = -45.0f;

    [ObservableProperty] private bool _eqEnabled = true;
    [ObservableProperty] private float _eqHpfHz = 75.0f;
    [ObservableProperty] private float _eqLowDb = 0.0f;
    [ObservableProperty] private float _eqMid1Hz = 500.0f;
    [ObservableProperty] private float _eqMid1Db = 0.0f;
    [ObservableProperty] private float _eqMid2Hz = 3000.0f;
    [ObservableProperty] private float _eqMid2Db = 0.0f;
    [ObservableProperty] private float _eqHighDb = 0.0f;

    [ObservableProperty] private bool _compEnabled = false;
    [ObservableProperty] private float _compThresholdDb = -20.0f;
    [ObservableProperty] private float _compRatio = 2.5f;
    [ObservableProperty] private float _compMakeupDb = 1.5f;

    [ObservableProperty] private bool _duckingEnabled = false;
    [ObservableProperty] private float _duckingDepthDb = -12.0f;

    [ObservableProperty] private bool _reverbEnabled = false;
    [ObservableProperty] private int _reverbMode = 0; // 0=Algorithmic, 1=StudioBooth, 2=BroadcastRoom, 3=ConcertHall
    [ObservableProperty] private float _reverbWet = 0.15f;
    [ObservableProperty] private float _reverbRoom = 0.45f;
    [ObservableProperty] private float _reverbDamp = 0.50f;

    [ObservableProperty] private bool _pitchEnabled = false;
    [ObservableProperty] private float _pitchSemitones = 0.0f;

    [ObservableProperty] private bool _limiterEnabled = true;
    [ObservableProperty] private float _limiterCeilingDb = -0.5f;

    public MicDspWireDto ToWireDto() => new()
    {
        DirectMonitor = DirectMonitor,
        MonitorProcessed = MonitorProcessed,
        BypassAll = BypassAll,
        MonitorGain = MonitorGain,
        DenoiseEnabled = DenoiseEnabled,
        DenoiseStrength = DenoiseStrength,
        GateEnabled = GateEnabled,
        GateThresholdDb = GateThresholdDb,
        EqEnabled = EqEnabled,
        EqHpfHz = EqHpfHz,
        EqLowDb = EqLowDb,
        EqMid1Hz = EqMid1Hz,
        EqMid1Db = EqMid1Db,
        EqMid2Hz = EqMid2Hz,
        EqMid2Db = EqMid2Db,
        EqHighDb = EqHighDb,
        CompEnabled = CompEnabled,
        CompThresholdDb = CompThresholdDb,
        CompRatio = CompRatio,
        CompMakeupDb = CompMakeupDb,
        DuckingEnabled = DuckingEnabled,
        DuckingDepthDb = DuckingDepthDb,
        ReverbEnabled = ReverbEnabled,
        ReverbMode = ReverbMode,
        ReverbWet = ReverbWet,
        ReverbRoom = ReverbRoom,
        ReverbDamp = ReverbDamp,
        PitchEnabled = PitchEnabled,
        PitchSemitones = PitchSemitones,
        LimiterEnabled = LimiterEnabled,
        LimiterCeilingDb = LimiterCeilingDb,
    };
}

/// <summary>
/// Snake-case JSON wire DTO matching the engine's <c>set_mic_dsp</c> command.
/// </summary>
public sealed class MicDspWireDto
{
    [JsonPropertyName("direct_monitor")] public bool DirectMonitor { get; set; }
    [JsonPropertyName("monitor_processed")] public bool MonitorProcessed { get; set; }
    [JsonPropertyName("bypass_all")] public bool BypassAll { get; set; }
    [JsonPropertyName("monitor_gain")] public float MonitorGain { get; set; }

    [JsonPropertyName("denoise_enabled")] public bool DenoiseEnabled { get; set; }
    [JsonPropertyName("denoise_strength")] public float DenoiseStrength { get; set; }

    [JsonPropertyName("gate_enabled")] public bool GateEnabled { get; set; }
    [JsonPropertyName("gate_threshold_db")] public float GateThresholdDb { get; set; }

    [JsonPropertyName("eq_enabled")] public bool EqEnabled { get; set; }
    [JsonPropertyName("eq_hpf_hz")] public float EqHpfHz { get; set; }
    [JsonPropertyName("eq_low_db")] public float EqLowDb { get; set; }
    [JsonPropertyName("eq_mid1_hz")] public float EqMid1Hz { get; set; }
    [JsonPropertyName("eq_mid1_db")] public float EqMid1Db { get; set; }
    [JsonPropertyName("eq_mid2_hz")] public float EqMid2Hz { get; set; }
    [JsonPropertyName("eq_mid2_db")] public float EqMid2Db { get; set; }
    [JsonPropertyName("eq_high_db")] public float EqHighDb { get; set; }

    [JsonPropertyName("comp_enabled")] public bool CompEnabled { get; set; }
    [JsonPropertyName("comp_threshold_db")] public float CompThresholdDb { get; set; }
    [JsonPropertyName("comp_ratio")] public float CompRatio { get; set; }
    [JsonPropertyName("comp_makeup_db")] public float CompMakeupDb { get; set; }

    [JsonPropertyName("ducking_enabled")] public bool DuckingEnabled { get; set; }
    [JsonPropertyName("ducking_depth_db")] public float DuckingDepthDb { get; set; }

    [JsonPropertyName("reverb_enabled")] public bool ReverbEnabled { get; set; }
    [JsonPropertyName("reverb_mode")] public int ReverbMode { get; set; }
    [JsonPropertyName("reverb_wet")] public float ReverbWet { get; set; }
    [JsonPropertyName("reverb_room")] public float ReverbRoom { get; set; }
    [JsonPropertyName("reverb_damp")] public float ReverbDamp { get; set; }

    [JsonPropertyName("pitch_enabled")] public bool PitchEnabled { get; set; }
    [JsonPropertyName("pitch_semitones")] public float PitchSemitones { get; set; }

    [JsonPropertyName("limiter_enabled")] public bool LimiterEnabled { get; set; }
    [JsonPropertyName("limiter_ceiling_db")] public float LimiterCeilingDb { get; set; }
}
