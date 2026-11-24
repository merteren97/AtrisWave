using CommunityToolkit.Mvvm.ComponentModel;

namespace AtrisWave.Core.Models;

/// <summary>
/// Studio Soundboard pad model (4x3 grid of 12 customizable WAV pads).
/// </summary>
public sealed class SoundboardPadModel : ObservableObject
{
    private string _title = string.Empty;
    private string _filePath = string.Empty;
    private string _accentColor = "#38BDF8";
    private float _gain = 1.0f;
    private bool _sendA1 = true;
    private bool _sendB1;
    private bool _sendB2 = true;
    private bool _loop;
    private bool _isPlaying;

    public int PadIndex { get; set; }

    public string Title
    {
        get => _title;
        set => SetProperty(ref _title, value);
    }

    public string FilePath
    {
        get => _filePath;
        set
        {
            if (SetProperty(ref _filePath, value))
            {
                OnPropertyChanged(nameof(HasFile));
                OnPropertyChanged(nameof(FileNameDisplay));
            }
        }
    }

    public string AccentColor
    {
        get => _accentColor;
        set => SetProperty(ref _accentColor, value);
    }

    public float Gain
    {
        get => _gain;
        set => SetProperty(ref _gain, value);
    }

    public bool SendA1
    {
        get => _sendA1;
        set => SetProperty(ref _sendA1, value);
    }

    public bool SendB1
    {
        get => _sendB1;
        set => SetProperty(ref _sendB1, value);
    }

    public bool SendB2
    {
        get => _sendB2;
        set => SetProperty(ref _sendB2, value);
    }

    public bool Loop
    {
        get => _loop;
        set => SetProperty(ref _loop, value);
    }

    public bool IsPlaying
    {
        get => _isPlaying;
        set => SetProperty(ref _isPlaying, value);
    }

    public bool HasFile => !string.IsNullOrWhiteSpace(FilePath);
    public string FileNameDisplay => HasFile ? Path.GetFileName(FilePath) : "—";
}
