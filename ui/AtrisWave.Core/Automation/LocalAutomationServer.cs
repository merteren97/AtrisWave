using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace AtrisWave.Core.Automation;

/// <summary>
/// Loopback-only (<c>127.0.0.1</c>) lightweight HTTP JSON-RPC 2.0 server for Stream Deck / Touch Portal / scripts.
/// Uses <see cref="TcpListener"/> on <see cref="IPAddress.Loopback"/> so no Admin/URLACL registration is required.
/// </summary>
public sealed class LocalAutomationServer : IDisposable
{
    private const int MaxHeaderBytes = 8192;
    private const int MaxHeaderLines = 32;
    private const int MaxBodyBytes = 65536;
    private static readonly TimeSpan ConnectionTimeout = TimeSpan.FromSeconds(5);

    private readonly int _requestedPort;
    private readonly Func<string, JsonElement?, Task<object?>> _rpcHandler;
    private readonly byte[]? _expectedAuthBytes;

    private TcpListener? _listener;
    private CancellationTokenSource? _cts;
    private Task? _acceptLoopTask;
    private bool _disposed;

    public LocalAutomationServer(
        Func<string, JsonElement?, Task<object?>> rpcHandler,
        int port = 18923,
        string? bearerToken = null)
    {
        _rpcHandler = rpcHandler ?? throw new ArgumentNullException(nameof(rpcHandler));
        _requestedPort = port;
        _expectedAuthBytes = string.IsNullOrWhiteSpace(bearerToken)
            ? null
            : Encoding.UTF8.GetBytes($"Bearer {bearerToken}");
    }

    public bool IsRunning => _listener is not null;
    public int Port => _listener is not null ? ((IPEndPoint)_listener.LocalEndpoint).Port : _requestedPort;

    public void Start()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (IsRunning)
        {
            return;
        }

        _cts = new CancellationTokenSource();
        _listener = new TcpListener(IPAddress.Loopback, _requestedPort);
        _listener.Start();
        _acceptLoopTask = Task.Run(() => AcceptLoopAsync(_cts.Token));
    }

    public async Task StopAsync()
    {
        if (!IsRunning)
        {
            return;
        }

        _cts?.Cancel();
        _listener?.Stop();
        _listener = null;

        if (_acceptLoopTask is not null)
        {
            try
            {
                await _acceptLoopTask.ConfigureAwait(false);
            }
            catch
            {
            }
        }

        _cts?.Dispose();
        _cts = null;
        _acceptLoopTask = null;
    }

    private async Task AcceptLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested && _listener is not null)
        {
            try
            {
                TcpClient client = await _listener.AcceptTcpClientAsync(ct).ConfigureAwait(false);
                _ = Task.Run(() => HandleClientAsync(client, ct), ct);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (ObjectDisposedException)
            {
                break;
            }
            catch (SocketException)
            {
                break;
            }
        }
    }

    private async Task HandleClientAsync(TcpClient client, CancellationToken ct)
    {
        using (client)
        using (var connCts = CancellationTokenSource.CreateLinkedTokenSource(ct))
        {
            connCts.CancelAfter(ConnectionTimeout);
            CancellationToken connCt = connCts.Token;

            try
            {
                await using NetworkStream stream = client.GetStream();

                byte[] headerBuf = new byte[MaxHeaderBytes];
                int totalRead = 0;
                int headerEndOffset = -1;
                int delimiterLen = 0;

                while (totalRead < MaxHeaderBytes)
                {
                    int read = await stream.ReadAsync(headerBuf.AsMemory(totalRead, MaxHeaderBytes - totalRead), connCt).ConfigureAwait(false);
                    if (read == 0)
                    {
                        return;
                    }

                    totalRead += read;
                    headerEndOffset = FindHeaderEnd(headerBuf.AsSpan(0, totalRead), out delimiterLen);
                    if (headerEndOffset >= 0)
                    {
                        break;
                    }
                }

                if (headerEndOffset < 0)
                {
                    await WriteResponseAsync(stream, 431, "Request Header Fields Too Large", """{"error":"Headers too large"}""", connCt).ConfigureAwait(false);
                    return;
                }

                string headerText = Encoding.ASCII.GetString(headerBuf, 0, headerEndOffset);
                string[] lines = headerText.Split(["\r\n", "\n"], StringSplitOptions.None);
                if (lines.Length == 0 || lines.Length > MaxHeaderLines)
                {
                    await WriteResponseAsync(stream, 400, "Bad Request", """{"error":"Invalid headers"}""", connCt).ConfigureAwait(false);
                    return;
                }

                string[] parts = lines[0].Split(' ');
                string method = parts.Length > 0 ? parts[0] : string.Empty;
                string path = parts.Length > 1 ? parts[1] : string.Empty;

                int contentLength = 0;
                string? authHeader = null;
                for (int i = 1; i < lines.Length; i++)
                {
                    int colon = lines[i].IndexOf(':');
                    if (colon > 0)
                    {
                        string name = lines[i][..colon].Trim();
                        string value = lines[i][(colon + 1)..].Trim();
                        if (name.Equals("Content-Length", StringComparison.OrdinalIgnoreCase))
                        {
                            int.TryParse(value, out contentLength);
                        }
                        else if (name.Equals("Authorization", StringComparison.OrdinalIgnoreCase))
                        {
                            authHeader = value;
                        }
                    }
                }

                if (!method.Equals("POST", StringComparison.OrdinalIgnoreCase) || !path.Equals("/rpc", StringComparison.OrdinalIgnoreCase))
                {
                    await WriteResponseAsync(stream, 404, "Not Found", """{"error":"Not found"}""", connCt).ConfigureAwait(false);
                    return;
                }

                if (_expectedAuthBytes is not null)
                {
                    byte[] actualAuthBytes = authHeader is null ? [] : Encoding.UTF8.GetBytes(authHeader);
                    if (!CryptographicOperations.FixedTimeEquals(actualAuthBytes, _expectedAuthBytes))
                    {
                        await WriteResponseAsync(stream, 401, "Unauthorized", """{"error":"Unauthorized"}""", connCt).ConfigureAwait(false);
                        return;
                    }
                }

                if (contentLength <= 0 || contentLength > MaxBodyBytes)
                {
                    await WriteResponseAsync(stream, 400, "Bad Request", """{"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"Invalid Content-Length"}}""", connCt).ConfigureAwait(false);
                    return;
                }

                byte[] bodyBytes = new byte[contentLength];
                int bodyStart = headerEndOffset + delimiterLen;
                int bufferedBodyLen = Math.Min(totalRead - bodyStart, contentLength);
                if (bufferedBodyLen > 0)
                {
                    Buffer.BlockCopy(headerBuf, bodyStart, bodyBytes, 0, bufferedBodyLen);
                }

                if (bufferedBodyLen < contentLength)
                {
                    await stream.ReadExactlyAsync(bodyBytes.AsMemory(bufferedBodyLen, contentLength - bufferedBodyLen), connCt).ConfigureAwait(false);
                }

                string body = Encoding.UTF8.GetString(bodyBytes);
                string responseJson = await DispatchRpcAsync(body).ConfigureAwait(false);
                await WriteResponseAsync(stream, 200, "OK", responseJson, connCt).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
            catch (IOException)
            {
            }
        }
    }

    private static int FindHeaderEnd(ReadOnlySpan<byte> span, out int delimiterLen)
    {
        for (int i = 0; i <= span.Length - 4; i++)
        {
            if (span[i] == '\r' && span[i + 1] == '\n' && span[i + 2] == '\r' && span[i + 3] == '\n')
            {
                delimiterLen = 4;
                return i;
            }
        }

        for (int i = 0; i <= span.Length - 2; i++)
        {
            if (span[i] == '\n' && span[i + 1] == '\n')
            {
                delimiterLen = 2;
                return i;
            }
        }

        delimiterLen = 0;
        return -1;
    }

    private async Task<string> DispatchRpcAsync(string body)
    {
        try
        {
            using JsonDocument doc = JsonDocument.Parse(body);
            JsonElement root = doc.RootElement;
            JsonElement? idEl = root.TryGetProperty("id", out JsonElement idProp) ? idProp.Clone() : null;
            string? rpcMethod = root.TryGetProperty("method", out JsonElement mProp) && mProp.ValueKind == JsonValueKind.String
                ? mProp.GetString()
                : null;
            JsonElement? rpcParams = root.TryGetProperty("params", out JsonElement pProp) ? pProp.Clone() : null;

            if (string.IsNullOrWhiteSpace(rpcMethod))
            {
                return JsonSerializer.Serialize(new
                {
                    jsonrpc = "2.0",
                    id = idEl,
                    error = new { code = -32600, message = "Invalid Request" },
                });
            }

            object? result = await _rpcHandler(rpcMethod, rpcParams).ConfigureAwait(false);
            if (result is RpcMethodNotFound)
            {
                return JsonSerializer.Serialize(new
                {
                    jsonrpc = "2.0",
                    id = idEl,
                    error = new { code = -32601, message = $"Method not found: {rpcMethod}" },
                });
            }

            return JsonSerializer.Serialize(new
            {
                jsonrpc = "2.0",
                id = idEl,
                result,
            });
        }
        catch (JsonException)
        {
            return """{"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"Parse error"}}""";
        }
        catch (Exception ex)
        {
            return JsonSerializer.Serialize(new
            {
                jsonrpc = "2.0",
                id = (object?)null,
                error = new { code = -32603, message = ex.Message },
            });
        }
    }

    private static async Task WriteResponseAsync(NetworkStream stream, int statusCode, string reason, string jsonBody, CancellationToken ct)
    {
        byte[] bodyBytes = Encoding.UTF8.GetBytes(jsonBody);
        string headers =
            $"HTTP/1.1 {statusCode} {reason}\r\n" +
            "Content-Type: application/json; charset=utf-8\r\n" +
            $"Content-Length: {bodyBytes.Length}\r\n" +
            "Connection: close\r\n\r\n";

        byte[] headerBytes = Encoding.ASCII.GetBytes(headers);
        await stream.WriteAsync(headerBytes, ct).ConfigureAwait(false);
        await stream.WriteAsync(bodyBytes, ct).ConfigureAwait(false);
        await stream.FlushAsync(ct).ConfigureAwait(false);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _cts?.Cancel();
        _listener?.Stop();
        _cts?.Dispose();
    }
}

public sealed record RpcMethodNotFound;
