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

        const string testPresetName = "IntegrationTestPreset";
        const string testRuleProcess = "TestGame.exe";
        string tempWavPath = Path.Combine(Path.GetTempPath(), $"atriswave_test_{Guid.NewGuid():N}.wav");
        using var pipe = new EnginePipeClient(TelemetryConstants.PipeName, TimeSpan.FromSeconds(2));

        try
        {
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
            Assert.True(statusResult.GetProperty("channel_count").GetUInt32() >= 2u);

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
            EnginePipeResponse savePresetResp = await pipe.SavePresetAsync(testPresetName);
            Assert.True(savePresetResp.Ok, $"SavePresetAsync failed: {savePresetResp.Error}");

            IReadOnlyList<string> presets = await pipe.ListPresetsAsync();
            Assert.Contains(testPresetName, presets);

            EnginePipeResponse loadPresetResp = await pipe.LoadPresetAsync(testPresetName);
            Assert.True(loadPresetResp.Ok, $"LoadPresetAsync failed: {loadPresetResp.Error}");

            // 5c. Phase 2 Driver Status & Persistent Routing Rules roundtrip
            DriverStatusInfo driverStatus = await pipe.GetDriverStatusAsync();
            Assert.Equal(16u, driverStatus.EndpointCount);
            Assert.False(string.IsNullOrWhiteSpace(driverStatus.FallbackMode));

            var testRule = new RoutingRule
            {
                ProcessName = testRuleProcess,
                ChannelId = (uint)MixerChannelId.Game,
                TargetDeviceId = "test-device-id",
                LoopbackCapture = false,
            };
            EnginePipeResponse setRuleResp = await pipe.SetRoutingRuleAsync(testRule);
            Assert.True(setRuleResp.Ok, $"SetRoutingRuleAsync failed: {setRuleResp.Error}");

            IReadOnlyList<RoutingRule> rules = await pipe.ListRoutingRulesAsync();
            Assert.Contains(rules, r => string.Equals(r.ProcessName, testRuleProcess, StringComparison.OrdinalIgnoreCase));

            EnginePipeResponse delRuleResp = await pipe.DeleteRoutingRuleAsync(testRuleProcess);
            Assert.True(delRuleResp.Ok, $"DeleteRoutingRuleAsync failed: {delRuleResp.Error}");

            // 6. SharedMemoryTelemetryReader V2: verify 704B snapshot & advancing seqlock sequence
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
            Assert.Equal(3u, snap1.Header.Version);
            Assert.True(snap1.Header.ChannelCount >= 2u);
            Assert.Equal(4u, snap1.Header.BusCount);
            Assert.Equal(32u, snap1.Header.SpectrumBins);
            Assert.Equal(48000u, snap1.Header.SampleRate);

            // 6b. v2.1 Spectrum Source & Soundboard roundtrip
            EnginePipeResponse specResp = await pipe.SetSpectrumSourceAsync(0);
            Assert.True(specResp.Ok, $"SetSpectrumSourceAsync failed: {specResp.Error}");

            WriteMinimalStereoWav(tempWavPath);
            EnginePipeResponse playSbResp = await pipe.PlaySoundboardAsync(tempWavPath, 0.8f, true, false, true, false);
            Assert.True(playSbResp.Ok, $"PlaySoundboardAsync failed: {playSbResp.Error}");

            EnginePipeResponse stopSbResp = await pipe.StopSoundboardAsync(-1);
            Assert.True(stopSbResp.Ok, $"StopSoundboardAsync failed: {stopSbResp.Error}");

            // 6b. v2 Dynamic Channels & Output Buses roundtrip
            IReadOnlyList<DynamicChannelModel> channels = await pipe.ListChannelsAsync();
            Assert.NotEmpty(channels);

            IReadOnlyList<OutputBusModel> buses = await pipe.ListOutputBusesAsync();
            Assert.Equal(4, buses.Count);

            (bool createdOk, uint createdId) = await pipe.CreateChannelAsync("Discord", "app_group", "#5865F2");
            Assert.True(createdOk);
            Assert.True(createdId > 0);

            EnginePipeResponse bindAppResp = await pipe.BindAppToChannelAsync("Discord.exe", createdId);
            Assert.True(bindAppResp.Ok, $"BindAppToChannelAsync failed: {bindAppResp.Error}");

            EnginePipeResponse sendResp = await pipe.SetChannelSendAsync(createdId, "A1", true, 1.0f);
            Assert.True(sendResp.Ok, $"SetChannelSendAsync failed: {sendResp.Error}");

            EnginePipeResponse renameResp = await pipe.RenameChannelAsync(createdId, "VoiceChat");
            Assert.True(renameResp.Ok, $"RenameChannelAsync failed: {renameResp.Error}");

            EnginePipeResponse delChResp = await pipe.DeleteChannelAsync(createdId);
            Assert.True(delChResp.Ok, $"DeleteChannelAsync failed: {delChResp.Error}");

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
                    await pipe.DeleteRoutingRuleAsync(testRuleProcess);
                    process.Kill(entireProcessTree: true);
                }
            }
            catch
            {
            }

            try
            {
                if (File.Exists(tempWavPath))
                {
                    File.Delete(tempWavPath);
                }

                string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                string presetPath = Path.Combine(localAppData, "AtrisWave", "presets", $"{testPresetName}.json");
                if (File.Exists(presetPath))
                {
                    File.Delete(presetPath);
                }
            }
            catch
            {
            }
        }
    }

    private static void WriteMinimalStereoWav(string path)
    {
        using var fs = File.Create(path);
        using var bw = new BinaryWriter(fs);
        const int numSamples = 480;
        const short channels = 2;
        const int sampleRate = 48000;
        const short bitsPerSample = 16;
        int dataSize = numSamples * channels * (bitsPerSample / 8);

        bw.Write("RIFF"u8);
        bw.Write(36 + dataSize);
        bw.Write("WAVE"u8);
        bw.Write("fmt "u8);
        bw.Write(16);
        bw.Write((short)1);
        bw.Write(channels);
        bw.Write(sampleRate);
        bw.Write(sampleRate * channels * (bitsPerSample / 8));
        bw.Write((short)(channels * (bitsPerSample / 8)));
        bw.Write(bitsPerSample);
        bw.Write("data"u8);
        bw.Write(dataSize);
        for (int i = 0; i < numSamples * channels; i++)
        {
            bw.Write((short)0);
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
