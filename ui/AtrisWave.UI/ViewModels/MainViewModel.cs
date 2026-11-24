using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Threading;
using AtrisWave.Core.Automation;
using AtrisWave.Core.Dsp;
using AtrisWave.Core.Ipc;
using AtrisWave.Core.Models;
using AtrisWave.Core.Plugins;
using AtrisWave.UI.Services;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Microsoft.Win32;

namespace AtrisWave.UI.ViewModels;

public sealed partial class MainViewModel : ObservableObject, IDisposable
{
    private readonly SharedMemoryTelemetryReader _telemetryReader = new();
    private readonly EnginePipeClient _pipeClient = new();
    private readonly EngineProcessManager _processManager = new();
    private readonly LocalAutomationServer _rpcServer;
    private readonly ChannelMeterFrame[] _channelBuffer = new ChannelMeterFrame[TelemetryConstants.MaxChannels];
    private readonly ChannelMeterFrame[] _busBuffer = new ChannelMeterFrame[TelemetryConstants.MaxBuses];
    private readonly DispatcherTimer _telemetryTimer;
    private readonly DispatcherTimer _micDspDebounceTimer;
    private bool _disposed;
    private bool _suppressSync;

    public ObservableCollection<DynamicChannelModel> DynamicChannels { get; } =
    [
        new() { ChannelId = 0, Name = "System (All Apps)", SourceType = "app_group", Color = "#38BDF8", IsSystem = true, SendA1 = true, SendB2 = true },
        new() { ChannelId = 1, Name = "Microphone", SourceType = "hardware_input", Color = "#06B6D4", IsSystem = false, SendA1 = false, SendB1 = true, SendB2 = true },
    ];

    public ObservableCollection<OutputBusModel> OutputBuses { get; } =
    [
        new() { BusId = "A1", Role = "A1", Name = "A1 Headphones" },
        new() { BusId = "A2", Role = "A2", Name = "A2 Speakers" },
        new() { BusId = "B1", Role = "B1", Name = "B1 Mic Out" },
        new() { BusId = "B2", Role = "B2", Name = "B2 Stream Mix" },
    ];

    public MicDspSettings MicDsp { get; } = new();

    public ObservableCollection<AudioDeviceInfo> InputDevices { get; } = [];
    public ObservableCollection<AudioDeviceInfo> OutputDevices { get; } = [];
    public ObservableCollection<AudioSessionInfo> AudioSessions { get; } = [];
    public ObservableCollection<RoutingRule> RoutingRules { get; } = [];
    public ObservableCollection<DiscoveredPlugin> DiscoveredPlugins { get; } = [];
    public ObservableCollection<string> Presets { get; } = ["Broadcast Warm", "Podcast Crisp", "Gaming Clean"];

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
    [ObservableProperty] private bool _globalHotkeysEnabled = true;
    [ObservableProperty] private bool _rpcServerEnabled;
    [ObservableProperty] private string _rpcStatusKey = "Pro_RpcStopped";

    [ObservableProperty] private bool _isHealthWarning = true;
    [ObservableProperty] private string _healthMessageKey = "Health_MissingA1";

    [ObservableProperty] private bool _isNewChannelCreatorOpen;
    [ObservableProperty] private string _newChannelName = string.Empty;
    [ObservableProperty] private int _newChannelSourceIndex;

    public event EventHandler<bool>? GlobalHotkeysEnabledChanged;

    public MainViewModel()
    {
        _rpcServer = new LocalAutomationServer(HandleRpcAsync, port: 18923);

        LocalizationService.Instance.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(LocalizationService.Revision))
            {
                RefreshLocalizedStatusStrings();
            }
        };

        AttachChannelHandlers();
        AttachBusHandlers();
        MicDsp.PropertyChanged += OnMicDspPropertyChanged;

        _micDspDebounceTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(45) };
        _micDspDebounceTimer.Tick += async (_, _) =>
        {
            _micDspDebounceTimer.Stop();
            await _pipeClient.SetMicDspAsync(MicDsp);
        };

        RefreshLocalizedStatusStrings();
        UpdateHealthCheck();

        _telemetryTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromMilliseconds(16.6)
        };
        _telemetryTimer.Tick += OnTelemetryTick;
        _telemetryTimer.Start();
    }

    private void AttachChannelHandlers()
    {
        foreach (DynamicChannelModel ch in DynamicChannels)
        {
            ch.PropertyChanged -= OnDynamicChannelPropertyChanged;
            ch.PropertyChanged += OnDynamicChannelPropertyChanged;
        }
    }

    private void AttachBusHandlers()
    {
        foreach (OutputBusModel bus in OutputBuses)
        {
            bus.PropertyChanged -= OnOutputBusPropertyChanged;
            bus.PropertyChanged += OnOutputBusPropertyChanged;
        }
    }

    partial void OnGlobalHotkeysEnabledChanged(bool value) => GlobalHotkeysEnabledChanged?.Invoke(this, value);

    partial void OnRpcServerEnabledChanged(bool value)
    {
        if (value)
        {
            _rpcServer.Start();
            RpcStatusKey = "Pro_RpcRunning";
        }
        else
        {
            _ = _rpcServer.StopAsync();
            RpcStatusKey = "Pro_RpcStopped";
        }
    }

    private void OnDynamicChannelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_suppressSync || sender is not DynamicChannelModel ch)
        {
            return;
        }

        switch (e.PropertyName)
        {
            case nameof(DynamicChannelModel.Volume):
            case nameof(DynamicChannelModel.Muted):
            case nameof(DynamicChannelModel.Solo):
            case nameof(DynamicChannelModel.Pan):
                _ = _pipeClient.SetChannelDspAsync(ch.ChannelId, ch.Volume, ch.Muted, ch.Solo, ch.Pan);
                break;
            case nameof(DynamicChannelModel.SendA1):
                _ = _pipeClient.SetChannelSendAsync(ch.ChannelId, "A1", ch.SendA1);
                break;
            case nameof(DynamicChannelModel.SendA2):
                _ = _pipeClient.SetChannelSendAsync(ch.ChannelId, "A2", ch.SendA2);
                break;
            case nameof(DynamicChannelModel.SendB1):
                _ = _pipeClient.SetChannelSendAsync(ch.ChannelId, "B1", ch.SendB1);
                break;
            case nameof(DynamicChannelModel.SendB2):
                _ = _pipeClient.SetChannelSendAsync(ch.ChannelId, "B2", ch.SendB2);
                break;
            case nameof(DynamicChannelModel.DeviceId):
                _ = _pipeClient.BindChannelSourceAsync(ch.ChannelId, ch.SourceType, ch.DeviceId);
                UpdateHealthCheck();
                break;
        }
    }

    private void OnOutputBusPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_suppressSync || sender is not OutputBusModel bus)
        {
            return;
        }

        if (e.PropertyName is nameof(OutputBusModel.DeviceId) or nameof(OutputBusModel.Volume) or nameof(OutputBusModel.Muted))
        {
            _ = _pipeClient.BindOutputBusAsync(bus.BusId, bus.DeviceId, bus.Volume, bus.Muted);
            UpdateHealthCheck();
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

        if (_telemetryReader.TryRead(out TelemetryHeader header, _channelBuffer, _busBuffer))
        {
            EngineState state = Enum.IsDefined(typeof(EngineState), header.EngineState)
                ? (EngineState)header.EngineState
                : EngineState.Stopped;

            IsEngineConnected = state != EngineState.Stopped;
            EngineState = state;
            SampleRate = header.SampleRate > 0 ? header.SampleRate : 48000;
            BufferFrames = header.BufferFrames;
            CpuLoadPercent = header.CpuLoadPercent;

            foreach (DynamicChannelModel ch in DynamicChannels)
            {
                if (ch.ChannelId >= TelemetryConstants.MaxChannels)
                {
                    continue;
                }

                ChannelMeterFrame frame = _channelBuffer[ch.ChannelId];
                ch.PeakLeft = frame.PeakLeft;
                ch.PeakRight = frame.PeakRight;
                ch.RmsLeft = frame.RmsLeft;
                ch.RmsRight = frame.RmsRight;
                ch.GainReductionDb = frame.GainReductionDb;
                ch.IsClipping = frame.IsClipping;
                ch.IsActive = frame.IsActive;
            }

            int busCount = (int)Math.Min(header.BusCount, TelemetryConstants.MaxBuses);
            for (int i = 0; i < busCount && i < OutputBuses.Count; i++)
            {
                ChannelMeterFrame frame = _busBuffer[i];
                OutputBusModel bus = OutputBuses[i];
                bus.PeakLeft = frame.PeakLeft;
                bus.PeakRight = frame.PeakRight;
                bus.RmsLeft = frame.RmsLeft;
                bus.RmsRight = frame.RmsRight;
                bus.IsClipping = frame.IsClipping;
            }

            DynamicChannelModel? mic = DynamicChannels.FirstOrDefault(c => c.IsHardwareInput);
            MicGainReductionNormalized = mic is not null
                ? Math.Clamp(MathF.Abs(mic.GainReductionDb) / 24.0f, 0.0f, 1.0f)
                : 0f;
        }
        else if (IsEngineConnected)
        {
            IsEngineConnected = false;
            EngineState = EngineState.Stopped;
            CpuLoadPercent = 0f;
            MicGainReductionNormalized = 0f;

            foreach (DynamicChannelModel ch in DynamicChannels)
            {
                ch.PeakLeft = 0f;
                ch.PeakRight = 0f;
                ch.RmsLeft = 0f;
                ch.RmsRight = 0f;
                ch.GainReductionDb = 0f;
                ch.IsClipping = false;
                ch.IsActive = false;
            }

            foreach (OutputBusModel bus in OutputBuses)
            {
                bus.PeakLeft = 0f;
                bus.PeakRight = 0f;
                bus.RmsLeft = 0f;
                bus.RmsRight = 0f;
                bus.IsClipping = false;
            }
        }

        RefreshLocalizedStatusStrings();
    }

    private void UpdateHealthCheck()
    {
        bool a1Ready = OutputBuses.Count > 0 && !string.IsNullOrWhiteSpace(OutputBuses[0].DeviceId);
        bool micReady = DynamicChannels.Any(c => c.IsHardwareInput && !string.IsNullOrWhiteSpace(c.DeviceId));
        bool ready = a1Ready && micReady;
        IsHealthWarning = !ready;
        HealthMessageKey = ready ? "Health_AllReady" : "Health_MissingA1";
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

        SampleRateDisplay = IsEngineConnected
            ? string.Format(loc.GetString("Status_SampleRateFormat"), SampleRate, BufferFrames)
            : loc.GetString("Status_SampleRateOffline");
    }

    [RelayCommand]
    private void ToggleLanguage() => LocalizationService.Instance.ToggleLanguage();

    [RelayCommand]
    private void ToggleMicMute()
    {
        DynamicChannelModel? mic = DynamicChannels.FirstOrDefault(c => c.IsHardwareInput);
        if (mic is not null)
        {
            mic.Muted = !mic.Muted;
        }
    }

    public void ToggleDirectMonitor() => MicDsp.DirectMonitor = !MicDsp.DirectMonitor;

    [RelayCommand]
    private void ToggleNewChannelCreator() => IsNewChannelCreatorOpen = !IsNewChannelCreatorOpen;

    [RelayCommand]
    private async Task CreateNewChannelAsync()
    {
        string name = string.IsNullOrWhiteSpace(NewChannelName) ? "Custom" : NewChannelName.Trim();
        string sourceType = NewChannelSourceIndex switch
        {
            1 => "hardware_input",
            2 => "process_loopback",
            _ => "app_group",
        };

        string[] palette = ["#A855F7", "#10B981", "#F59E0B", "#EC4899", "#38BDF8"];
        string color = palette[DynamicChannels.Count % palette.Length];

        (bool ok, uint _) = await _pipeClient.CreateChannelAsync(name, sourceType, color);
        if (ok)
        {
            await RefreshV2MixerStateAsync();
        }
        else
        {
            uint nextId = DynamicChannels.Count > 0 ? DynamicChannels.Max(c => c.ChannelId) + 1 : 2;
            DynamicChannels.Add(new DynamicChannelModel
            {
                ChannelId = nextId,
                Name = name,
                SourceType = sourceType,
                Color = color,
                SendA1 = sourceType != "process_loopback",
                SendB2 = true,
            });
            AttachChannelHandlers();
        }

        NewChannelName = string.Empty;
        IsNewChannelCreatorOpen = false;
    }

    [RelayCommand]
    private async Task DeleteChannelAsync(DynamicChannelModel? channel)
    {
        if (channel is null || !channel.CanDelete)
        {
            return;
        }

        await _pipeClient.DeleteChannelAsync(channel.ChannelId);
        DynamicChannels.Remove(channel);
    }

    [RelayCommand]
    private async Task UnbindAppFromChannelAsync(object? parameter)
    {
        if (parameter is ValueTuple<DynamicChannelModel, string> pair)
        {
            await _pipeClient.UnbindAppFromChannelAsync(pair.Item2, pair.Item1.ChannelId);
            pair.Item1.BoundApps.Remove(pair.Item2);
        }
    }

    [RelayCommand]
    private async Task AutoSetupDefaultsAsync()
    {
        EnginePipeResponse resp = await _pipeClient.AutoSetupDefaultsAsync();
        if (resp.Ok)
        {
            await RefreshV2MixerStateAsync();
        }
        else
        {
            await RefreshDevicesAsync();
            AudioDeviceInfo? defOut = OutputDevices.FirstOrDefault(d => d.IsDefault && !d.IsVirtual) ?? OutputDevices.FirstOrDefault();
            AudioDeviceInfo? defMic = InputDevices.FirstOrDefault(d => d.IsDefault) ?? InputDevices.FirstOrDefault();
            if (defOut is not null && OutputBuses.Count > 0)
            {
                OutputBuses[0].DeviceId = defOut.Id;
            }
            DynamicChannelModel? mic = DynamicChannels.FirstOrDefault(c => c.IsHardwareInput);
            if (defMic is not null && mic is not null)
            {
                mic.DeviceId = defMic.Id;
            }
            UpdateHealthCheck();
        }
    }

    [RelayCommand]
    private void ImportAutoEq()
    {
        var dialog = new OpenFileDialog
        {
            Filter = "AutoEq Parametric EQ (*.txt)|*.txt|All Files (*.*)|*.*",
        };

        if (dialog.ShowDialog() == true && File.Exists(dialog.FileName))
        {
            string content = File.ReadAllText(dialog.FileName);
            AutoEqProfile profile = AutoEqParser.Parse(content);
            AutoEqParser.ApplyTo(profile, MicDsp);
        }
    }

    [RelayCommand]
    private void ScanPlugins()
    {
        DiscoveredPlugins.Clear();
        foreach (DiscoveredPlugin plugin in PluginScanner.ScanStandardDirectories())
        {
            DiscoveredPlugins.Add(plugin);
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
            await RefreshV2MixerStateAsync();
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

        _suppressSync = true;
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
        }
        finally
        {
            _suppressSync = false;
        }
    }

    [RelayCommand]
    private async Task RefreshV2MixerStateAsync()
    {
        IReadOnlyList<DynamicChannelModel> channels = await _pipeClient.ListChannelsAsync();
        IReadOnlyList<OutputBusModel> buses = await _pipeClient.ListOutputBusesAsync();

        _suppressSync = true;
        try
        {
            if (channels.Count > 0)
            {
                DynamicChannels.Clear();
                foreach (DynamicChannelModel ch in channels)
                {
                    DynamicChannels.Add(ch);
                }
                AttachChannelHandlers();
            }

            if (buses.Count > 0)
            {
                OutputBuses.Clear();
                foreach (OutputBusModel bus in buses)
                {
                    OutputBuses.Add(bus);
                }
                AttachBusHandlers();
            }
        }
        finally
        {
            _suppressSync = false;
        }

        UpdateHealthCheck();
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

    public async Task BindSessionToDynamicChannelAsync(AudioSessionInfo session, DynamicChannelModel channel)
    {
        ArgumentNullException.ThrowIfNull(session);
        ArgumentNullException.ThrowIfNull(channel);

        string procName = string.IsNullOrWhiteSpace(session.AppName) ? session.EffectiveLabel : session.AppName;
        EnginePipeResponse resp = await _pipeClient.BindAppToChannelAsync(procName, channel.ChannelId);
        if (resp.Ok && !channel.BoundApps.Contains(procName))
        {
            channel.BoundApps.Add(procName);
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
            : (OutputBuses.FirstOrDefault(b => b.BusId == "A1")?.DeviceId ?? string.Empty);

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

    private async Task<object?> HandleRpcAsync(string method, JsonElement? parameters)
    {
        switch (method)
        {
            case "get_status":
                DynamicChannelModel? mic = DynamicChannels.FirstOrDefault(c => c.IsHardwareInput);
                return new
                {
                    connected = IsEngineConnected,
                    sample_rate = SampleRate,
                    buffer_frames = BufferFrames,
                    mic_muted = mic?.Muted ?? false,
                };
            case "toggle_mic_mute":
                await Application.Current.Dispatcher.InvokeAsync(ToggleMicMute);
                return new { mic_muted = DynamicChannels.FirstOrDefault(c => c.IsHardwareInput)?.Muted ?? false };
            case "set_channel_volume":
                if (parameters is JsonElement pVol &&
                    pVol.TryGetProperty("channel_id", out JsonElement chEl) &&
                    pVol.TryGetProperty("volume", out JsonElement volEl))
                {
                    uint cid = chEl.GetUInt32();
                    float vol = volEl.GetSingle();
                    DynamicChannelModel? ch = DynamicChannels.FirstOrDefault(c => c.ChannelId == cid);
                    if (ch is not null)
                    {
                        await Application.Current.Dispatcher.InvokeAsync(() => ch.Volume = Math.Clamp(vol, 0f, 1.25f));
                        return new { ok = true };
                    }
                }
                return new { ok = false };
            case "set_channel_mute":
                if (parameters is JsonElement pMute &&
                    pMute.TryGetProperty("channel_id", out JsonElement chM) &&
                    pMute.TryGetProperty("muted", out JsonElement mEl))
                {
                    uint cid = chM.GetUInt32();
                    bool muted = mEl.GetBoolean();
                    DynamicChannelModel? ch = DynamicChannels.FirstOrDefault(c => c.ChannelId == cid);
                    if (ch is not null)
                    {
                        await Application.Current.Dispatcher.InvokeAsync(() => ch.Muted = muted);
                        return new { ok = true };
                    }
                }
                return new { ok = false };
            case "load_preset":
                if (parameters is JsonElement pPreset && pPreset.TryGetProperty("name", out JsonElement nameEl))
                {
                    string? presetName = nameEl.GetString();
                    if (!string.IsNullOrWhiteSpace(presetName))
                    {
                        EnginePipeResponse resp = await _pipeClient.LoadPresetAsync(presetName);
                        return new { ok = resp.Ok };
                    }
                }
                return new { ok = false };
            default:
                return new RpcMethodNotFound();
        }
    }

    public async Task ShutdownEngineOnExitAsync()
        => await _processManager.StopEngineAsync(_pipeClient);

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
        _rpcServer.Dispose();
    }
}
