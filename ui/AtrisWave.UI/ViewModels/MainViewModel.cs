using System.Collections.ObjectModel;
using System.ComponentModel;
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
    private readonly EngineProcessManager _processManager = new();
    private readonly ChannelMeterFrame[] _channelBuffer = new ChannelMeterFrame[TelemetryConstants.MaxChannels];
    private readonly DispatcherTimer _telemetryTimer;
    private readonly DispatcherTimer _micDspDebounceTimer;
    private bool _disposed;
    private bool _suppressDeviceSelection;

    public ObservableCollection<MixerChannel> Channels { get; } =
    [
        new(MixerChannelId.Master, "Channel_Master", "#38BDF8"),
        new(MixerChannelId.Game, "Channel_Game", "#A855F7"),
        new(MixerChannelId.Chat, "Channel_Chat", "#10B981"),
        new(MixerChannelId.Media, "Channel_Media", "#F59E0B"),
        new(MixerChannelId.Aux, "Channel_Aux", "#EC4899"),
        new(MixerChannelId.Microphone, "Channel_Microphone", "#06B6D4"),
    ];

    public MicDspSettings MicDsp { get; } = new();

    public ObservableCollection<AudioDeviceInfo> InputDevices { get; } = [];
    public ObservableCollection<AudioDeviceInfo> OutputDevices { get; } = [];
    public ObservableCollection<AudioSessionInfo> AudioSessions { get; } = [];
    public ObservableCollection<RoutingRule> RoutingRules { get; } = [];
    public ObservableCollection<string> Presets { get; } = ["Broadcast Warm", "Podcast Crisp", "Gaming Clean"];

    [ObservableProperty] private AudioDeviceInfo? _selectedInputDevice;
    [ObservableProperty] private AudioDeviceInfo? _selectedMonitorDevice;
    [ObservableProperty] private AudioDeviceInfo? _selectedVirtualOutputDevice;
    [ObservableProperty] private AudioSessionInfo? _selectedAudioSession;
    [ObservableProperty] private string _presetNameInput = "Broadcast Warm";

    [ObservableProperty] private bool _isDriverInstalled;
    [ObservableProperty] private string _driverStatusKey = "Driver_StatusBridge";
    [ObservableProperty] private bool _isEngineConnected;
    [ObservableProperty] private bool _isEngineProcessRunning;
    [ObservableProperty] private EngineState _engineState = EngineState.Stopped;
    [ObservableProperty] private uint _sampleRate = 48000;
    [ObservableProperty] private uint _bufferFrames = 256;
    [ObservableProperty] private float _cpuLoadPercent;
    [ObservableProperty] private string _statusBadgeKey = "Status_Standby";
    [ObservableProperty] private string _sampleRateDisplay = string.Empty;
    [ObservableProperty] private string _lastPingMessage = string.Empty;
    [ObservableProperty] private string _engineActionKey = "Io_LaunchEngine";
    [ObservableProperty] private float _micGainReductionNormalized;

    public MainViewModel()
    {
        LocalizationService.Instance.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(LocalizationService.Revision))
            {
                RefreshLocalizedStatusStrings();
            }
        };

        foreach (MixerChannel channel in Channels)
        {
            channel.PropertyChanged += OnChannelPropertyChanged;
        }

        MicDsp.PropertyChanged += OnMicDspPropertyChanged;

        _micDspDebounceTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(45) };
        _micDspDebounceTimer.Tick += async (_, _) =>
        {
            _micDspDebounceTimer.Stop();
            await PushMicDspAsync();
        };

        RefreshLocalizedStatusStrings();

        _telemetryTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromMilliseconds(16.6)
        };
        _telemetryTimer.Tick += OnTelemetryTick;
        _telemetryTimer.Start();
    }

    partial void OnSelectedInputDeviceChanged(AudioDeviceInfo? value) => _ = PushDeviceSelectionAsync();
    partial void OnSelectedMonitorDeviceChanged(AudioDeviceInfo? value) => _ = PushDeviceSelectionAsync();
    partial void OnSelectedVirtualOutputDeviceChanged(AudioDeviceInfo? value) => _ = PushDeviceSelectionAsync();

    private void OnChannelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (sender is not MixerChannel ch)
        {
            return;
        }

        if (e.PropertyName is nameof(MixerChannel.Volume) or nameof(MixerChannel.IsMuted) or nameof(MixerChannel.IsSolo))
        {
            _ = _pipeClient.SetChannelAsync((uint)ch.Id, ch.Volume, ch.IsMuted, ch.IsSolo);
        }
    }

    private void OnMicDspPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        _micDspDebounceTimer.Stop();
        _micDspDebounceTimer.Start();
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
        IsEngineProcessRunning = _processManager.IsRunning;
        EngineActionKey = IsEngineProcessRunning ? "Io_StopEngine" : "Io_LaunchEngine";

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

            float micGr = Channels[(int)MixerChannelId.Microphone].GainReductionDb;
            MicGainReductionNormalized = Math.Clamp(MathF.Abs(micGr) / 24.0f, 0.0f, 1.0f);
        }
        else
        {
            if (IsEngineConnected)
            {
                IsEngineConnected = false;
                EngineState = EngineState.Stopped;
                CpuLoadPercent = 0f;
                MicGainReductionNormalized = 0f;

                foreach (MixerChannel channel in Channels)
                {
                    channel.PeakLeft = 0f;
                    channel.PeakRight = 0f;
                    channel.RmsLeft = 0f;
                    channel.RmsRight = 0f;
                    channel.GainReductionDb = 0f;
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
        => LocalizationService.Instance.ToggleLanguage();

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
        LastPingMessage = response.Ok ? "OK" : (response.Error ?? "Offline");
    }

    [RelayCommand]
    private async Task LaunchOrStopEngineAsync()
    {
        if (_processManager.IsRunning)
        {
            await _processManager.StopEngineAsync(_pipeClient);
            IsEngineProcessRunning = false;
            EngineActionKey = "Io_LaunchEngine";
            LastPingMessage = "Stopped";
            return;
        }

        if (_processManager.TryStartEngine(out string? err))
        {
            IsEngineProcessRunning = true;
            EngineActionKey = "Io_StopEngine";
            LastPingMessage = "Started";
            await Task.Delay(250);
            await RefreshDevicesAsync();
            await RefreshSessionsAsync();
            await RefreshDriverStatusAsync();
            await RefreshRoutingRulesAsync();
        }
        else
        {
            LastPingMessage = err ?? "Launch failed";
        }
    }

    [RelayCommand]
    private async Task RefreshDevicesAsync()
    {
        IReadOnlyList<AudioDeviceInfo> devices = await _pipeClient.ListDevicesAsync();
        if (devices.Count == 0)
        {
            return;
        }

        _suppressDeviceSelection = true;
        try
        {
            InputDevices.Clear();
            OutputDevices.Clear();

            foreach (AudioDeviceInfo dev in devices)
            {
                if (dev.IsInput)
                {
                    InputDevices.Add(dev);
                }
                else
                {
                    OutputDevices.Add(dev);
                }
            }

            SelectedInputDevice ??= InputDevices.FirstOrDefault(d => d.IsDefault) ?? InputDevices.FirstOrDefault();
            SelectedMonitorDevice ??= OutputDevices.FirstOrDefault(d => d.IsDefault && !d.IsVirtual) ?? OutputDevices.FirstOrDefault();
            SelectedVirtualOutputDevice ??= OutputDevices.FirstOrDefault(d => d.IsVirtual) ?? OutputDevices.FirstOrDefault();
        }
        finally
        {
            _suppressDeviceSelection = false;
        }
    }

    [RelayCommand]
    private async Task RefreshSessionsAsync()
    {
        IReadOnlyList<AudioSessionInfo> sessions = await _pipeClient.ListSessionsAsync();
        AudioSessions.Clear();
        foreach (AudioSessionInfo session in sessions)
        {
            AudioSessions.Add(session);
        }
    }

    [RelayCommand]
    private async Task RouteAppAsync(AudioSessionInfo? session)
    {
        if (session is null || string.IsNullOrWhiteSpace(session.TargetDeviceId))
        {
            return;
        }

        await _pipeClient.RouteAppAsync(session.Pid, session.TargetDeviceId, session.LoopbackCapture);
    }

    [RelayCommand]
    private async Task RefreshDriverStatusAsync()
    {
        DriverStatusInfo status = await _pipeClient.GetDriverStatusAsync();
        IsDriverInstalled = status.DriverInstalled;
        DriverStatusKey = status.DriverInstalled ? "Driver_StatusActive" : "Driver_StatusBridge";
    }

    [RelayCommand]
    private async Task RefreshRoutingRulesAsync()
    {
        IReadOnlyList<RoutingRule> rules = await _pipeClient.ListRoutingRulesAsync();
        RoutingRules.Clear();
        foreach (RoutingRule rule in rules)
        {
            RoutingRules.Add(rule);
        }
    }

    [RelayCommand]
    private Task AssignSelectedSessionToChannelAsync(string? channelIdStr)
    {
        if (SelectedAudioSession is null || !Enum.TryParse<MixerChannelId>(channelIdStr, out MixerChannelId channelId))
        {
            return Task.CompletedTask;
        }

        return AssignSessionToChannelAsync(SelectedAudioSession, channelId);
    }

    public async Task AssignSessionToChannelAsync(AudioSessionInfo session, MixerChannelId channelId)
    {
        ArgumentNullException.ThrowIfNull(session);

        string targetDeviceId = !string.IsNullOrWhiteSpace(session.TargetDeviceId)
            ? session.TargetDeviceId
            : (SelectedVirtualOutputDevice?.Id ?? SelectedMonitorDevice?.Id ?? string.Empty);

        var rule = new RoutingRule
        {
            ProcessName = string.IsNullOrWhiteSpace(session.AppName) ? session.EffectiveLabel : session.AppName,
            ChannelId = (uint)channelId,
            TargetDeviceId = targetDeviceId,
            LoopbackCapture = session.LoopbackCapture,
        };

        EnginePipeResponse resp = await _pipeClient.SetRoutingRuleAsync(rule);
        if (resp.Ok)
        {
            await RefreshRoutingRulesAsync();
        }
    }

    [RelayCommand]
    private async Task ApplyAllRoutingRulesAsync()
    {
        await _pipeClient.ApplyRoutingRulesAsync();
        await RefreshSessionsAsync();
    }

    [RelayCommand]
    private async Task DeleteRoutingRuleAsync(RoutingRule? rule)
    {
        if (rule is null || string.IsNullOrWhiteSpace(rule.ProcessName))
        {
            return;
        }

        EnginePipeResponse resp = await _pipeClient.DeleteRoutingRuleAsync(rule.ProcessName);
        if (resp.Ok)
        {
            RoutingRules.Remove(rule);
        }
    }

    [RelayCommand]
    private async Task SavePresetAsync()
    {
        if (string.IsNullOrWhiteSpace(PresetNameInput))
        {
            return;
        }

        EnginePipeResponse resp = await _pipeClient.SavePresetAsync(PresetNameInput.Trim());
        if (resp.Ok && !Presets.Contains(PresetNameInput.Trim()))
        {
            Presets.Add(PresetNameInput.Trim());
        }
    }

    [RelayCommand]
    private async Task LoadPresetAsync()
    {
        if (string.IsNullOrWhiteSpace(PresetNameInput))
        {
            return;
        }

        await _pipeClient.LoadPresetAsync(PresetNameInput.Trim());
    }

    private Task PushMicDspAsync()
        => _pipeClient.SetMicDspAsync(MicDsp);

    private Task PushDeviceSelectionAsync()
    {
        if (_suppressDeviceSelection)
        {
            return Task.CompletedTask;
        }

        return _pipeClient.SelectDevicesAsync(
            SelectedInputDevice?.Id,
            SelectedMonitorDevice?.Id,
            SelectedVirtualOutputDevice?.Id);
    }

    public async Task ShutdownEngineOnExitAsync()
    {
        await _processManager.StopEngineAsync(_pipeClient);
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
        _micDspDebounceTimer.Stop();
        _telemetryReader.Dispose();
        _pipeClient.Dispose();
        _processManager.Dispose();
    }
}
