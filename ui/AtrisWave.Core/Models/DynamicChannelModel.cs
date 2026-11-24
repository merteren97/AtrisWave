using System.Collections.ObjectModel;
using System.Text.Json.Serialization;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AtrisWave.Core.Models;

/// <summary>
/// Dynamic mixer channel strip model (up to 16 channels: System, Microphone, and user-created channels).
/// </summary>
public sealed class DynamicChannelModel : ObservableObject
{
    private string _name = string.Empty;
    private string _sourceType = "app_group";
    private string _deviceId = string.Empty;
    private string _color = "#38BDF8";
    private bool _isSystem;
    private float _volume = 1.0f;
    private bool _muted;
    private bool _solo;
    private float _pan;
    private bool _sendA1 = true;
    private bool _sendA2;
    private bool _sendB1;
    private bool _sendB2 = true;
    private float _peakLeft;
    private float _peakRight;
    private float _rmsLeft;
    private float _rmsRight;
    private float _gainReductionDb;
    private bool _isClipping;
    private bool _isActive;

    [JsonPropertyName("channel_id")]
    public uint ChannelId { get; set; }

    [JsonPropertyName("name")]
    public string Name
    {
        get => _name;
        set => SetProperty(ref _name, value);
    }

    [JsonPropertyName("source_type")]
    public string SourceType
    {
        get => _sourceType;
        set
        {
            if (SetProperty(ref _sourceType, value))
            {
                OnPropertyChanged(nameof(EchoLockedA1A2));
                OnPropertyChanged(nameof(IsHardwareInput));
            }
        }
    }

    [JsonPropertyName("device_id")]
    public string DeviceId
    {
        get => _deviceId;
        set => SetProperty(ref _deviceId, value);
    }

    [JsonPropertyName("color")]
    public string Color
    {
        get => _color;
        set => SetProperty(ref _color, value);
    }

    [JsonPropertyName("is_system")]
    public bool IsSystem
    {
        get => _isSystem;
        set => SetProperty(ref _isSystem, value);
    }

    [JsonPropertyName("volume")]
    public float Volume
    {
        get => _volume;
        set
        {
            if (SetProperty(ref _volume, value))
            {
                OnPropertyChanged(nameof(VolumeDbText));
            }
        }
    }

    [JsonPropertyName("muted")]
    public bool Muted
    {
        get => _muted;
        set => SetProperty(ref _muted, value);
    }

    [JsonPropertyName("solo")]
    public bool Solo
    {
        get => _solo;
        set => SetProperty(ref _solo, value);
    }

    [JsonPropertyName("pan")]
    public float Pan
    {
        get => _pan;
        set => SetProperty(ref _pan, value);
    }

    [JsonPropertyName("send_a1")]
    public bool SendA1
    {
        get => _sendA1;
        set => SetProperty(ref _sendA1, value);
    }

    [JsonPropertyName("send_a2")]
    public bool SendA2
    {
        get => _sendA2;
        set => SetProperty(ref _sendA2, value);
    }

    [JsonPropertyName("send_b1")]
    public bool SendB1
    {
        get => _sendB1;
        set => SetProperty(ref _sendB1, value);
    }

    [JsonPropertyName("send_b2")]
    public bool SendB2
    {
        get => _sendB2;
        set => SetProperty(ref _sendB2, value);
    }

    [JsonPropertyName("bound_apps")]
    public ObservableCollection<string> BoundApps { get; set; } = [];

    [JsonIgnore]
    public bool EchoLockedA1A2 => string.Equals(SourceType, "process_loopback", StringComparison.OrdinalIgnoreCase);

    [JsonIgnore]
    public bool IsHardwareInput => string.Equals(SourceType, "hardware_input", StringComparison.OrdinalIgnoreCase);

    [JsonIgnore]
    public bool CanDelete => !IsSystem && ChannelId != 0;

    [JsonIgnore] public float PeakLeft { get => _peakLeft; set => SetProperty(ref _peakLeft, value); }
    [JsonIgnore] public float PeakRight { get => _peakRight; set => SetProperty(ref _peakRight, value); }
    [JsonIgnore] public float RmsLeft { get => _rmsLeft; set => SetProperty(ref _rmsLeft, value); }
    [JsonIgnore] public float RmsRight { get => _rmsRight; set => SetProperty(ref _rmsRight, value); }
    [JsonIgnore] public float GainReductionDb { get => _gainReductionDb; set => SetProperty(ref _gainReductionDb, value); }
    [JsonIgnore] public bool IsClipping { get => _isClipping; set => SetProperty(ref _isClipping, value); }
    [JsonIgnore] public bool IsActive { get => _isActive; set => SetProperty(ref _isActive, value); }

    [JsonIgnore]
    public string VolumeDbText
    {
        get
        {
            if (Volume <= 0.0001f)
            {
                return "-∞ dB";
            }

            double db = 20.0 * Math.Log10(Volume);
            return $"{db:+0.0;-0.0;0.0} dB";
        }
    }
}
