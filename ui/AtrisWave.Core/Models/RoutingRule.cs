using System.Text.Json.Serialization;

namespace AtrisWave.Core.Models;

/// <summary>
/// Persistent per-application routing rule stored by the engine.
/// </summary>
public sealed class RoutingRule
{
    [JsonPropertyName("process_name")]
    public string ProcessName { get; set; } = string.Empty;

    [JsonPropertyName("channel_id")]
    public uint ChannelId { get; set; } = (uint)MixerChannelId.Game;

    [JsonPropertyName("target_device_id")]
    public string TargetDeviceId { get; set; } = string.Empty;

    [JsonPropertyName("loopback_capture")]
    public bool LoopbackCapture { get; set; }

    [JsonIgnore]
    public string ChannelNameKey => (MixerChannelId)ChannelId switch
    {
        MixerChannelId.Master => "Channel_Master",
        MixerChannelId.Game => "Channel_Game",
        MixerChannelId.Chat => "Channel_Chat",
        MixerChannelId.Media => "Channel_Media",
        MixerChannelId.Aux => "Channel_Aux",
        MixerChannelId.Microphone => "Channel_Microphone",
        _ => "Channel_Game",
    };
}
