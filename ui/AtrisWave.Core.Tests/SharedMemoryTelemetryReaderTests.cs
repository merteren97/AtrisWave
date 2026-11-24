using System.IO.MemoryMappedFiles;
using AtrisWave.Core.Ipc;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class SharedMemoryTelemetryReaderTests
{
    [Fact]
    public void TryRead_ReturnsFalse_WhenMappingDoesNotExist()
    {
        using var reader = new SharedMemoryTelemetryReader($"Local\\AtrisWaveMissing_{Guid.NewGuid():N}");
        Assert.False(reader.TryRead(out TelemetrySnapshot? snapshot));
        Assert.Null(snapshot);
    }

    [Fact]
    public void TryRead_ReturnsValidSnapshot_WhenSequenceIsEvenAndMagicMatches()
    {
        string mapName = $"Local\\AtrisWaveTest_{Guid.NewGuid():N}";
        using var mmf = MemoryMappedFile.CreateNew(mapName, TelemetryConstants.TotalSnapshotSizeBytes);
        using var writer = mmf.CreateViewAccessor(0, TelemetryConstants.TotalSnapshotSizeBytes, MemoryMappedFileAccess.Write);

        var header = new TelemetryHeader
        {
            Magic = TelemetryConstants.Magic,
            Version = TelemetryConstants.ProtocolVersion,
            ChannelCount = 6,
            SampleRate = 48000,
            BufferFrames = 256,
            EngineState = (uint)EngineState.Running,
            Sequence = 42,
            TimestampQpc = 123456789UL,
            CpuLoadPercent = 3.5f,
            XrunCount = 0,
        };
        writer.Write(0, ref header);

        var micFrame = new ChannelMeterFrame
        {
            ChannelId = 5,
            PeakLeft = 0.85f,
            PeakRight = 0.82f,
            RmsLeft = 0.45f,
            RmsRight = 0.44f,
            GainReductionDb = -2.5f,
            Flags = (uint)(ChannelMeterFlags.Active | ChannelMeterFlags.Clipping),
        };
        int micOffset = TelemetryConstants.HeaderSizeBytes + (5 * TelemetryConstants.ChannelFrameSizeBytes);
        writer.Write(micOffset, ref micFrame);

        using var reader = new SharedMemoryTelemetryReader(mapName);
        Assert.True(reader.TryRead(out TelemetrySnapshot? snapshot));
        Assert.NotNull(snapshot);
        Assert.Equal(EngineState.Running, snapshot.State);
        Assert.Equal(48000u, snapshot.Header.SampleRate);
        Assert.Equal(256u, snapshot.Header.BufferFrames);
        Assert.Equal(42UL, snapshot.Header.Sequence);
        Assert.Equal(0.85f, snapshot.Channels[5].PeakLeft, precision: 4);
        Assert.True(snapshot.Channels[5].IsActive);
        Assert.True(snapshot.Channels[5].IsClipping);
        Assert.False(snapshot.Channels[5].IsMuted);
    }

    [Fact]
    public void TryRead_RejectsTornSnapshot_WhenSequenceIsOdd()
    {
        string mapName = $"Local\\AtrisWaveTest_{Guid.NewGuid():N}";
        using var mmf = MemoryMappedFile.CreateNew(mapName, TelemetryConstants.TotalSnapshotSizeBytes);
        using var writer = mmf.CreateViewAccessor(0, TelemetryConstants.TotalSnapshotSizeBytes, MemoryMappedFileAccess.Write);

        var header = new TelemetryHeader
        {
            Magic = TelemetryConstants.Magic,
            Version = TelemetryConstants.ProtocolVersion,
            ChannelCount = 6,
            SampleRate = 48000,
            BufferFrames = 256,
            EngineState = (uint)EngineState.Running,
            Sequence = 17, // Odd = write in progress
        };
        writer.Write(0, ref header);

        using var reader = new SharedMemoryTelemetryReader(mapName);
        Assert.False(reader.TryRead(out TelemetrySnapshot? snapshot));
        Assert.Null(snapshot);
    }

    [Fact]
    public void TryRead_RejectsInvalidMagic()
    {
        string mapName = $"Local\\AtrisWaveTest_{Guid.NewGuid():N}";
        using var mmf = MemoryMappedFile.CreateNew(mapName, TelemetryConstants.TotalSnapshotSizeBytes);
        using var writer = mmf.CreateViewAccessor(0, TelemetryConstants.TotalSnapshotSizeBytes, MemoryMappedFileAccess.Write);

        var header = new TelemetryHeader
        {
            Magic = 0xDEADBEEFu,
            Version = TelemetryConstants.ProtocolVersion,
            Sequence = 2,
        };
        writer.Write(0, ref header);

        using var reader = new SharedMemoryTelemetryReader(mapName);
        Assert.False(reader.TryRead(out TelemetrySnapshot? snapshot));
        Assert.Null(snapshot);
    }
}
