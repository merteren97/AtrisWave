using System.IO.Pipes;
using System.Text;
using AtrisWave.Core.Ipc;
using AtrisWave.Core.Models;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class EnginePipeClientTests
{
    [Fact]
    public async Task ListDevicesResultAsync_ReturnsError_WhenPipeOffline()
    {
        string pipeName = $"AtrisWaveMissingPipe_{Guid.NewGuid():N}";
        using var client = new EnginePipeClient(pipeName, TimeSpan.FromMilliseconds(150));

        PipeResult<IReadOnlyList<AudioDeviceInfo>> res = await client.ListDevicesResultAsync();

        Assert.False(res.Ok);
        Assert.Empty(res.Data);
        Assert.False(string.IsNullOrWhiteSpace(res.Error));
        Assert.Equal(res.Error, client.LastError);
    }

    [Fact]
    public async Task PingAsync_WithShortTimeout_FailsFast()
    {
        string pipeName = $"AtrisWaveMissingPipe_{Guid.NewGuid():N}";
        using var client = new EnginePipeClient(pipeName, TimeSpan.FromSeconds(5));

        var sw = System.Diagnostics.Stopwatch.StartNew();
        EnginePipeResponse resp = await client.PingAsync(120);
        sw.Stop();

        Assert.False(resp.Ok);
        Assert.True(sw.ElapsedMilliseconds < 1500, $"Expected fast timeout, took {sw.ElapsedMilliseconds}ms");
    }

    [Fact]
    public async Task ListDevicesResultAsync_ParsesDevicesAndClearsLastError()
    {
        string pipeName = $"AtrisWaveMockPipe_{Guid.NewGuid():N}";
        using var server = new NamedPipeServerStream(pipeName, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);

        Task serverTask = Task.Run(async () =>
        {
            await server.WaitForConnectionAsync();
            using var reader = new StreamReader(server, Encoding.UTF8, leaveOpen: true);
            _ = await reader.ReadLineAsync();
            const string reply = "{\"id\":1,\"ok\":true,\"result\":{\"devices\":[{\"id\":\"mic-1\",\"name\":\"Studio Mic\",\"is_capture\":true,\"is_default\":true,\"is_virtual\":false}]}}\n";
            byte[] bytes = Encoding.UTF8.GetBytes(reply);
            await server.WriteAsync(bytes);
            await server.FlushAsync();
        });

        using var client = new EnginePipeClient(pipeName, TimeSpan.FromSeconds(2));
        PipeResult<IReadOnlyList<AudioDeviceInfo>> res = await client.ListDevicesResultAsync();
        await serverTask;

        Assert.True(res.Ok);
        Assert.Null(res.Error);
        Assert.Null(client.LastError);
        Assert.Single(res.Data);
        Assert.Equal("Studio Mic", res.Data[0].Name);
        Assert.True(res.Data[0].IsInput);
    }

    [Fact]
    public async Task ListDevicesResultAsync_ReturnsError_OnMalformedJson()
    {
        string pipeName = $"AtrisWaveBadJsonPipe_{Guid.NewGuid():N}";
        using var server = new NamedPipeServerStream(pipeName, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);

        Task serverTask = Task.Run(async () =>
        {
            await server.WaitForConnectionAsync();
            using var reader = new StreamReader(server, Encoding.UTF8, leaveOpen: true);
            _ = await reader.ReadLineAsync();
            byte[] bytes = Encoding.UTF8.GetBytes("{not-valid-json\n");
            await server.WriteAsync(bytes);
            await server.FlushAsync();
        });

        using var client = new EnginePipeClient(pipeName, TimeSpan.FromSeconds(2));
        PipeResult<IReadOnlyList<AudioDeviceInfo>> res = await client.ListDevicesResultAsync();
        await serverTask;

        Assert.False(res.Ok);
        Assert.Empty(res.Data);
        Assert.False(string.IsNullOrWhiteSpace(res.Error));
    }
}
