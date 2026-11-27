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
    private float _peak;
    private bool _isCapture;
    private bool _isActive;
    private string _deviceId = string.Empty;
    private string _targetDeviceId = string.Empty;
    private bool _loopbackCapture;

    [JsonPropertyName("pid")]
    public uint Pid { get; set; }

    [JsonPropertyName("process_name")]
    public string AppName { get; set; } = string.Empty;

    [JsonPropertyName("display_name")]
    public string DisplayName { get; set; } = string.Empty;

    [JsonPropertyName("device_id")]
    public string DeviceId
    {
        get => _deviceId;
        set => SetProperty(ref _deviceId, value);
    }

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

    [JsonPropertyName("peak")]
    public float Peak
    {
        get => _peak;
        set => SetProperty(ref _peak, value);
    }

    [JsonPropertyName("is_capture")]
    public bool IsCapture
    {
        get => _isCapture;
        set => SetProperty(ref _isCapture, value);
    }

    [JsonPropertyName("is_active")]
    public bool IsActive
    {
        get => _isActive;
        set => SetProperty(ref _isActive, value);
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

    public string ChipLabel => FormatProcessLabel(string.IsNullOrWhiteSpace(DisplayName) || DisplayName.StartsWith('@') ? AppName : DisplayName);

    public static string FormatProcessLabel(string raw)
    {
        if (string.IsNullOrWhiteSpace(raw))
        {
            return "App";
        }

        string trimmed = raw.Trim();
        if (trimmed.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) && trimmed.Length > 4)
        {
            trimmed = trimmed[..^4];
        }

        if (trimmed.Length > 0 && char.IsLower(trimmed[0]))
        {
            return char.ToUpperInvariant(trimmed[0]) + trimmed[1..];
        }

        return trimmed;
    }
}
