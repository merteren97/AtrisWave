using System.Text.Json.Serialization;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AtrisWave.Core.Models;

/// <summary>
/// Active Windows audio session metadata returned by the engine's <c>list_sessions</c> command.
/// </summary>
public sealed class AudioSessionInfo : ObservableObject
{
    private float _volume = 1.0f;
    private bool _isMuted;
    private string _targetDeviceId = string.Empty;
    private bool _loopbackCapture;

    [JsonPropertyName("pid")]
    public uint Pid { get; set; }

    [JsonPropertyName("process_name")]
    public string AppName { get; set; } = string.Empty;

    [JsonPropertyName("display_name")]
    public string DisplayName { get; set; } = string.Empty;

    [JsonPropertyName("volume")]
    public float Volume
    {
        get => _volume;
        set => SetProperty(ref _volume, value);
    }

    [JsonPropertyName("muted")]
    public bool IsMuted
    {
        get => _isMuted;
        set => SetProperty(ref _isMuted, value);
    }

    [JsonPropertyName("target_device_id")]
    public string TargetDeviceId
    {
        get => _targetDeviceId;
        set => SetProperty(ref _targetDeviceId, value);
    }

    [JsonPropertyName("loopback_capture")]
    public bool LoopbackCapture
    {
        get => _loopbackCapture;
        set => SetProperty(ref _loopbackCapture, value);
    }

    public string EffectiveLabel => string.IsNullOrWhiteSpace(DisplayName) ? AppName : DisplayName;
}
