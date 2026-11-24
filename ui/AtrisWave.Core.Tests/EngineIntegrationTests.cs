using System.Diagnostics;
using System.Text.Json;
using AtrisWave.Core.Ipc;
using AtrisWave.Core.Models;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class EngineIntegrationTests
{
    [Fact]
    public async Task HeadlessEngine_LivePipeAndSharedMemoryRoundtrip()
    {
        string? exePath = FindReleaseEngineExecutable();
        if (exePath is null)
        {
            // Skip gracefully when running in a UI-only environment where native engine wasn't built.
            return;
        }

        using var process = Process.Start(new ProcessStartInfo
        {
            FileName = exePath,
            Arguments = "--headless-test",
            WorkingDirectory = Path.GetDirectoryName(exePath)!,
            CreateNoWindow = true,
            UseShellExecute = false,
        });

        Assert.NotNull(process);

        try
        {
            using var pipe = new EnginePipeClient(TelemetryConstants.PipeName, TimeSpan.FromSeconds(2));

            // 1. Wait for pipe server startup & PingAsync
            EnginePipeResponse? pingResp = null;
            for (int i = 0; i < 30; i++)
            {
                pingResp = await pipe.PingAsync();
                if (pingResp.Ok)
                {
                    break;
                }

                await Task.Delay(100);
            }

            Assert.NotNull(pingResp);
            Assert.True(pingResp.Ok, $"PingAsync failed: {pingResp.Error}");

            // 2. GetStatusAsync (unwrap "result" envelope)
            EnginePipeResponse statusResp = await pipe.GetStatusAsync();
            Assert.True(statusResp.Ok, $"GetStatusAsync failed: {statusResp.Error}");
            Assert.NotNull(statusResp.Payload);
            JsonElement statusRoot = statusResp.Payload.Value;
            JsonElement statusResult = statusRoot.TryGetProperty("result", out JsonElement innerStatus) ? innerStatus : statusRoot;
            Assert.Equal(48000u, statusResult.GetProperty("sample_rate").GetUInt32());
            Assert.Equal(6u, statusResult.GetProperty("channel_count").GetUInt32());

            // 3. ListDevicesAsync
            IReadOnlyList<AudioDeviceInfo> devices = await pipe.ListDevicesAsync();
            Assert.NotNull(devices);

            // 4. SetChannelAsync
            EnginePipeResponse setChResp = await pipe.SetChannelAsync(1, 0.75f, false, false);
            Assert.True(setChResp.Ok, $"SetChannelAsync failed: {setChResp.Error}");

            // 5. SetMicDspAsync
            var dsp = new MicDspSettings
            {
                DirectMonitor = true,
                DenoiseEnabled = true,
                EqMid1Db = 3.0f,
                DuckingEnabled = true,
                DuckingDepthDb = -10.0f,
            };
            EnginePipeResponse setDspResp = await pipe.SetMicDspAsync(dsp);
            Assert.True(setDspResp.Ok, $"SetMicDspAsync failed: {setDspResp.Error}");

            // 5b. SavePresetAsync -> ListPresetsAsync -> LoadPresetAsync (proves live result unwrapping)
            const string testPresetName = "IntegrationTestPreset";
            EnginePipeResponse savePresetResp = await pipe.SavePresetAsync(testPresetName);
            Assert.True(savePresetResp.Ok, $"SavePresetAsync failed: {savePresetResp.Error}");

            IReadOnlyList<string> presets = await pipe.ListPresetsAsync();
            Assert.Contains(testPresetName, presets);

            EnginePipeResponse loadPresetResp = await pipe.LoadPresetAsync(testPresetName);
            Assert.True(loadPresetResp.Ok, $"LoadPresetAsync failed: {loadPresetResp.Error}");

            // 6. SharedMemoryTelemetryReader: verify snapshot & advancing seqlock sequence
            using var shm = new SharedMemoryTelemetryReader(TelemetryConstants.SharedMemoryName);
            TelemetrySnapshot? snap1 = null;
            for (int i = 0; i < 20; i++)
            {
                if (shm.TryRead(out snap1) && snap1 is not null)
                {
                    break;
                }

                await Task.Delay(25);
            }

            Assert.NotNull(snap1);
            Assert.Equal(TelemetryConstants.Magic, snap1.Header.Magic);
            Assert.Equal(TelemetryConstants.ProtocolVersion, snap1.Header.Version);
            Assert.Equal(6u, snap1.Header.ChannelCount);
            Assert.Equal(48000u, snap1.Header.SampleRate);

            TelemetrySnapshot? snap2 = null;
            for (int i = 0; i < 30; i++)
            {
                await Task.Delay(25);
                if (shm.TryRead(out snap2) && snap2 is not null && snap2.Header.Sequence > snap1.Header.Sequence)
                {
                    break;
                }
            }

            Assert.NotNull(snap2);
            Assert.True(snap2.Header.Sequence > snap1.Header.Sequence,
                $"Expected advancing sequence, got {snap1.Header.Sequence} -> {snap2.Header.Sequence}");

            // 7. ShutdownAsync & clean exit
            EnginePipeResponse shutdownResp = await pipe.ShutdownAsync();
            Assert.True(shutdownResp.Ok, $"ShutdownAsync failed: {shutdownResp.Error}");

            bool exited = process.WaitForExit(3000);
            Assert.True(exited, "AtrisWaveEngine.exe did not exit within 3 seconds after shutdown command.");
            Assert.Equal(0, process.ExitCode);
        }
        finally
        {
            try
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                }
            }
            catch
            {
            }
        }
    }

    private static string? FindReleaseEngineExecutable()
    {
        DirectoryInfo? dir = new(AppContext.BaseDirectory);
        while (dir is not null)
        {
            string candidate = Path.Combine(dir.FullName, "engine", "build", "msvc-release", "AtrisWaveEngine.exe");
            if (File.Exists(candidate))
            {
                return candidate;
            }

            dir = dir.Parent;
        }

        return null;
    }
}
