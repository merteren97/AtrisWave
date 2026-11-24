using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using AtrisWave.Core.Models;

namespace AtrisWave.Core.Ipc;

/// <summary>
/// Response envelope returned by the native engine over the control pipe.
/// </summary>
public sealed record EnginePipeResponse(bool Ok, int Id, JsonElement? Payload, string? Error);

public sealed record SetChannelRequest(
    [property: JsonPropertyName("channel_id")] uint ChannelId,
    [property: JsonPropertyName("volume")] float Volume,
    [property: JsonPropertyName("muted")] bool Muted,
    [property: JsonPropertyName("solo")] bool Solo);

public sealed record SelectDevicesRequest(
    [property: JsonPropertyName("input_id")] string? InputId,
    [property: JsonPropertyName("monitor_id")] string? MonitorId,
    [property: JsonPropertyName("output_id")] string? OutputId);

public sealed record RouteAppRequest(
    [property: JsonPropertyName("pid")] uint Pid,
    [property: JsonPropertyName("target_device_id")] string TargetDeviceId,
    [property: JsonPropertyName("loopback_capture")] bool LoopbackCapture);

public sealed record PresetRequest([property: JsonPropertyName("name")] string Name);

/// <summary>
/// Async newline-delimited UTF-8 JSON client for the <c>AtrisWaveEngine</c> Named Pipe.
/// </summary>
public sealed class EnginePipeClient : IDisposable
{
    private readonly string _pipeName;
    private readonly TimeSpan _defaultTimeout;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private int _nextRequestId;
    private bool _disposed;

    public EnginePipeClient(
        string pipeName = TelemetryConstants.PipeName,
        TimeSpan? defaultTimeout = null)
    {
        if (string.IsNullOrWhiteSpace(pipeName))
        {
            throw new ArgumentException("Pipe name must not be empty.", nameof(pipeName));
        }

        _pipeName = pipeName;
        _defaultTimeout = defaultTimeout ?? TimeSpan.FromSeconds(2);
    }

    public Task<EnginePipeResponse> PingAsync(CancellationToken cancellationToken = default)
        => SendCommandAsync("ping", null, cancellationToken);

    public Task<EnginePipeResponse> GetStatusAsync(CancellationToken cancellationToken = default)
        => SendCommandAsync("get_status", null, cancellationToken);

    public Task<EnginePipeResponse> ShutdownAsync(CancellationToken cancellationToken = default)
        => SendCommandAsync("shutdown", null, cancellationToken);

    public async Task<IReadOnlyList<AudioDeviceInfo>> ListDevicesAsync(CancellationToken cancellationToken = default)
    {
        EnginePipeResponse resp = await SendCommandAsync("list_devices", null, cancellationToken).ConfigureAwait(false);
        if (!TryUnwrapResult(resp, out JsonElement result))
        {
            return [];
        }

        if (result.TryGetProperty("devices", out JsonElement devicesEl) && devicesEl.ValueKind == JsonValueKind.Array)
        {
            return JsonSerializer.Deserialize<List<AudioDeviceInfo>>(devicesEl.GetRawText()) ?? [];
        }

        return [];
    }

    public async Task<IReadOnlyList<AudioSessionInfo>> ListSessionsAsync(CancellationToken cancellationToken = default)
    {
        EnginePipeResponse resp = await SendCommandAsync("list_sessions", null, cancellationToken).ConfigureAwait(false);
        if (!TryUnwrapResult(resp, out JsonElement result))
        {
            return [];
        }

        if (result.TryGetProperty("sessions", out JsonElement sessionsEl) && sessionsEl.ValueKind == JsonValueKind.Array)
        {
            return JsonSerializer.Deserialize<List<AudioSessionInfo>>(sessionsEl.GetRawText()) ?? [];
        }

        return [];
    }

    public Task<EnginePipeResponse> SetChannelAsync(
        uint channelId,
        float volume,
        bool muted,
        bool solo,
        CancellationToken cancellationToken = default)
        => SendCommandAsync("set_channel", new SetChannelRequest(channelId, volume, muted, solo), cancellationToken);

    public Task<EnginePipeResponse> SetMicDspAsync(MicDspSettings settings, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(settings);
        return SendCommandAsync("set_mic_dsp", settings.ToWireDto(), cancellationToken);
    }

    public Task<EnginePipeResponse> SelectDevicesAsync(
        string? inputId,
        string? monitorId,
        string? outputId,
        CancellationToken cancellationToken = default)
        => SendCommandAsync("select_devices", new SelectDevicesRequest(inputId, monitorId, outputId), cancellationToken);

    public Task<EnginePipeResponse> RouteAppAsync(
        uint pid,
        string targetDeviceId,
        bool loopbackCapture,
        CancellationToken cancellationToken = default)
        => SendCommandAsync("route_app", new RouteAppRequest(pid, targetDeviceId, loopbackCapture), cancellationToken);

    public Task<EnginePipeResponse> SavePresetAsync(string name, CancellationToken cancellationToken = default)
        => SendCommandAsync("save_preset", new PresetRequest(name), cancellationToken);

    public Task<EnginePipeResponse> LoadPresetAsync(string name, CancellationToken cancellationToken = default)
        => SendCommandAsync("load_preset", new PresetRequest(name), cancellationToken);

    public async Task<IReadOnlyList<string>> ListPresetsAsync(CancellationToken cancellationToken = default)
    {
        EnginePipeResponse resp = await SendCommandAsync("list_presets", null, cancellationToken).ConfigureAwait(false);
        if (!TryUnwrapResult(resp, out JsonElement result))
        {
            return [];
        }

        if (result.TryGetProperty("presets", out JsonElement presetsEl) && presetsEl.ValueKind == JsonValueKind.Array)
        {
            return JsonSerializer.Deserialize<List<string>>(presetsEl.GetRawText()) ?? [];
        }

        return [];
    }

    public async Task<DriverStatusInfo> GetDriverStatusAsync(CancellationToken cancellationToken = default)
    {
        EnginePipeResponse resp = await SendCommandAsync("get_driver_status", null, cancellationToken).ConfigureAwait(false);
        if (!TryUnwrapResult(resp, out JsonElement result))
        {
            return new DriverStatusInfo();
        }

        return JsonSerializer.Deserialize<DriverStatusInfo>(result.GetRawText()) ?? new DriverStatusInfo();
    }

    public async Task<IReadOnlyList<RoutingRule>> ListRoutingRulesAsync(CancellationToken cancellationToken = default)
    {
        EnginePipeResponse resp = await SendCommandAsync("list_routing_rules", null, cancellationToken).ConfigureAwait(false);
        if (!TryUnwrapResult(resp, out JsonElement result))
        {
            return [];
        }

        if (result.TryGetProperty("rules", out JsonElement rulesEl) && rulesEl.ValueKind == JsonValueKind.Array)
        {
            return JsonSerializer.Deserialize<List<RoutingRule>>(rulesEl.GetRawText()) ?? [];
        }

        return [];
    }

    public Task<EnginePipeResponse> SetRoutingRuleAsync(RoutingRule rule, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(rule);
        return SendCommandAsync("set_routing_rule", rule, cancellationToken);
    }

    public Task<EnginePipeResponse> DeleteRoutingRuleAsync(string processName, CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(processName);
        return SendCommandAsync("delete_routing_rule", new { process_name = processName }, cancellationToken);
    }

    public Task<EnginePipeResponse> ApplyRoutingRulesAsync(CancellationToken cancellationToken = default)
        => SendCommandAsync("apply_routing_rules", null, cancellationToken);

    // --- v2 Dynamic Channel & Output Bus Commands ---

    public async Task<IReadOnlyList<DynamicChannelModel>> ListChannelsAsync(CancellationToken cancellationToken = default)
    {
        EnginePipeResponse resp = await SendCommandAsync("list_channels", null, cancellationToken).ConfigureAwait(false);
        if (!TryUnwrapResult(resp, out JsonElement result))
        {
            return [];
        }

        if (result.TryGetProperty("channels", out JsonElement el) && el.ValueKind == JsonValueKind.Array)
        {
            return JsonSerializer.Deserialize<List<DynamicChannelModel>>(el.GetRawText()) ?? [];
        }

        return [];
    }

    public async Task<(bool Ok, uint ChannelId)> CreateChannelAsync(
        string name,
        string sourceType = "app_group",
        string color = "#A855F7",
        CancellationToken cancellationToken = default)
    {
        EnginePipeResponse resp = await SendCommandAsync(
            "create_channel",
            new { name, source_type = sourceType, color },
            cancellationToken).ConfigureAwait(false);

        if (TryUnwrapResult(resp, out JsonElement result) &&
            result.TryGetProperty("channel_id", out JsonElement idEl))
        {
            return (true, idEl.GetUInt32());
        }

        return (resp.Ok, 0);
    }

    public Task<EnginePipeResponse> DeleteChannelAsync(uint channelId, CancellationToken cancellationToken = default)
        => SendCommandAsync("delete_channel", new { channel_id = channelId }, cancellationToken);

    public Task<EnginePipeResponse> RenameChannelAsync(uint channelId, string name, CancellationToken cancellationToken = default)
        => SendCommandAsync("rename_channel", new { channel_id = channelId, name }, cancellationToken);

    public Task<EnginePipeResponse> BindChannelSourceAsync(
        uint channelId,
        string sourceType,
        string deviceId,
        CancellationToken cancellationToken = default)
        => SendCommandAsync("bind_channel_source", new { channel_id = channelId, source_type = sourceType, device_id = deviceId }, cancellationToken);

    public async Task<IReadOnlyList<OutputBusModel>> ListOutputBusesAsync(CancellationToken cancellationToken = default)
    {
        EnginePipeResponse resp = await SendCommandAsync("list_output_buses", null, cancellationToken).ConfigureAwait(false);
        if (!TryUnwrapResult(resp, out JsonElement result))
        {
            return [];
        }

        if (result.TryGetProperty("buses", out JsonElement el) && el.ValueKind == JsonValueKind.Array)
        {
            return JsonSerializer.Deserialize<List<OutputBusModel>>(el.GetRawText()) ?? [];
        }

        return [];
    }

    public Task<EnginePipeResponse> BindOutputBusAsync(
        string busId,
        string deviceId,
        float volume,
        bool muted,
        CancellationToken cancellationToken = default)
        => SendCommandAsync("bind_output_bus", new { bus_id = busId, device_id = deviceId, volume, muted }, cancellationToken);

    public Task<EnginePipeResponse> SetChannelSendAsync(
        uint channelId,
        string busId,
        bool enabled,
        float sendGain = 1.0f,
        CancellationToken cancellationToken = default)
        => SendCommandAsync("set_channel_send", new { channel_id = channelId, bus_id = busId, enabled, send_gain = sendGain }, cancellationToken);

    public Task<EnginePipeResponse> SetChannelDspAsync(
        uint channelId,
        float volume,
        bool muted,
        bool solo,
        float pan = 0.0f,
        CancellationToken cancellationToken = default)
        => SendCommandAsync("set_channel_dsp", new { channel_id = channelId, volume, muted, solo, pan }, cancellationToken);

    public Task<EnginePipeResponse> BindAppToChannelAsync(
        string processName,
        uint channelId,
        CancellationToken cancellationToken = default)
        => SendCommandAsync("bind_app_to_channel", new { process_name = processName, channel_id = channelId }, cancellationToken);

    public Task<EnginePipeResponse> UnbindAppFromChannelAsync(
        string processName,
        uint channelId,
        CancellationToken cancellationToken = default)
        => SendCommandAsync("unbind_app_from_channel", new { process_name = processName, channel_id = channelId }, cancellationToken);

    public Task<EnginePipeResponse> AutoSetupDefaultsAsync(CancellationToken cancellationToken = default)
        => SendCommandAsync("auto_setup_defaults", null, cancellationToken);

    private static bool TryUnwrapResult(EnginePipeResponse resp, out JsonElement result)
    {
        if (resp.Ok && resp.Payload is JsonElement root)
        {
            if (root.TryGetProperty("result", out JsonElement inner) && inner.ValueKind == JsonValueKind.Object)
            {
                result = inner;
                return true;
            }

            result = root;
            return true;
        }

        result = default;
        return false;
    }

    /// <summary>
    /// Sends a single newline-delimited JSON command and awaits the single-line JSON response.
    /// </summary>
    public async Task<EnginePipeResponse> SendCommandAsync(
        string command,
        object? parameters = null,
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentException.ThrowIfNullOrWhiteSpace(command);

        int id = Interlocked.Increment(ref _nextRequestId);

        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeoutCts.CancelAfter(_defaultTimeout);
            CancellationToken ct = timeoutCts.Token;

            await using var pipe = new NamedPipeClientStream(
                ".",
                _pipeName,
                PipeDirection.InOut,
                PipeOptions.Asynchronous);

            try
            {
                await pipe.ConnectAsync((int)_defaultTimeout.TotalMilliseconds, ct).ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is TimeoutException or IOException or OperationCanceledException)
            {
                return new EnginePipeResponse(false, id, null, $"Engine offline or unreachable: {ex.Message}");
            }

            string jsonLine = parameters is null
                ? JsonSerializer.Serialize(new { id, cmd = command }) + "\n"
                : JsonSerializer.Serialize(new { id, cmd = command, @params = parameters }) + "\n";

            byte[] requestBytes = Encoding.UTF8.GetBytes(jsonLine);
            await pipe.WriteAsync(requestBytes, ct).ConfigureAwait(false);
            await pipe.FlushAsync(ct).ConfigureAwait(false);

            using var reader = new StreamReader(pipe, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
            string? responseLine = await reader.ReadLineAsync(ct).ConfigureAwait(false);

            if (string.IsNullOrWhiteSpace(responseLine))
            {
                return new EnginePipeResponse(false, id, null, "Empty response from engine.");
            }

            using JsonDocument doc = JsonDocument.Parse(responseLine);
            JsonElement root = doc.RootElement.Clone();

            bool ok = root.TryGetProperty("ok", out JsonElement okProp) && okProp.ValueKind == JsonValueKind.True;
            string? error = root.TryGetProperty("error", out JsonElement errProp) && errProp.ValueKind == JsonValueKind.String
                ? errProp.GetString()
                : null;

            return new EnginePipeResponse(ok, id, root, error);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return new EnginePipeResponse(false, id, null, "Pipe request timed out.");
        }
        catch (Exception ex) when (ex is IOException or JsonException)
        {
            return new EnginePipeResponse(false, id, null, ex.Message);
        }
        finally
        {
            _gate.Release();
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _gate.Dispose();
    }
}
