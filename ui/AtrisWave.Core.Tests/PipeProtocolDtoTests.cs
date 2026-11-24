using System.Text.Json;
using AtrisWave.Core.Ipc;
using AtrisWave.Core.Models;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class PipeProtocolDtoTests
{
    [Fact]
    public void MicDspWireDto_SerializesWithExactSnakeCaseKeys()
    {
        var settings = new MicDspSettings
        {
            DirectMonitor = true,
            MonitorGain = 0.85f,
            DenoiseEnabled = true,
            DenoiseStrength = 0.75f,
            DuckingEnabled = true,
            DuckingDepthDb = -10.0f,
            LimiterCeilingDb = -0.5f,
        };

        string json = JsonSerializer.Serialize(settings.ToWireDto());
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement root = doc.RootElement;

        Assert.True(root.GetProperty("direct_monitor").GetBoolean());
        Assert.Equal(0.85f, root.GetProperty("monitor_gain").GetSingle(), precision: 4);
        Assert.True(root.GetProperty("denoise_enabled").GetBoolean());
        Assert.Equal(0.75f, root.GetProperty("denoise_strength").GetSingle(), precision: 4);
        Assert.True(root.GetProperty("ducking_enabled").GetBoolean());
        Assert.Equal(-10.0f, root.GetProperty("ducking_depth_db").GetSingle(), precision: 4);
        Assert.Equal(-0.5f, root.GetProperty("limiter_ceiling_db").GetSingle(), precision: 4);
    }

    [Fact]
    public void CommandRequestDtos_SerializeWithExpectedWireFields()
    {
        string chJson = JsonSerializer.Serialize(new SetChannelRequest(5, 0.9f, true, false));
        using (JsonDocument doc = JsonDocument.Parse(chJson))
        {
            Assert.Equal(5u, doc.RootElement.GetProperty("channel_id").GetUInt32());
            Assert.Equal(0.9f, doc.RootElement.GetProperty("volume").GetSingle(), precision: 4);
            Assert.True(doc.RootElement.GetProperty("muted").GetBoolean());
            Assert.False(doc.RootElement.GetProperty("solo").GetBoolean());
        }

        string routeJson = JsonSerializer.Serialize(new RouteAppRequest(1234, "dev-game", true));
        using (JsonDocument doc = JsonDocument.Parse(routeJson))
        {
            Assert.Equal(1234u, doc.RootElement.GetProperty("pid").GetUInt32());
            Assert.Equal("dev-game", doc.RootElement.GetProperty("target_device_id").GetString());
            Assert.True(doc.RootElement.GetProperty("loopback_capture").GetBoolean());
        }
    }

    [Fact]
    public void DeviceAndSessionDtos_DeserializeFromEngineEnvelope()
    {
        const string envelopeJson = """
            {
              "id": 1,
              "ok": true,
              "result": {
                "devices": [
                  {"id":"mic-1","name":"USB Mic","is_capture":true,"is_default":true,"is_virtual":false}
                ],
                "sessions": [
                  {"pid":4242,"process_name":"game.exe","display_name":"Game","volume":0.8,"muted":true,"target_device_id":"dev-1","loopback_capture":true}
                ],
                "presets": ["Broadcast Warm"]
              }
            }
            """;

        using JsonDocument doc = JsonDocument.Parse(envelopeJson);
        JsonElement result = doc.RootElement.GetProperty("result");

        List<AudioDeviceInfo>? devices = JsonSerializer.Deserialize<List<AudioDeviceInfo>>(result.GetProperty("devices").GetRawText());
        Assert.NotNull(devices);
        Assert.Single(devices);
        Assert.True(devices[0].IsCapture);
        Assert.True(devices[0].IsInput);
        Assert.True(devices[0].IsDefault);

        List<AudioSessionInfo>? sessions = JsonSerializer.Deserialize<List<AudioSessionInfo>>(result.GetProperty("sessions").GetRawText());
        Assert.NotNull(sessions);
        Assert.Single(sessions);
        Assert.Equal(4242u, sessions[0].Pid);
        Assert.Equal("game.exe", sessions[0].AppName);
        Assert.True(sessions[0].IsMuted);
        Assert.Equal("dev-1", sessions[0].TargetDeviceId);
        Assert.True(sessions[0].LoopbackCapture);

        List<string>? presets = JsonSerializer.Deserialize<List<string>>(result.GetProperty("presets").GetRawText());
        Assert.NotNull(presets);
        Assert.Contains("Broadcast Warm", presets);
    }
}
