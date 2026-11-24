using System.Runtime.InteropServices;

namespace AtrisWave.Core.Ipc;

/// <summary>
/// Binary C-ABI constants matching <c>shared/include/atriswave/ipc_protocol.h</c> (Telemetry V2).
/// </summary>
public static class TelemetryConstants
{
    public const string PipeName = "AtrisWaveEngine";
    public const string SharedMemoryName = @"Local\AtrisWaveTelemetryV2";
    public const uint Magic = 0x4157544Du; // "AWTM"
    public const uint ProtocolVersion = 2u;
    public const int MaxChannels = 16;
    public const int MaxBuses = 4;
    public const int HeaderSizeBytes = 64;
    public const int ChannelFrameSizeBytes = 32;
    public const int TotalSnapshotSizeBytes =
        HeaderSizeBytes + (MaxChannels * ChannelFrameSizeBytes) + (MaxBuses * ChannelFrameSizeBytes); // 704 bytes
}

/// <summary>
/// Audio engine lifecycle state reported in <see cref="TelemetryHeader.EngineState"/>.
/// </summary>
public enum EngineState : uint
{
    Stopped = 0,
    Running = 1,
    Degraded = 2,
}

/// <summary>
/// Per-channel status bitflags reported in <see cref="ChannelMeterFrame.Flags"/>.
/// </summary>
[Flags]
public enum ChannelMeterFlags : uint
{
    None = 0,
    Muted = 1u << 0,
    Clipping = 1u << 1,
    Active = 1u << 2,
}

/// <summary>
/// Per-channel / per-bus telemetry meter frame (32 bytes, C-ABI Pack=4).
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct ChannelMeterFrame
{
    public uint ChannelId;
    public float PeakLeft;
    public float PeakRight;
    public float RmsLeft;
    public float RmsRight;
    public float GainReductionDb;
    public uint Flags;
    public uint Reserved;

    public readonly bool IsMuted => ((ChannelMeterFlags)Flags & ChannelMeterFlags.Muted) != 0;
    public readonly bool IsClipping => ((ChannelMeterFlags)Flags & ChannelMeterFlags.Clipping) != 0;
    public readonly bool IsActive => ((ChannelMeterFlags)Flags & ChannelMeterFlags.Active) != 0;
}

/// <summary>
/// Shared-memory telemetry header (64 bytes, C-ABI Pack=4, V2 with BusCount at offset 48).
/// Uses <see cref="Sequence"/> as a seqlock counter: odd = write in progress, even = stable.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct TelemetryHeader
{
    public uint Magic;
    public uint Version;
    public uint ChannelCount;
    public uint SampleRate;
    public uint BufferFrames;
    public uint EngineState;
    public ulong Sequence;
    public ulong TimestampQpc;
    public float CpuLoadPercent;
    public uint XrunCount;
    public uint BusCount;
    public uint Reserved1;
    public uint Reserved2;
    public uint Reserved3;
}

/// <summary>
/// Consistent, seqlock-verified Telemetry V2 snapshot read from shared memory (16 channels + 4 buses).
/// </summary>
public sealed class TelemetrySnapshot
{
    public TelemetryHeader Header { get; }
    public ChannelMeterFrame[] Channels { get; }
    public ChannelMeterFrame[] Buses { get; }

    public TelemetrySnapshot(TelemetryHeader header, ChannelMeterFrame[] channels, ChannelMeterFrame[]? buses = null)
    {
        Header = header;
        Channels = channels;
        Buses = buses ?? new ChannelMeterFrame[TelemetryConstants.MaxBuses];
    }

    public EngineState State => Enum.IsDefined(typeof(EngineState), Header.EngineState)
        ? (EngineState)Header.EngineState
        : EngineState.Stopped;
}
