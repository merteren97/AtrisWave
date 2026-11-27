using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows;
using System.Windows.Threading;
using AtrisWave.Core.Automation;
using AtrisWave.Core.Dsp;
using AtrisWave.Core.Ipc;
using AtrisWave.Core.Models;
using AtrisWave.Core.Plugins;
using AtrisWave.Core.Services;
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
    private readonly AppSettingsService _settingsService = new();
    private readonly StartupRegistrationService _startupService = new();
    private readonly AppSettings _appSettings;
    private readonly LocalAutomationServer _rpcServer;
    private readonly ChannelMeterFrame[] _channelBuffer = new ChannelMeterFrame[TelemetryConstants.MaxChannels];
    private readonly ChannelMeterFrame[] _busBuffer = new ChannelMeterFrame[TelemetryConstants.MaxBuses];
    public float[] SpectrumDb { get; } = new float[TelemetryConstants.SpectrumBins];

    private readonly DispatcherTimer _telemetryTimer;
    private readonly DispatcherTimer _micDspDebounceTimer;
    private readonly DispatcherTimer _mixDebounceTimer;
    private readonly HashSet<uint> _pendingChannelMixIds = [];
    private readonly HashSet<uint> _pendingChannelFullDspIds = [];
    private readonly HashSet<string> _pendingBusMixIds = new(StringComparer.OrdinalIgnoreCase);

    private DynamicChannelModel? _cachedPrimaryMic;
    private bool _disposed;
    private bool _suppressSync;
    private bool _suppressSettingsSave;
    private bool _isSyncingFromEngine;
    private int _startupInitialized;
    private DateTime _lastAutoSyncUtc = DateTime.MinValue;
    private double _autoSyncBackoffSeconds = 2.0;
    private string _updateLatestVersion = string.Empty;

    private EngineState _lastStatusEngineState = (EngineState)uint.MaxValue;
    private uint _lastStatusSampleRate;
    private uint _lastStatusBufferFrames;
    private bool _lastStatusConnected;

    public string CurrentAppVersion { get; } = ResolveAppVersion();

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
    public ObservableCollection<SoundboardPadModel> SoundboardPads { get; } = CreateDefaultSoundboardPads();
    public ObservableCollection<string> Presets { get; } = ["Broadcast Warm", "Podcast Crisp", "Gaming Clean"];

    [ObservableProperty] private int _selectedNavIndex;
    [ObservableProperty] private bool _isChannelDrawerOpen;
    [ObservableProperty] private DynamicChannelModel? _selectedDrawerChannel;
    [ObservableProperty] private string _updateStatusText = string.Empty;
    [ObservableProperty] private string _updateReleaseUrl = string.Empty;
    [ObservableProperty] private string _updateInstallerUrl = string.Empty;
    [ObservableProperty] private string _updateChecksumsUrl = string.Empty;
    [ObservableProperty] private string _updateReleaseNotes = string.Empty;
    [ObservableProperty] private bool _isUpdateAvailable;
    [ObservableProperty] private bool _canDownloadUpdate;
    [ObservableProperty] private bool _isDownloadingUpdate;
    [ObservableProperty] private double _updateDownloadProgress;
    [ObservableProperty] private bool _isMicMuted;

    public event EventHandler? SpectrumUpdated;

    [ObservableProperty] private AudioSessionInfo? _selectedAudioSession;
    [ObservableProperty] private string _presetNameInput = "Broadcast Warm";

    [ObservableProperty] private bool _isDriverInstalled;
    [ObservableProperty] private string _driverStatusKey = "Driver_StatusBridge";
    [ObservableProperty] private bool _isEngineConnected;
    [ObservableProperty] private bool _isEngineProcessRunning;
    [ObservableProperty] private bool _isEngineRunning;
    [ObservableProperty] private EngineState _engineState = EngineState.Stopped;
    [ObservableProperty] private uint _sampleRate = 48000;
    [ObservableProperty] private uint _bufferFrames = 256;
    [ObservableProperty] private float _cpuLoadPercent;
    [ObservableProperty] private string _statusBadgeKey = "Status_Standby";
    [ObservableProperty] private string _engineStatusBadgeText = string.Empty;
    [ObservableProperty] private string _sampleRateDisplay = string.Empty;
    [ObservableProperty] private string _engineDiagnosticsText = string.Empty;
    [ObservableProperty] private string _lastPingMessage = string.Empty;
    [ObservableProperty] private string _lastErrorMessage = string.Empty;
    [ObservableProperty] private bool _hasLastError;
    [ObservableProperty] private string _engineActionKey = "Io_LaunchEngine";
    [ObservableProperty] private float _micGainReductionNormalized;
    [ObservableProperty] private bool _globalHotkeysEnabled = true;
    [ObservableProperty] private bool _rpcServerEnabled;
    [ObservableProperty] private string _rpcStatusKey = "Pro_RpcStopped";

    [ObservableProperty] private bool _isHealthWarning = true;
    [ObservableProperty] private string _healthMessageKey = "Health_MissingA1";
    [ObservableProperty] private string _healthWarningMessage = string.Empty;

    [ObservableProperty] private bool _hasNoOutputDevices = true;
    [ObservableProperty] private bool _hasNoInputDevices = true;
    [ObservableProperty] private bool _hasNoAudioSessions = true;
    [ObservableProperty] private bool _hasNoPlugins = true;
    [ObservableProperty] private string _activeMicDisplayName = string.Empty;

    [ObservableProperty] private bool _autoStartEngineOnLogon;
    [ObservableProperty] private bool _autoStartAppOnLogon;
    [ObservableProperty] private bool _startMinimizedToTray;
    [ObservableProperty] private bool _keepEngineRunningOnExit = true;

    [ObservableProperty] private bool _isStreamerMode;
    [ObservableProperty] private bool _isMicProMode;
    [ObservableProperty] private float _micLiveLevelNormalized;
    [ObservableProperty] private string _newChannelAppProcess = string.Empty;

    [ObservableProperty] private bool _isNewChannelCreatorOpen;
    [ObservableProperty] private string _newChannelName = string.Empty;
    [ObservableProperty] private int _newChannelSourceIndex;

    public OutputBusModel? PrimaryOutputBus => OutputBuses.Count > 0 ? OutputBuses[0] : null;
    public DynamicChannelModel? PrimaryMicChannel => _cachedPrimaryMic ??= DynamicChannels.FirstOrDefault(c => c.IsHardwareInput);

    public event EventHandler<bool>? GlobalHotkeysEnabledChanged;

    public MainViewModel()
    {
        _rpcServer = new LocalAutomationServer(HandleRpcAsync, port: 18923);
        _appSettings = _settingsService.Load();

        _suppressSettingsSave = true;
        try
        {
            if (!string.IsNullOrWhiteSpace(_appSettings.Language) &&
                !string.Equals(_appSettings.Language, LocalizationService.Instance.CurrentLanguage, StringComparison.OrdinalIgnoreCase))
            {
                LocalizationService.Instance.SetLanguage(_appSettings.Language);
            }

            IsStreamerMode = _appSettings.IsStreamerMode;
            GlobalHotkeysEnabled = _appSettings.GlobalHotkeysEnabled;
            StartMinimizedToTray = _appSettings.StartMinimizedToTray;
            KeepEngineRunningOnExit = _appSettings.KeepEngineRunningOnExit;
            AutoStartEngineOnLogon = _startupService.IsEngineAutoStartEnabled();
            AutoStartAppOnLogon = _startupService.IsAppAutoStartEnabled();
            if (_appSettings.RpcServerEnabled)
            {
                RpcServerEnabled = true;
            }
        }
        finally
        {
            _suppressSettingsSave = false;
        }

        LocalizationService.Instance.PropertyChanged += OnLocalizationPropertyChanged;

        AttachChannelHandlers();
        AttachBusHandlers();
        MicDsp.PropertyChanged += OnMicDspPropertyChanged;

        _micDspDebounceTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(45) };
        _micDspDebounceTimer.Tick += OnMicDspDebounceTick;

        _mixDebounceTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(35) };
        _mixDebounceTimer.Tick += OnMixDebounceTick;

        RefreshLocalizedStatusStrings();
        UpdateHealthCheck();
        UpdateDeviceEmptyStates();

        _telemetryTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromMilliseconds(16.6)
        };
        _telemetryTimer.Tick += OnTelemetryTick;
        _telemetryTimer.Start();
    }

    private void OnLocalizationPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(LocalizationService.Revision))
        {
            if (!_suppressSettingsSave)
            {
                _appSettings.Language = LocalizationService.Instance.CurrentLanguage;
                _settingsService.Save(_appSettings);
            }

            RefreshLocalizedStatusStrings(force: true);
            UpdateHealthCheck();
            UpdateActiveMicDisplayName();
        }
    }

    private async void OnMicDspDebounceTick(object? sender, EventArgs e)
    {
        _micDspDebounceTimer.Stop();
        if (_disposed)
        {
            return;
        }

        try
        {
            await _pipeClient.SetMicDspAsync(MicDsp);
        }
        catch (Exception ex)
        {
            SetError(ex.Message);
        }
    }

    private async void OnMixDebounceTick(object? sender, EventArgs e)
    {
        _mixDebounceTimer.Stop();
        if (_disposed)
        {
            return;
        }

        uint[] mixIds = [.. _pendingChannelMixIds];
        uint[] fullDspIds = [.. _pendingChannelFullDspIds];
        string[] busIds = [.. _pendingBusMixIds];
        _pendingChannelMixIds.Clear();
        _pendingChannelFullDspIds.Clear();
        _pendingBusMixIds.Clear();

        try
        {
            foreach (uint cid in mixIds)
            {
                DynamicChannelModel? ch = DynamicChannels.FirstOrDefault(c => c.ChannelId == cid);
                if (ch is not null)
                {
                    await _pipeClient.SetChannelDspAsync(ch.ChannelId, ch.Volume, ch.Muted, ch.Solo, ch.Pan);
                }
            }

            foreach (uint cid in fullDspIds)
            {
                DynamicChannelModel? ch = DynamicChannels.FirstOrDefault(c => c.ChannelId == cid);
                if (ch is not null)
                {
                    await _pipeClient.SetChannelFullDspAsync(ch);
                }
            }

            foreach (string busId in busIds)
            {
                OutputBusModel? bus = OutputBuses.FirstOrDefault(b => string.Equals(b.BusId, busId, StringComparison.OrdinalIgnoreCase));
                if (bus is not null)
                {
                    await _pipeClient.SetOutputBusMixAsync(bus.BusId, bus.Volume, bus.Muted);
                }
            }
        }
        catch (Exception ex)
        {
            SetError(ex.Message);
        }
    }

    private void AttachChannelHandlers()
    {
        foreach (DynamicChannelModel ch in DynamicChannels)
        {
            ch.PropertyChanged -= OnDynamicChannelPropertyChanged;
            ch.PropertyChanged += OnDynamicChannelPropertyChanged;
        }
        _cachedPrimaryMic = DynamicChannels.FirstOrDefault(c => c.IsHardwareInput);
    }

    private void AttachBusHandlers()
    {
        foreach (OutputBusModel bus in OutputBuses)
        {
            bus.PropertyChanged -= OnOutputBusPropertyChanged;
            bus.PropertyChanged += OnOutputBusPropertyChanged;
        }
    }

    partial void OnGlobalHotkeysEnabledChanged(bool value)
    {
        GlobalHotkeysEnabledChanged?.Invoke(this, value);
        if (!_suppressSettingsSave)
        {
            _appSettings.GlobalHotkeysEnabled = value;
            _settingsService.Save(_appSettings);
        }
    }

    partial void OnIsStreamerModeChanged(bool value)
    {
        if (!_suppressSettingsSave)
        {
            _appSettings.IsStreamerMode = value;
            _settingsService.Save(_appSettings);
        }
    }

    partial void OnAutoStartEngineOnLogonChanged(bool value)
    {
        if (_suppressSettingsSave)
        {
            return;
        }

        try
        {
            string? engineExe = _processManager.FindEngineExecutable();
            _startupService.SetEngineAutoStart(value, engineExe);
            _appSettings.AutoStartEngineOnLogon = value;
            _settingsService.Save(_appSettings);
            ClearError();
        }
        catch (Exception ex)
        {
            SetError(ex.Message);
            _suppressSettingsSave = true;
            try
            {
                AutoStartEngineOnLogon = !value;
            }
            finally
            {
                _suppressSettingsSave = false;
            }
        }
    }

    partial void OnAutoStartAppOnLogonChanged(bool value)
    {
        if (_suppressSettingsSave)
        {
            return;
        }

        try
        {
            string? uiExe = Environment.ProcessPath;
            _startupService.SetAppAutoStart(value, uiExe, StartMinimizedToTray);
            _appSettings.AutoStartAppOnLogon = value;
            _settingsService.Save(_appSettings);
            ClearError();
        }
        catch (Exception ex)
        {
            SetError(ex.Message);
            _suppressSettingsSave = true;
            try
            {
                AutoStartAppOnLogon = !value;
            }
            finally
            {
                _suppressSettingsSave = false;
            }
        }
    }

    partial void OnStartMinimizedToTrayChanged(bool value)
    {
        if (_suppressSettingsSave)
        {
            return;
        }

        try
        {
            _appSettings.StartMinimizedToTray = value;
            _settingsService.Save(_appSettings);
            if (AutoStartAppOnLogon)
            {
                _startupService.SetAppAutoStart(true, Environment.ProcessPath, value);
            }
        }
        catch (Exception ex)
        {
            SetError(ex.Message);
        }
    }

    partial void OnKeepEngineRunningOnExitChanged(bool value)
    {
        if (!_suppressSettingsSave)
        {
            _appSettings.KeepEngineRunningOnExit = value;
            _settingsService.Save(_appSettings);
        }
    }

    partial void OnLastErrorMessageChanged(string value)
        => HasLastError = !string.IsNullOrWhiteSpace(value);

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

        if (!_suppressSettingsSave)
        {
            _appSettings.RpcServerEnabled = value;
            _settingsService.Save(_appSettings);
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
            case nameof(DynamicChannelModel.Muted):
                if (ch.IsHardwareInput)
                {
                    IsMicMuted = ch.Muted;
                }
                _pendingChannelMixIds.Add(ch.ChannelId);
                RestartMixDebounce();
                break;
            case nameof(DynamicChannelModel.Volume):
            case nameof(DynamicChannelModel.Solo):
            case nameof(DynamicChannelModel.Pan):
                _pendingChannelMixIds.Add(ch.ChannelId);
                RestartMixDebounce();
                break;
            case nameof(DynamicChannelModel.SendA1):
                _ = SafeFireAsync(() => _pipeClient.SetChannelSendAsync(ch.ChannelId, "A1", ch.SendA1));
                break;
            case nameof(DynamicChannelModel.SendA2):
                _ = SafeFireAsync(() => _pipeClient.SetChannelSendAsync(ch.ChannelId, "A2", ch.SendA2));
                break;
            case nameof(DynamicChannelModel.SendB1):
                _ = SafeFireAsync(() => _pipeClient.SetChannelSendAsync(ch.ChannelId, "B1", ch.SendB1));
                break;
            case nameof(DynamicChannelModel.SendB2):
                _ = SafeFireAsync(() => _pipeClient.SetChannelSendAsync(ch.ChannelId, "B2", ch.SendB2));
                break;
            case nameof(DynamicChannelModel.DeviceId):
                _ = SafeFireAsync(() => _pipeClient.BindChannelSourceAsync(ch.ChannelId, ch.SourceType, ch.DeviceId));
                UpdateHealthCheck();
                UpdateActiveMicDisplayName();
                break;
            case nameof(DynamicChannelModel.EqLowDb):
            case nameof(DynamicChannelModel.EqMidDb):
            case nameof(DynamicChannelModel.EqHighDb):
            case nameof(DynamicChannelModel.CompEnabled):
            case nameof(DynamicChannelModel.CompThresholdDb):
            case nameof(DynamicChannelModel.DuckSourceId):
            case nameof(DynamicChannelModel.DuckDepthDb):
                _pendingChannelFullDspIds.Add(ch.ChannelId);
                RestartMixDebounce();
                break;
        }
    }

    private void OnOutputBusPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_suppressSync || sender is not OutputBusModel bus)
        {
            return;
        }

        if (e.PropertyName is nameof(OutputBusModel.DeviceId))
        {
            _ = SafeFireAsync(() => _pipeClient.BindOutputBusAsync(bus.BusId, bus.DeviceId, bus.Volume, bus.Muted));
            UpdateHealthCheck();
        }
        else if (e.PropertyName is nameof(OutputBusModel.Volume) or nameof(OutputBusModel.Muted))
        {
            _pendingBusMixIds.Add(bus.BusId);
            RestartMixDebounce();
        }
    }

    private void RestartMixDebounce()
    {
        _mixDebounceTimer.Stop();
        _mixDebounceTimer.Start();
    }

    private async Task SafeFireAsync(Func<Task<EnginePipeResponse>> action)
    {
        try
        {
            EnginePipeResponse resp = await action();
            if (!resp.Ok && !string.IsNullOrWhiteSpace(resp.Error))
            {
                SetError(resp.Error);
            }
        }
        catch (Exception ex)
        {
            SetError(ex.Message);
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
        if (_telemetryReader.TryRead(out TelemetryHeader header, _channelBuffer, _busBuffer, SpectrumDb))
        {
            SpectrumUpdated?.Invoke(this, EventArgs.Empty);
            EngineState state = Enum.IsDefined(typeof(EngineState), header.EngineState)
                ? (EngineState)header.EngineState
                : EngineState.Stopped;

            bool wasConnected = IsEngineConnected;
            bool nowConnected = state != EngineState.Stopped;
            IsEngineConnected = nowConnected;
            if (wasConnected != nowConnected)
            {
                SetEngineProcessState(nowConnected);
            }

            EngineState = state;
            SampleRate = header.SampleRate > 0 ? header.SampleRate : 48000;
            BufferFrames = header.BufferFrames;
            CpuLoadPercent = header.CpuLoadPercent;

            if (nowConnected &&
                (!wasConnected ||
                 ((OutputDevices.Count == 0 || InputDevices.Count == 0) &&
                  (DateTime.UtcNow - _lastAutoSyncUtc).TotalSeconds >= _autoSyncBackoffSeconds)) &&
                !_isSyncingFromEngine)
            {
                _ = SyncAllFromEngineAsync();
            }

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

            DynamicChannelModel? mic = _cachedPrimaryMic;
            MicGainReductionNormalized = mic is not null
                ? Math.Clamp(MathF.Abs(mic.GainReductionDb) / 24.0f, 0.0f, 1.0f)
                : 0f;
            MicLiveLevelNormalized = mic is not null
                ? Math.Clamp(MathF.Max(mic.PeakLeft, mic.PeakRight), 0.0f, 1.0f)
                : 0f;
        }
        else if (IsEngineConnected)
        {
            IsEngineConnected = false;
            SetEngineProcessState(false);
            _autoSyncBackoffSeconds = 2.0;
            EngineState = EngineState.Stopped;
            CpuLoadPercent = 0f;
            MicGainReductionNormalized = 0f;
            MicLiveLevelNormalized = 0f;

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

    private void SetEngineProcessState(bool running)
    {
        IsEngineProcessRunning = running;
        IsEngineRunning = running;
        EngineActionKey = running ? "Io_StopEngine" : "Io_LaunchEngine";
    }

    private void SetError(string? message)
        => LastErrorMessage = message ?? string.Empty;

    private void ClearError()
        => LastErrorMessage = string.Empty;

    private void UpdateDeviceEmptyStates()
    {
        HasNoOutputDevices = OutputDevices.Count == 0;
        HasNoInputDevices = InputDevices.Count == 0;
        HasNoAudioSessions = AudioSessions.Count == 0;
        HasNoPlugins = DiscoveredPlugins.Count == 0;
        UpdateActiveMicDisplayName();
    }

    private void UpdateActiveMicDisplayName()
    {
        string? devId = PrimaryMicChannel?.DeviceId;
        if (!string.IsNullOrWhiteSpace(devId))
        {
            AudioDeviceInfo? match = InputDevices.FirstOrDefault(d => string.Equals(d.Id, devId, StringComparison.OrdinalIgnoreCase));
            if (match is not null)
            {
                ActiveMicDisplayName = match.Name;
                return;
            }
        }

        ActiveMicDisplayName = InputDevices.FirstOrDefault(d => d.IsDefault)?.Name
                               ?? LocalizationService.Instance.GetString("Status_Standby");
    }

    private void UpdateHealthCheck()
    {
        bool a1Ready = OutputBuses.Count > 0 && !string.IsNullOrWhiteSpace(OutputBuses[0].DeviceId);
        bool micReady = DynamicChannels.Any(c => c.IsHardwareInput && !string.IsNullOrWhiteSpace(c.DeviceId));
        bool ready = a1Ready && micReady;
        IsHealthWarning = !ready;
        HealthMessageKey = ready ? "Health_AllReady" : "Health_MissingA1";
        HealthWarningMessage = LocalizationService.Instance.GetString(HealthMessageKey);
    }

    private void RefreshLocalizedStatusStrings(bool force = false)
    {
        if (!force &&
            _lastStatusEngineState == EngineState &&
            _lastStatusSampleRate == SampleRate &&
            _lastStatusBufferFrames == BufferFrames &&
            _lastStatusConnected == IsEngineConnected)
        {
            return;
        }

        _lastStatusEngineState = EngineState;
        _lastStatusSampleRate = SampleRate;
        _lastStatusBufferFrames = BufferFrames;
        _lastStatusConnected = IsEngineConnected;

        LocalizationService loc = LocalizationService.Instance;

        StatusBadgeKey = EngineState switch
        {
            EngineState.Running => "Status_Connected",
            EngineState.Degraded => "Status_Degraded",
            _ => "Status_Standby",
        };
        EngineStatusBadgeText = loc.GetString(StatusBadgeKey);

        SampleRateDisplay = IsEngineConnected
            ? string.Format(loc.GetString("Status_SampleRateFormat"), SampleRate, BufferFrames)
            : loc.GetString("Status_SampleRateOffline");
        EngineDiagnosticsText = SampleRateDisplay;
    }

    [RelayCommand]
    private void ToggleLanguage() => LocalizationService.Instance.ToggleLanguage();

    [RelayCommand]
    private void ToggleMicMute()
    {
        DynamicChannelModel? mic = PrimaryMicChannel;
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
        string name = string.IsNullOrWhiteSpace(NewChannelName)
            ? LocalizationService.Instance.GetString("Ch_DefaultName")
            : NewChannelName.Trim();
        if (name == "Ch_DefaultName")
        {
            name = "Custom";
        }

        string sourceType = NewChannelSourceIndex switch
        {
            1 => "hardware_input",
            2 => "process_loopback",
            _ => "app_group",
        };

        string[] palette = ["#A855F7", "#10B981", "#F59E0B", "#EC4899", "#38BDF8"];
        string color = palette[DynamicChannels.Count % palette.Length];

        (bool ok, uint createdId) = await _pipeClient.CreateChannelAsync(name, sourceType, color);
        if (ok)
        {
            if (!string.IsNullOrWhiteSpace(NewChannelAppProcess))
            {
                await _pipeClient.BindAppToChannelAsync(NewChannelAppProcess.Trim(), createdId);
            }
            await RefreshV2MixerStateAsync();
        }
        else
        {
            uint nextId = DynamicChannels.Count > 0 ? DynamicChannels.Max(c => c.ChannelId) + 1 : 2;
            var ch = new DynamicChannelModel
            {
                ChannelId = nextId,
                Name = name,
                SourceType = sourceType,
                Color = color,
                SendA1 = sourceType != "process_loopback",
                SendB2 = true,
            };
            if (!string.IsNullOrWhiteSpace(NewChannelAppProcess))
            {
                ch.BoundApps.Add(NewChannelAppProcess.Trim());
            }
            DynamicChannels.Add(ch);
            AttachChannelHandlers();
        }

        NewChannelName = string.Empty;
        NewChannelAppProcess = string.Empty;
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
        _cachedPrimaryMic = DynamicChannels.FirstOrDefault(c => c.IsHardwareInput);
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
            ClearError();
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
            DynamicChannelModel? mic = PrimaryMicChannel;
            if (defMic is not null && mic is not null)
            {
                mic.DeviceId = defMic.Id;
            }
            UpdateHealthCheck();
            UpdateActiveMicDisplayName();
        }
    }

    [RelayCommand]
    private async Task ImportAutoEqAsync()
    {
        var dialog = new OpenFileDialog
        {
            Filter = "AutoEq Parametric EQ (*.txt)|*.txt|All Files (*.*)|*.*",
        };

        if (dialog.ShowDialog() == true && File.Exists(dialog.FileName))
        {
            string content = await File.ReadAllTextAsync(dialog.FileName);
            AutoEqProfile profile = AutoEqParser.Parse(content);
            AutoEqParser.ApplyTo(profile, MicDsp);
        }
    }

    [RelayCommand]
    private async Task ScanPluginsAsync()
    {
        IReadOnlyList<DiscoveredPlugin> plugins = await Task.Run(PluginScanner.ScanStandardDirectories);
        DiscoveredPlugins.Clear();
        foreach (DiscoveredPlugin plugin in plugins)
        {
            DiscoveredPlugins.Add(plugin);
        }
        HasNoPlugins = DiscoveredPlugins.Count == 0;
    }

    [RelayCommand]
    private async Task PingEngineAsync()
    {
        EnginePipeResponse response = await _pipeClient.PingAsync(500);
        if (response.Ok)
        {
            ClearError();
            LastPingMessage = LocalizationService.Instance.GetString("Status_Connected");
        }
        else
        {
            SetError(response.Error ?? "Offline");
            LastPingMessage = response.Error ?? LocalizationService.Instance.GetString("Status_Standby");
        }
    }

    [RelayCommand]
    private async Task LaunchOrStopEngineAsync()
    {
        if (_processManager.IsEngineDetected())
        {
            await _processManager.StopEngineAsync(_pipeClient);
            SetEngineProcessState(false);
            LastPingMessage = LocalizationService.Instance.GetString("Status_Standby");
            return;
        }

        if (_processManager.TryStartEngine(out string? err))
        {
            ClearError();
            SetEngineProcessState(true);
            LastPingMessage = LocalizationService.Instance.GetString("Status_Connected");
            for (int i = 0; i < 15; i++)
            {
                EnginePipeResponse ping = await _pipeClient.PingAsync(200);
                if (ping.Ok)
                {
                    break;
                }
                await Task.Delay(100);
            }
            await SyncAllFromEngineAsync();
        }
        else
        {
            SetError(err ?? "Launch failed");
            LastPingMessage = err ?? "Launch failed";
        }
    }

    [RelayCommand]
    private async Task RestartEngineAsync()
    {
        (bool ok, string? err) = await _processManager.RestartEngineAsync(_pipeClient);
        if (!ok)
        {
            SetEngineProcessState(false);
            SetError(err ?? "Restart failed");
            return;
        }

        ClearError();
        SetEngineProcessState(true);
        for (int i = 0; i < 15; i++)
        {
            EnginePipeResponse ping = await _pipeClient.PingAsync(200);
            if (ping.Ok)
            {
                break;
            }
            await Task.Delay(100);
        }

        await SyncAllFromEngineAsync();
    }

    [RelayCommand]
    private async Task RefreshDevicesAsync()
    {
        PipeResult<IReadOnlyList<AudioDeviceInfo>> res = await _pipeClient.ListDevicesResultAsync();
        if (!res.Ok)
        {
            SetError(res.Error ?? "Failed to list audio devices.");
            UpdateDeviceEmptyStates();
            return;
        }

        ClearError();
        IReadOnlyList<AudioDeviceInfo> devices = res.Data;
        List<AudioDeviceInfo> nextInputs = [];
        List<AudioDeviceInfo> nextOutputs = [];
        foreach (AudioDeviceInfo dev in devices)
        {
            if (dev.IsInput)
            {
                nextInputs.Add(dev);
            }
            else
            {
                nextOutputs.Add(dev);
            }
        }

        _suppressSync = true;
        try
        {
            ReconcileDeviceCollection(InputDevices, nextInputs);
            ReconcileDeviceCollection(OutputDevices, nextOutputs);
        }
        finally
        {
            _suppressSync = false;
        }

        UpdateDeviceEmptyStates();
        OnPropertyChanged(nameof(PrimaryOutputBus));
        OnPropertyChanged(nameof(PrimaryMicChannel));
        UpdateHealthCheck();
    }

    private static void ReconcileDeviceCollection(ObservableCollection<AudioDeviceInfo> target, IReadOnlyList<AudioDeviceInfo> incoming)
    {
        var incomingById = new Dictionary<string, AudioDeviceInfo>(StringComparer.OrdinalIgnoreCase);
        foreach (AudioDeviceInfo dev in incoming)
        {
            incomingById[dev.Id] = dev;
        }

        for (int i = target.Count - 1; i >= 0; i--)
        {
            if (!incomingById.ContainsKey(target[i].Id))
            {
                target.RemoveAt(i);
            }
        }

        for (int i = 0; i < incoming.Count; i++)
        {
            AudioDeviceInfo src = incoming[i];
            int existingIdx = -1;
            for (int j = 0; j < target.Count; j++)
            {
                if (string.Equals(target[j].Id, src.Id, StringComparison.OrdinalIgnoreCase))
                {
                    existingIdx = j;
                    break;
                }
            }

            if (existingIdx >= 0)
            {
                AudioDeviceInfo existing = target[existingIdx];
                existing.Name = src.Name;
                existing.IsCapture = src.IsCapture;
                existing.IsDefault = src.IsDefault;
                existing.IsVirtual = src.IsVirtual;
                if (existingIdx != i)
                {
                    target.Move(existingIdx, i);
                }
            }
            else
            {
                target.Insert(i, src);
            }
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
                ReconcileChannelsInPlace(channels);
                AttachChannelHandlers();
            }

            if (buses.Count > 0)
            {
                ReconcileBusesInPlace(buses);
                AttachBusHandlers();
            }
        }
        finally
        {
            _suppressSync = false;
        }

        IsMicMuted = PrimaryMicChannel?.Muted ?? false;
        OnPropertyChanged(nameof(PrimaryOutputBus));
        OnPropertyChanged(nameof(PrimaryMicChannel));
        UpdateHealthCheck();
        UpdateActiveMicDisplayName();
    }

    private void ReconcileChannelsInPlace(IReadOnlyList<DynamicChannelModel> incoming)
    {
        var incomingById = incoming.ToDictionary(c => c.ChannelId);

        for (int i = DynamicChannels.Count - 1; i >= 0; i--)
        {
            if (!incomingById.ContainsKey(DynamicChannels[i].ChannelId))
            {
                DynamicChannels.RemoveAt(i);
            }
        }

        for (int i = 0; i < incoming.Count; i++)
        {
            DynamicChannelModel src = incoming[i];
            DynamicChannelModel? existing = DynamicChannels.FirstOrDefault(c => c.ChannelId == src.ChannelId);
            if (existing is not null)
            {
                existing.Name = src.Name;
                existing.SourceType = src.SourceType;
                if (!string.IsNullOrWhiteSpace(src.DeviceId))
                {
                    existing.DeviceId = src.DeviceId;
                }
                existing.Color = src.Color;
                existing.IsSystem = src.IsSystem;
                existing.Volume = src.Volume;
                existing.Muted = src.Muted;
                existing.Solo = src.Solo;
                existing.Pan = src.Pan;
                existing.SendA1 = src.SendA1;
                existing.SendA2 = src.SendA2;
                existing.SendB1 = src.SendB1;
                existing.SendB2 = src.SendB2;

                existing.BoundApps.Clear();
                foreach (string app in src.BoundApps)
                {
                    existing.BoundApps.Add(app);
                }
            }
            else
            {
                DynamicChannels.Insert(Math.Min(i, DynamicChannels.Count), src);
            }
        }
    }

    private void ReconcileBusesInPlace(IReadOnlyList<OutputBusModel> incoming)
    {
        var incomingById = incoming.ToDictionary(b => b.BusId, StringComparer.OrdinalIgnoreCase);

        for (int i = OutputBuses.Count - 1; i >= 0; i--)
        {
            if (!incomingById.ContainsKey(OutputBuses[i].BusId))
            {
                OutputBuses.RemoveAt(i);
            }
        }

        for (int i = 0; i < incoming.Count; i++)
        {
            OutputBusModel src = incoming[i];
            OutputBusModel? existing = OutputBuses.FirstOrDefault(b => string.Equals(b.BusId, src.BusId, StringComparison.OrdinalIgnoreCase));
            if (existing is not null)
            {
                existing.Role = src.Role;
                existing.Name = src.Name;
                if (!string.IsNullOrWhiteSpace(src.DeviceId))
                {
                    existing.DeviceId = src.DeviceId;
                }
                existing.DeviceName = src.DeviceName;
                existing.IsVirtual = src.IsVirtual;
                existing.Volume = src.Volume;
                existing.Muted = src.Muted;
                existing.Active = src.Active;
            }
            else
            {
                OutputBuses.Insert(Math.Min(i, OutputBuses.Count), src);
            }
        }
    }

    [RelayCommand]
    private async Task RefreshSessionsAsync()
    {
        PipeResult<IReadOnlyList<AudioSessionInfo>> res = await _pipeClient.ListSessionsResultAsync();
        if (!res.Ok)
        {
            SetError(res.Error ?? "Failed to list audio sessions.");
            HasNoAudioSessions = AudioSessions.Count == 0;
            return;
        }

        AudioSessions.Clear();
        foreach (AudioSessionInfo session in res.Data)
        {
            AudioSessions.Add(session);
        }
        HasNoAudioSessions = AudioSessions.Count == 0;
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
        string processName = string.IsNullOrWhiteSpace(session.AppName) ? session.EffectiveLabel : session.AppName;
        string channelName = channelId.ToString();

        // Find existing channel matching Game / Chat / Media / Aux, or create one automatically
        DynamicChannelModel? targetCh = DynamicChannels.FirstOrDefault(
            c => !c.IsSystem && !c.IsHardwareInput &&
                 string.Equals(c.Name, channelName, StringComparison.OrdinalIgnoreCase));

        if (targetCh is null)
        {
            string color = channelId switch
            {
                MixerChannelId.Game => "#A855F7",
                MixerChannelId.Chat => "#10B981",
                MixerChannelId.Media => "#F59E0B",
                _ => "#EC4899",
            };
            (bool created, uint newId) = await _pipeClient.CreateChannelAsync(channelName, "app_group", color);
            if (created)
            {
                await RefreshV2MixerStateAsync();
                targetCh = DynamicChannels.FirstOrDefault(c => c.ChannelId == newId);
            }
        }

        if (targetCh is not null)
        {
            EnginePipeResponse bindResp = await _pipeClient.BindAppToChannelAsync(processName, targetCh.ChannelId);
            if (bindResp.Ok)
            {
                await RefreshV2MixerStateAsync();
                await RefreshRoutingRulesAsync();
                await RefreshSessionsAsync();
                return;
            }
        }

        string targetDeviceId = !string.IsNullOrWhiteSpace(session.TargetDeviceId)
            ? session.TargetDeviceId
            : (targetCh?.DeviceId ?? string.Empty);

        var rule = new RoutingRule
        {
            ProcessName = processName,
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
    private async Task EnableVirtualSystemDefaultAsync()
    {
        EnginePipeResponse resp = await _pipeClient.SetSystemDefaultVirtualAsync(true);
        if (resp.Ok)
        {
            ClearError();
            await RefreshDevicesAsync();
            await RefreshV2MixerStateAsync();
        }
        else
        {
            SetError(resp.Error ?? "Could not set AtrisWave System as default Windows playback device.");
        }
    }

    [RelayCommand]
    private async Task RestorePhysicalSystemDefaultAsync()
    {
        EnginePipeResponse resp = await _pipeClient.SetSystemDefaultVirtualAsync(false);
        if (resp.Ok)
        {
            ClearError();
            await RefreshDevicesAsync();
        }
        else
        {
            SetError(resp.Error ?? "Could not restore physical Windows default playback device.");
        }
    }

    [RelayCommand]
    private void InstallVirtualDriver()
    {
        try
        {
            string? scriptPath = FindDriverInstallScript();
            if (scriptPath is null)
            {
                SetError("Driver install script (driver\\scripts\\build-and-testsign.ps1) not found.");
                return;
            }

            var psi = new System.Diagnostics.ProcessStartInfo
            {
                FileName = "powershell.exe",
                Arguments = $"-ExecutionPolicy Bypass -NoExit -File \"{scriptPath}\" -Install",
                UseShellExecute = true,
                Verb = "runas",
            };
            System.Diagnostics.Process.Start(psi);
        }
        catch (Exception ex)
        {
            SetError(ex.Message);
        }
    }

    private static string? FindDriverInstallScript()
    {
        DirectoryInfo? dir = new(AppContext.BaseDirectory);
        for (int i = 0; i < 6 && dir is not null; i++)
        {
            string candidate = Path.Combine(dir.FullName, "driver", "scripts", "build-and-testsign.ps1");
            if (File.Exists(candidate))
            {
                return candidate;
            }
            dir = dir.Parent;
        }
        return null;
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
                return await Application.Current.Dispatcher.InvokeAsync(() =>
                {
                    DynamicChannelModel? mic = PrimaryMicChannel;
                    return (object)new
                    {
                        connected = IsEngineConnected,
                        sample_rate = SampleRate,
                        buffer_frames = BufferFrames,
                        mic_muted = mic?.Muted ?? false,
                    };
                });
            case "toggle_mic_mute":
                await Application.Current.Dispatcher.InvokeAsync(ToggleMicMute);
                return await Application.Current.Dispatcher.InvokeAsync(() =>
                    (object)new { mic_muted = PrimaryMicChannel?.Muted ?? false });
            case "set_channel_volume":
                if (parameters is JsonElement pVol &&
                    pVol.TryGetProperty("channel_id", out JsonElement chEl) &&
                    pVol.TryGetProperty("volume", out JsonElement volEl))
                {
                    uint cid = chEl.GetUInt32();
                    float vol = volEl.GetSingle();
                    bool updated = await Application.Current.Dispatcher.InvokeAsync(() =>
                    {
                        DynamicChannelModel? ch = DynamicChannels.FirstOrDefault(c => c.ChannelId == cid);
                        if (ch is null) return false;
                        ch.Volume = Math.Clamp(vol, 0f, 1.5f);
                        return true;
                    });
                    return new { ok = updated };
                }
                return new { ok = false };
            case "set_channel_mute":
                if (parameters is JsonElement pMute &&
                    pMute.TryGetProperty("channel_id", out JsonElement chM) &&
                    pMute.TryGetProperty("muted", out JsonElement mEl))
                {
                    uint cid = chM.GetUInt32();
                    bool muted = mEl.GetBoolean();
                    bool updated = await Application.Current.Dispatcher.InvokeAsync(() =>
                    {
                        DynamicChannelModel? ch = DynamicChannels.FirstOrDefault(c => c.ChannelId == cid);
                        if (ch is null) return false;
                        ch.Muted = muted;
                        return true;
                    });
                    return new { ok = updated };
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

    [RelayCommand]
    private void SelectNav(string? indexStr)
    {
        if (int.TryParse(indexStr, out int idx))
        {
            SelectedNavIndex = idx;
        }
    }

    [RelayCommand]
    private void ApplyMicVoicePreset(string? presetName)
    {
        if (Enum.TryParse<MicVoicePresetId>(presetName, out var preset))
        {
            MicVoicePresets.ApplyPreset(preset, MicDsp);
        }
    }

    [RelayCommand]
    private void SetDenoiseLevel(string? level)
    {
        switch (level)
        {
            case "Light":
                MicDsp.DenoiseEnabled = true;
                MicDsp.DenoiseStrength = 0.20f;
                break;
            case "Medium":
                MicDsp.DenoiseEnabled = true;
                MicDsp.DenoiseStrength = 0.45f;
                break;
            case "Strong":
                MicDsp.DenoiseEnabled = true;
                MicDsp.DenoiseStrength = 0.75f;
                break;
            default:
                MicDsp.DenoiseEnabled = false;
                break;
        }
    }

    public async Task EnsureEngineAutoStartedAsync()
    {
        if (Interlocked.Exchange(ref _startupInitialized, 1) != 0)
        {
            if (!IsEngineConnected || OutputDevices.Count == 0 || InputDevices.Count == 0)
            {
                await SyncAllFromEngineAsync();
            }
            return;
        }

        EnginePipeResponse ping = await _pipeClient.PingAsync(200);
        if (!ping.Ok && !_processManager.IsEngineDetected())
        {
            if (_processManager.TryStartEngine(out string? startErr))
            {
                SetEngineProcessState(true);
                ClearError();
            }
            else
            {
                SetEngineProcessState(false);
                SetError(startErr ?? "AtrisWaveEngine.exe could not be started.");
                return;
            }
        }
        else if (ping.Ok || _processManager.IsEngineDetected())
        {
            SetEngineProcessState(true);
        }

        if (!ping.Ok)
        {
            for (int attempt = 0; attempt < 20; attempt++)
            {
                ping = await _pipeClient.PingAsync(200);
                if (ping.Ok)
                {
                    break;
                }

                await Task.Delay(100);
            }
        }

        if (!ping.Ok)
        {
            SetError(ping.Error ?? "Engine did not respond on control pipe.");
        }
        else
        {
            ClearError();
        }

        await SyncAllFromEngineAsync();
        _ = CheckForUpdatesDelayedAsync();
    }

    private async Task SyncAllFromEngineAsync()
    {
        if (_isSyncingFromEngine)
        {
            return;
        }

        _isSyncingFromEngine = true;
        _lastAutoSyncUtc = DateTime.UtcNow;
        try
        {
            await RefreshDevicesAsync();
            await RefreshV2MixerStateAsync();
            await RefreshSessionsAsync();
            await RefreshDriverStatusAsync();
            await RefreshRoutingRulesAsync();

            if (IsHealthWarning)
            {
                await AutoSetupDefaultsAsync();
            }

            if (OutputDevices.Count > 0 && InputDevices.Count > 0)
            {
                _autoSyncBackoffSeconds = 2.0;
            }
            else
            {
                _autoSyncBackoffSeconds = Math.Min(30.0, _autoSyncBackoffSeconds * 2.0);
            }

            OnPropertyChanged(nameof(PrimaryOutputBus));
            OnPropertyChanged(nameof(PrimaryMicChannel));
        }
        catch (Exception ex)
        {
            SetError(ex.Message);
        }
        finally
        {
            _isSyncingFromEngine = false;
        }
    }

    private async Task CheckForUpdatesDelayedAsync()
    {
        await Task.Delay(2000);
        if (!_disposed)
        {
            await CheckForUpdatesAsync();
        }
    }

    private static string ResolveAppVersion()
    {
        string? info = Assembly.GetEntryAssembly()?
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
            .InformationalVersion;
        if (!string.IsNullOrWhiteSpace(info))
        {
            int plus = info.IndexOf('+');
            return plus >= 0 ? info[..plus] : info;
        }

        return "0.1.3";
    }

    [RelayCommand]
    private void OpenChannelDrawer(DynamicChannelModel? channel)
    {
        if (channel is null) return;
        SelectedDrawerChannel = channel;
        IsChannelDrawerOpen = true;
    }

    [RelayCommand]
    private void CloseChannelDrawer() => IsChannelDrawerOpen = false;

    [RelayCommand]
    private async Task FocusSpectrumOnChannelAsync(DynamicChannelModel? channel)
    {
        if (channel is null) return;
        await _pipeClient.SetSpectrumSourceAsync(channel.ChannelId);
    }

    [RelayCommand]
    private void BrowseSoundboardWav(SoundboardPadModel? pad)
    {
        if (pad is null) return;
        var dlg = new OpenFileDialog { Filter = "WAV Audio (*.wav)|*.wav|All Files (*.*)|*.*" };
        if (dlg.ShowDialog() == true)
        {
            pad.FilePath = dlg.FileName;
            if (string.IsNullOrWhiteSpace(pad.Title) || pad.Title.StartsWith("Pad "))
            {
                pad.Title = Path.GetFileNameWithoutExtension(dlg.FileName);
            }
        }
    }

    [RelayCommand]
    private async Task PlaySoundboardPadAsync(SoundboardPadModel? pad)
    {
        if (pad is null || !pad.HasFile) return;
        await _pipeClient.PlaySoundboardAsync(pad.FilePath, pad.Gain, pad.SendA1, pad.SendB1, pad.SendB2, pad.Loop);
    }

    [RelayCommand]
    private async Task StopAllSoundboardAsync()
        => await _pipeClient.StopSoundboardAsync(-1);

    [RelayCommand]
    private void ToggleA1A2QuickOutput()
    {
        if (OutputBuses.Count < 2) return;
        bool a1Muted = OutputBuses[0].Muted;
        OutputBuses[0].Muted = !a1Muted;
        OutputBuses[1].Muted = a1Muted;
    }

    [RelayCommand]
    private async Task CheckForUpdatesAsync()
    {
        UpdateCheckResult res = await GitHubUpdateChecker.CheckForUpdatesAsync(CurrentAppVersion);
        if (!res.Success)
        {
            UpdateStatusText = res.Error ?? LocalizationService.Instance.GetString("Status_Standby");
            IsUpdateAvailable = false;
            CanDownloadUpdate = false;
            return;
        }

        IsUpdateAvailable = res.UpdateAvailable;
        _updateLatestVersion = res.LatestVersion;
        UpdateReleaseUrl = res.ReleaseUrl;
        UpdateInstallerUrl = res.InstallerDownloadUrl ?? string.Empty;
        UpdateChecksumsUrl = res.ChecksumsDownloadUrl ?? string.Empty;
        UpdateReleaseNotes = res.ReleaseNotes ?? string.Empty;
        CanDownloadUpdate = res.UpdateAvailable && !string.IsNullOrWhiteSpace(UpdateInstallerUrl);
        UpdateStatusText = res.UpdateAvailable
            ? $"{LocalizationService.Instance.GetString("Update_Available")} {res.LatestVersion}"
            : LocalizationService.Instance.GetString("Update_UpToDate");
    }

    [RelayCommand]
    private async Task DownloadAndInstallUpdateAsync()
    {
        if (IsDownloadingUpdate || string.IsNullOrWhiteSpace(UpdateInstallerUrl))
        {
            return;
        }

        IsDownloadingUpdate = true;
        UpdateDownloadProgress = 0;
        UpdateStatusText = LocalizationService.Instance.GetString("Update_Downloading");

        try
        {
            var progress = new Progress<double>(p => UpdateDownloadProgress = p * 100.0);
            string tagForFile = string.IsNullOrWhiteSpace(_updateLatestVersion) ? CurrentAppVersion : _updateLatestVersion;
            string installerPath = await AppUpdaterService.DownloadAndVerifyInstallerAsync(
                UpdateInstallerUrl,
                string.IsNullOrWhiteSpace(UpdateChecksumsUrl) ? null : UpdateChecksumsUrl,
                tagForFile,
                progress);

            await ShutdownEngineOnExitAsync();
            Process.Start(new ProcessStartInfo { FileName = installerPath, UseShellExecute = true });
            Application.Current?.Shutdown();
        }
        catch (Exception ex)
        {
            UpdateStatusText = ex.Message;
        }
        finally
        {
            IsDownloadingUpdate = false;
        }
    }

    [RelayCommand]
    private void OpenUpdateRelease()
    {
        if (UpdateReleaseUrl.StartsWith("https://github.com/", StringComparison.OrdinalIgnoreCase))
        {
            Process.Start(new ProcessStartInfo { FileName = UpdateReleaseUrl, UseShellExecute = true });
        }
    }

    private static ObservableCollection<SoundboardPadModel> CreateDefaultSoundboardPads()
    {
        string[] palette = ["#38BDF8", "#A855F7", "#10B981", "#F59E0B", "#EC4899", "#06B6D4"];
        var list = new ObservableCollection<SoundboardPadModel>();
        for (int i = 0; i < 12; i++)
        {
            list.Add(new SoundboardPadModel
            {
                PadIndex = i,
                Title = $"Pad {i + 1}",
                AccentColor = palette[i % palette.Length],
            });
        }
        return list;
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
        LocalizationService.Instance.PropertyChanged -= OnLocalizationPropertyChanged;
        MicDsp.PropertyChanged -= OnMicDspPropertyChanged;
        foreach (DynamicChannelModel ch in DynamicChannels)
        {
            ch.PropertyChanged -= OnDynamicChannelPropertyChanged;
        }
        foreach (OutputBusModel bus in OutputBuses)
        {
            bus.PropertyChanged -= OnOutputBusPropertyChanged;
        }

        _telemetryTimer.Stop();
        _telemetryTimer.Tick -= OnTelemetryTick;
        _micDspDebounceTimer.Stop();
        _micDspDebounceTimer.Tick -= OnMicDspDebounceTick;
        _mixDebounceTimer.Stop();
        _mixDebounceTimer.Tick -= OnMixDebounceTick;

        _telemetryReader.Dispose();
        _pipeClient.Dispose();
        _processManager.Dispose();
        _rpcServer.Dispose();
    }
}
