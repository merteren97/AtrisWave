using System.Text.Json.Serialization;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AtrisWave.Core.Models;

/// <summary>
/// Hardware / Virtual Master Output Bus model (<c>A1</c>, <c>A2</c>, <c>B1</c>, <c>B2</c>).
/// </summary>
public sealed class OutputBusModel : ObservableObject
{
    private string _deviceId = string.Empty;
    private string _deviceName = string.Empty;
    private bool _isVirtual;
    private float _volume = 1.0f;
    private bool _muted;
    private bool _active;
    private float _peakLeft;
    private float _peakRight;
    private float _rmsLeft;
    private float _rmsRight;
    private bool _isClipping;

    [JsonPropertyName("bus_id")]
    public string BusId { get; set; } = "A1";

    [JsonPropertyName("role")]
    public string Role { get; set; } = "A1";

    [JsonPropertyName("name")]
    public string Name { get; set; } = string.Empty;

    [JsonPropertyName("device_id")]
    public string DeviceId
    {
        get => _deviceId;
        set => SetProperty(ref _deviceId, value);
    }

    [JsonPropertyName("device_name")]
    public string DeviceName
    {
        get => _deviceName;
        set => SetProperty(ref _deviceName, value);
    }

    [JsonPropertyName("is_virtual")]
    public bool IsVirtual
    {
        get => _isVirtual;
        set => SetProperty(ref _isVirtual, value);
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

    [JsonPropertyName("active")]
    public bool Active
    {
        get => _active;
        set => SetProperty(ref _active, value);
    }

    [JsonIgnore] public float PeakLeft { get => _peakLeft; set => SetProperty(ref _peakLeft, value); }
    [JsonIgnore] public float PeakRight { get => _peakRight; set => SetProperty(ref _peakRight, value); }
    [JsonIgnore] public float RmsLeft { get => _rmsLeft; set => SetProperty(ref _rmsLeft, value); }
    [JsonIgnore] public float RmsRight { get => _rmsRight; set => SetProperty(ref _rmsRight, value); }
    [JsonIgnore] public bool IsClipping { get => _isClipping; set => SetProperty(ref _isClipping, value); }

    [JsonIgnore]
    public string RoleNameKey => BusId switch
    {
        "A1" => "Bus_A1_Name",
        "A2" => "Bus_A2_Name",
        "B1" => "Bus_B1_Name",
        _ => "Bus_B2_Name",
    };

    [JsonIgnore]
    public string RoleDescKey => BusId switch
    {
        "A1" => "Bus_A1_Desc",
        "A2" => "Bus_A2_Desc",
        "B1" => "Bus_B1_Desc",
        _ => "Bus_B2_Desc",
    };

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
