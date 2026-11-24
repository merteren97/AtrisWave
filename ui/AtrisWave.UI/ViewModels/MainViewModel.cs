using System.Collections.ObjectModel;
using System.Windows.Threading;
using AtrisWave.Core.Ipc;
using AtrisWave.Core.Models;
using AtrisWave.UI.Services;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace AtrisWave.UI.ViewModels;

public sealed partial class MainViewModel : ObservableObject, IDisposable
{
    private readonly SharedMemoryTelemetryReader _telemetryReader = new();
    private readonly EnginePipeClient _pipeClient = new();
    private readonly ChannelMeterFrame[] _channelBuffer = new ChannelMeterFrame[TelemetryConstants.MaxChannels];
    private readonly DispatcherTimer _telemetryTimer;
    private bool _disposed;

    public ObservableCollection<MixerChannel> Channels { get; } =
    [
        new(MixerChannelId.Master, "Channel_Master", "#38BDF8"),
        new(MixerChannelId.Game, "Channel_Game", "#A855F7"),
        new(MixerChannelId.Chat, "Channel_Chat", "#10B981"),
        new(MixerChannelId.Media, "Channel_Media", "#F59E0B"),
        new(MixerChannelId.Aux, "Channel_Aux", "#EC4899"),
        new(MixerChannelId.Microphone, "Channel_Microphone", "#06B6D4"),
    ];

    public ObservableCollection<DspModuleItem> MicDspModules { get; } =
    [
        new("direct_monitor", "Dsp_DirectMonitor_Name", "Dsp_DirectMonitor_Desc", "~6-12 ms", isEnabled: true),
        new("ai_denoise", "Dsp_AiDenoise_Name", "Dsp_AiDenoise_Desc", "AI", isEnabled: true),
        new("noise_gate", "Dsp_NoiseGate_Name", "Dsp_NoiseGate_Desc", "GATE", isEnabled: true),
        new("parametric_eq", "Dsp_ParametricEq_Name", "Dsp_ParametricEq_Desc", "EQ", isEnabled: false),
        new("compressor", "Dsp_Compressor_Name", "Dsp_Compressor_Desc", "COMP", isEnabled: true),
        new("reverb", "Dsp_Reverb_Name", "Dsp_Reverb_Desc", "FX", isEnabled: false),
        new("limiter", "Dsp_Limiter_Name", "Dsp_Limiter_Desc", "LIMIT", isEnabled: true),
    ];

    [ObservableProperty]
    private bool _isEngineConnected;

    [ObservableProperty]
    private EngineState _engineState = EngineState.Stopped;

    [ObservableProperty]
    private uint _sampleRate = 48000;

    [ObservableProperty]
    private uint _bufferFrames = 256;

    [ObservableProperty]
    private float _cpuLoadPercent;

    [ObservableProperty]
    private string _statusBadgeKey = "Status_Standby";

    [ObservableProperty]
    private string _sampleRateDisplay = string.Empty;

    [ObservableProperty]
    private string _lastPingMessage = string.Empty;

    public MainViewModel()
    {
        LocalizationService.Instance.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(LocalizationService.Revision))
            {
                RefreshLocalizedStatusStrings();
            }
        };

        RefreshLocalizedStatusStrings();

        _telemetryTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromMilliseconds(16.6)
        };
        _telemetryTimer.Tick += OnTelemetryTick;
        _telemetryTimer.Start();
    }

    public void SetTelemetryActive(bool active)
    {
        if (_disposed)
        {
            return;
        }

        if (active && !_telemetryTimer.IsEnabled)
        {
            OnTelemetryTick(this, EventArgs.Empty);
            _telemetryTimer.Start();
        }
        else if (!active && _telemetryTimer.IsEnabled)
        {
            _telemetryTimer.Stop();
        }
    }

    private void OnTelemetryTick(object? sender, EventArgs e)
    {
        if (_telemetryReader.TryRead(out TelemetryHeader header, _channelBuffer))
        {
            EngineState state = Enum.IsDefined(typeof(EngineState), header.EngineState)
                ? (EngineState)header.EngineState
                : EngineState.Stopped;

            IsEngineConnected = state != EngineState.Stopped;
            EngineState = state;
            SampleRate = header.SampleRate > 0 ? header.SampleRate : 48000;
            BufferFrames = header.BufferFrames;
            CpuLoadPercent = header.CpuLoadPercent;

            int count = (int)Math.Min(header.ChannelCount, (uint)Channels.Count);
            for (int i = 0; i < count; i++)
            {
                ChannelMeterFrame frame = _channelBuffer[i];
                MixerChannel channel = Channels[i];
                channel.PeakLeft = frame.PeakLeft;
                channel.PeakRight = frame.PeakRight;
                channel.RmsLeft = frame.RmsLeft;
                channel.RmsRight = frame.RmsRight;
                channel.GainReductionDb = frame.GainReductionDb;
                channel.IsClipping = frame.IsClipping;
                channel.IsActive = frame.IsActive;
            }
        }
        else
        {
            if (IsEngineConnected)
            {
                IsEngineConnected = false;
                EngineState = EngineState.Stopped;
                CpuLoadPercent = 0f;

                foreach (MixerChannel channel in Channels)
                {
                    channel.PeakLeft = 0f;
                    channel.PeakRight = 0f;
                    channel.RmsLeft = 0f;
                    channel.RmsRight = 0f;
                    channel.IsClipping = false;
                    channel.IsActive = false;
                }
            }
        }

        RefreshLocalizedStatusStrings();
    }

    private void RefreshLocalizedStatusStrings()
    {
        LocalizationService loc = LocalizationService.Instance;

        StatusBadgeKey = EngineState switch
        {
            EngineState.Running => "Status_Connected",
            EngineState.Degraded => "Status_Degraded",
            _ => "Status_Standby",
        };

        if (IsEngineConnected)
        {
            string fmt = loc.GetString("Status_SampleRateFormat");
            SampleRateDisplay = string.Format(fmt, SampleRate, BufferFrames);
        }
        else
        {
            SampleRateDisplay = loc.GetString("Status_SampleRateOffline");
        }
    }

    [RelayCommand]
    private void ToggleLanguage()
    {
        LocalizationService.Instance.ToggleLanguage();
    }

    [RelayCommand]
    private void ToggleMicMute()
    {
        MixerChannel? mic = Channels.FirstOrDefault(c => c.Id == MixerChannelId.Microphone);
        if (mic is not null)
        {
            mic.IsMuted = !mic.IsMuted;
        }
    }

    [RelayCommand]
    private async Task PingEngineAsync()
    {
        EnginePipeResponse response = await _pipeClient.PingAsync();
        LastPingMessage = response.Ok
            ? "OK"
            : (response.Error ?? "Offline");
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _telemetryTimer.Stop();
        _telemetryTimer.Tick -= OnTelemetryTick;
        _telemetryReader.Dispose();
        _pipeClient.Dispose();
    }
}
