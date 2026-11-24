using System.Runtime.InteropServices;

namespace AtrisWave.Core.Ipc;

/// <summary>
/// Binary C-ABI constants matching <c>shared/include/atriswave/ipc_protocol.h</c> (Telemetry V3).
/// </summary>
public static class TelemetryConstants
{
    public const string PipeName = "AtrisWaveEngine";
    public const string SharedMemoryName = @"Local\AtrisWaveTelemetryV3";
    public const uint Magic = 0x4157544Du; // "AWTM"
    public const uint ProtocolVersion = 3u;
    public const int MaxChannels = 16;
    public const int MaxBuses = 4;
    public const int SpectrumBins = 32;
    public const int HeaderSizeBytes = 64;
    public const int ChannelFrameSizeBytes = 32;
    public const int SpectrumBytes = SpectrumBins * sizeof(float); // 128 bytes
    public const int TotalSnapshotSizeBytes =
        HeaderSizeBytes + (MaxChannels * ChannelFrameSizeBytes) + (MaxBuses * ChannelFrameSizeBytes) + SpectrumBytes; // 832 bytes
}

public enum EngineState : uint
{
    Stopped = 0,
    Running = 1,
    Degraded = 2,
}

[Flags]
public enum ChannelMeterFlags : uint
{
    None = 0,
    Muted = 1u << 0,
    Clipping = 1u << 1,
    Active = 1u << 2,
}

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
/// Shared-memory telemetry header (64 bytes, C-ABI Pack=4, V3 with BusCount @ 48 and SpectrumBins @ 52).
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
    public uint SpectrumBins;
    public uint Reserved2;
    public uint Reserved3;
}

/// <summary>
/// Consistent, seqlock-verified Telemetry V3 snapshot (16 channels + 4 buses + 32 FFT spectrum bins).
/// </summary>
public sealed class TelemetrySnapshot
{
    public TelemetryHeader Header { get; }
    public ChannelMeterFrame[] Channels { get; }
    public ChannelMeterFrame[] Buses { get; }
    public float[] SpectrumDb { get; }

    public TelemetrySnapshot(
        TelemetryHeader header,
        ChannelMeterFrame[] channels,
        ChannelMeterFrame[]? buses = null,
        float[]? spectrumDb = null)
    {
        Header = header;
        Channels = channels;
        Buses = buses ?? new ChannelMeterFrame[TelemetryConstants.MaxBuses];
        SpectrumDb = spectrumDb ?? new float[TelemetryConstants.SpectrumBins];
    }

    public EngineState State => Enum.IsDefined(typeof(EngineState), Header.EngineState)
        ? (EngineState)Header.EngineState
        : EngineState.Stopped;
}
