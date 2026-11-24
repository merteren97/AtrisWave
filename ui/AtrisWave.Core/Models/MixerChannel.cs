using CommunityToolkit.Mvvm.ComponentModel;

namespace AtrisWave.Core.Models;

/// <summary>
/// Canonical virtual mixer channels for Phase 0/1.
/// </summary>
public enum MixerChannelId : uint
{
    Master = 0,
    Game = 1,
    Chat = 2,
    Media = 3,
    Aux = 4,
    Microphone = 5,
}

/// <summary>
/// Observable model for a single virtual mixer channel strip.
/// </summary>
public partial class MixerChannel : ObservableObject
{
    public MixerChannel(MixerChannelId id, string nameKey, string accentHex)
    {
        Id = id;
        NameKey = nameKey;
        AccentHex = accentHex;
    }

    public MixerChannelId Id { get; }
    public string NameKey { get; }
    public string AccentHex { get; }

    /// <summary>
    /// Linear fader gain (0.0 to 1.25; 1.0 = 0.0 dB unity).
    /// </summary>
    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(VolumeDbText))]
    private float _volume = 1.0f;

    [ObservableProperty]
    private bool _isMuted;

    [ObservableProperty]
    private bool _isSolo;

    [ObservableProperty]
    private float _peakLeft;

    [ObservableProperty]
    private float _peakRight;

    [ObservableProperty]
    private float _rmsLeft;

    [ObservableProperty]
    private float _rmsRight;

    [ObservableProperty]
    private float _gainReductionDb;

    [ObservableProperty]
    private bool _isClipping;

    [ObservableProperty]
    private bool _isActive;

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

/// <summary>
/// Observable model for a Phase 1 Dual-Path Microphone DSP chain module toggle.
/// </summary>
public partial class DspModuleItem : ObservableObject
{
    public DspModuleItem(string id, string nameKey, string descriptionKey, string badgeText, bool isEnabled = false)
    {
        Id = id;
        NameKey = nameKey;
        DescriptionKey = descriptionKey;
        BadgeText = badgeText;
        _isEnabled = isEnabled;
    }

    public string Id { get; }
    public string NameKey { get; }
    public string DescriptionKey { get; }
    public string BadgeText { get; }

    [ObservableProperty]
    private bool _isEnabled;
}
