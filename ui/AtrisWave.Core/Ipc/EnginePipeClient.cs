using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace AtrisWave.Core.Ipc;

/// <summary>
/// Response envelope returned by the native engine over the control pipe.
/// </summary>
public sealed record EnginePipeResponse(bool Ok, int Id, JsonElement? Payload, string? Error);

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
