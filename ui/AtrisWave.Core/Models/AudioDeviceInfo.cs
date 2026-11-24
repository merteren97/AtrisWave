using System.Text.Json.Serialization;

namespace AtrisWave.Core.Models;

/// <summary>
/// Audio endpoint metadata returned by the engine's <c>list_devices</c> command.
/// </summary>
public sealed class AudioDeviceInfo
{
    [JsonPropertyName("id")] public string Id { get; set; } = string.Empty;
    [JsonPropertyName("name")] public string Name { get; set; } = string.Empty;
    [JsonPropertyName("is_capture")] public bool IsCapture { get; set; }
    [JsonPropertyName("is_default")] public bool IsDefault { get; set; }
    [JsonPropertyName("is_virtual")] public bool IsVirtual { get; set; }

    public bool IsInput => IsCapture;

    public override string ToString() => IsVirtual ? $"[Virtual] {Name}" : Name;
}
