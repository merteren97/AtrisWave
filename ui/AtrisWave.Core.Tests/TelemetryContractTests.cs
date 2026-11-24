using System.Runtime.InteropServices;
using AtrisWave.Core.Ipc;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class TelemetryContractTests
{
    [Fact]
    public void Constants_MatchCAbiSpecification()
    {
        Assert.Equal("AtrisWaveEngine", TelemetryConstants.PipeName);
        Assert.Equal(@"Local\AtrisWaveTelemetryV1", TelemetryConstants.SharedMemoryName);
        Assert.Equal(0x4157544Du, TelemetryConstants.Magic);
        Assert.Equal(1u, TelemetryConstants.ProtocolVersion);
        Assert.Equal(16, TelemetryConstants.MaxChannels);
        Assert.Equal(64, TelemetryConstants.HeaderSizeBytes);
        Assert.Equal(32, TelemetryConstants.ChannelFrameSizeBytes);
        Assert.Equal(576, TelemetryConstants.TotalSnapshotSizeBytes);
    }

    [Fact]
    public void ChannelMeterFrame_SizeAndFieldOffsets_MatchCAbi()
    {
        Assert.Equal(32, Marshal.SizeOf<ChannelMeterFrame>());
        Assert.Equal((nint)0, Marshal.OffsetOf<ChannelMeterFrame>(nameof(ChannelMeterFrame.ChannelId)));
        Assert.Equal((nint)4, Marshal.OffsetOf<ChannelMeterFrame>(nameof(ChannelMeterFrame.PeakLeft)));
        Assert.Equal((nint)8, Marshal.OffsetOf<ChannelMeterFrame>(nameof(ChannelMeterFrame.PeakRight)));
        Assert.Equal((nint)12, Marshal.OffsetOf<ChannelMeterFrame>(nameof(ChannelMeterFrame.RmsLeft)));
        Assert.Equal((nint)16, Marshal.OffsetOf<ChannelMeterFrame>(nameof(ChannelMeterFrame.RmsRight)));
        Assert.Equal((nint)20, Marshal.OffsetOf<ChannelMeterFrame>(nameof(ChannelMeterFrame.GainReductionDb)));
        Assert.Equal((nint)24, Marshal.OffsetOf<ChannelMeterFrame>(nameof(ChannelMeterFrame.Flags)));
        Assert.Equal((nint)28, Marshal.OffsetOf<ChannelMeterFrame>(nameof(ChannelMeterFrame.Reserved)));
    }

    [Fact]
    public void TelemetryHeader_SizeAndFieldOffsets_MatchCAbi()
    {
        Assert.Equal(64, Marshal.SizeOf<TelemetryHeader>());
        Assert.Equal((nint)0, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.Magic)));
        Assert.Equal((nint)4, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.Version)));
        Assert.Equal((nint)8, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.ChannelCount)));
        Assert.Equal((nint)12, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.SampleRate)));
        Assert.Equal((nint)16, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.BufferFrames)));
        Assert.Equal((nint)20, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.EngineState)));
        Assert.Equal((nint)24, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.Sequence)));
        Assert.Equal((nint)32, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.TimestampQpc)));
        Assert.Equal((nint)40, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.CpuLoadPercent)));
        Assert.Equal((nint)44, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.XrunCount)));
        Assert.Equal((nint)48, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.Reserved0)));
        Assert.Equal((nint)52, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.Reserved1)));
        Assert.Equal((nint)56, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.Reserved2)));
        Assert.Equal((nint)60, Marshal.OffsetOf<TelemetryHeader>(nameof(TelemetryHeader.Reserved3)));
    }
}
