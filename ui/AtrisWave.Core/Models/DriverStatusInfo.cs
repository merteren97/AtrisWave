using System.Text.Json.Serialization;

namespace AtrisWave.Core.Models;

/// <summary>
/// Virtual audio driver status returned by the engine's <c>get_driver_status</c> command.
/// </summary>
public sealed class DriverStatusInfo
{
    [JsonPropertyName("driver_installed")]
    public bool DriverInstalled { get; set; }

    [JsonPropertyName("driver_version")]
    public uint DriverVersion { get; set; }

    [JsonPropertyName("endpoint_count")]
    public uint EndpointCount { get; set; } = 6;

    [JsonPropertyName("fallback_mode")]
    public string FallbackMode { get; set; } = "wasapi_vbcable";
}
