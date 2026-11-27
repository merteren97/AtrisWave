using System.Collections.ObjectModel;
using System.Text.Json.Serialization;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AtrisWave.Core.Models;

/// <summary>
/// Draggable app tag displayed in a channel's bottom Apps tray.
/// </summary>
public sealed class RoutedAppChip : ObservableObject
{
    private bool _isLiveActive;

    public uint ChannelId { get; init; }
    public string ProcessName { get; init; } = string.Empty;
    public string DisplayLabel => AudioSessionInfo.FormatProcessLabel(ProcessName);

    public bool IsLiveActive
    {
        get => _isLiveActive;
        set => SetProperty(ref _isLiveActive, value);
    }
}

/// <summary>
/// Dynamic mixer channel strip model (up to 16 channels: System, Microphone, Game, Chat, and user-created channels).
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
    private float _eqLowDb;
    private float _eqMidDb;
    private float _eqHighDb;
    private bool _compEnabled;
    private float _compThresholdDb = -18.0f;
    private int _duckSourceId = -1;
    private float _duckDepthDb = -12.0f;

    [JsonPropertyName("channel_id")]
    public uint ChannelId { get; set; }

    [JsonPropertyName("name")]
    public string Name
    {
        get => _name;
        set
        {
            if (SetProperty(ref _name, value))
            {
                OnPropertyChanged(nameof(IconKind));
                OnPropertyChanged(nameof(VirtualBusLabel));
            }
        }
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
    public ObservableCollection<RoutedAppChip> BoundAppChips { get; } = [];

    [JsonIgnore]
    public bool HasNoBoundApps => BoundAppChips.Count == 0;

    public void NotifyBoundAppsChanged() => OnPropertyChanged(nameof(HasNoBoundApps));

    [JsonIgnore]
    public string IconKind
    {
        get
        {
            if (IsSystem || ChannelId == 0) return "System";
            if (IsHardwareInput || ChannelId == 1) return "Mic";
            if (Name.Contains("game", StringComparison.OrdinalIgnoreCase) || Name.Contains("oyun", StringComparison.OrdinalIgnoreCase)) return "Game";
            if (Name.Contains("chat", StringComparison.OrdinalIgnoreCase) || Name.Contains("sohbet", StringComparison.OrdinalIgnoreCase) || Name.Contains("discord", StringComparison.OrdinalIgnoreCase)) return "Chat";
            if (Name.Contains("media", StringComparison.OrdinalIgnoreCase) || Name.Contains("medya", StringComparison.OrdinalIgnoreCase) || Name.Contains("music", StringComparison.OrdinalIgnoreCase)) return "Media";
            if (Name.Contains("aux", StringComparison.OrdinalIgnoreCase) || Name.Contains("yard", StringComparison.OrdinalIgnoreCase)) return "Aux";
            return "Custom";
        }
    }

    [JsonIgnore]
    public string VirtualBusLabel => IconKind switch
    {
        "System" => "AtrisWave System",
        "Game" => "AtrisWave Game",
        "Chat" => "AtrisWave Chat",
        "Media" => "AtrisWave Media",
        "Aux" => "AtrisWave Aux",
        "Mic" => "AtrisWave Mic",
        _ => $"AtrisWave {Name}",
    };

    [JsonIgnore]
    public bool EchoLockedA1A2 => string.Equals(SourceType, "process_loopback", StringComparison.OrdinalIgnoreCase);

    [JsonIgnore]
    public bool IsHardwareInput => string.Equals(SourceType, "hardware_input", StringComparison.OrdinalIgnoreCase);

    [JsonIgnore]
    public bool CanDelete => !IsSystem && !IsHardwareInput && ChannelId > 3;

    [JsonIgnore] public float PeakLeft { get => _peakLeft; set => SetProperty(ref _peakLeft, value); }
    [JsonIgnore] public float PeakRight { get => _peakRight; set => SetProperty(ref _peakRight, value); }
    [JsonIgnore] public float RmsLeft { get => _rmsLeft; set => SetProperty(ref _rmsLeft, value); }
    [JsonIgnore] public float RmsRight { get => _rmsRight; set => SetProperty(ref _rmsRight, value); }
    [JsonIgnore] public float GainReductionDb { get => _gainReductionDb; set => SetProperty(ref _gainReductionDb, value); }
    [JsonIgnore] public bool IsClipping { get => _isClipping; set => SetProperty(ref _isClipping, value); }
    [JsonIgnore] public bool IsActive { get => _isActive; set => SetProperty(ref _isActive, value); }
    [JsonIgnore] public float EqLowDb { get => _eqLowDb; set => SetProperty(ref _eqLowDb, value); }
    [JsonIgnore] public float EqMidDb { get => _eqMidDb; set => SetProperty(ref _eqMidDb, value); }
    [JsonIgnore] public float EqHighDb { get => _eqHighDb; set => SetProperty(ref _eqHighDb, value); }
    [JsonIgnore] public bool CompEnabled { get => _compEnabled; set => SetProperty(ref _compEnabled, value); }
    [JsonIgnore] public float CompThresholdDb { get => _compThresholdDb; set => SetProperty(ref _compThresholdDb, value); }
    [JsonIgnore] public int DuckSourceId { get => _duckSourceId; set => SetProperty(ref _duckSourceId, value); }
    [JsonIgnore] public float DuckDepthDb { get => _duckDepthDb; set => SetProperty(ref _duckDepthDb, value); }

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
