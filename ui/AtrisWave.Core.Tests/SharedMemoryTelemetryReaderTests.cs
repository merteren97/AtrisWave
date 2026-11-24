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
    public void TryRead_ReturnsValidV2Snapshot_WithChannelsAndBuses()
    {
        string mapName = $"Local\\AtrisWaveTest_{Guid.NewGuid():N}";
        using var mmf = MemoryMappedFile.CreateNew(mapName, TelemetryConstants.TotalSnapshotSizeBytes);
        using var writer = mmf.CreateViewAccessor(0, TelemetryConstants.TotalSnapshotSizeBytes, MemoryMappedFileAccess.Write);

        var header = new TelemetryHeader
        {
            Magic = TelemetryConstants.Magic,
            Version = TelemetryConstants.ProtocolVersion,
            ChannelCount = 6,
            BusCount = 4,
            SpectrumBins = 32,
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
            ChannelId = 1,
            PeakLeft = 0.85f,
            PeakRight = 0.82f,
            RmsLeft = 0.45f,
            RmsRight = 0.44f,
            GainReductionDb = -2.5f,
            Flags = (uint)(ChannelMeterFlags.Active | ChannelMeterFlags.Clipping),
        };
        int chOffset = TelemetryConstants.HeaderSizeBytes + (1 * TelemetryConstants.ChannelFrameSizeBytes);
        writer.Write(chOffset, ref micFrame);

        var busA1Frame = new ChannelMeterFrame
        {
            ChannelId = 0,
            PeakLeft = 0.72f,
            PeakRight = 0.70f,
            RmsLeft = 0.35f,
            RmsRight = 0.33f,
            Flags = (uint)ChannelMeterFlags.Active,
        };
        int busOffset = TelemetryConstants.HeaderSizeBytes + (16 * TelemetryConstants.ChannelFrameSizeBytes);
        writer.Write(busOffset, ref busA1Frame);

        int spectrumOffset = TelemetryConstants.HeaderSizeBytes + (20 * TelemetryConstants.ChannelFrameSizeBytes);
        writer.Write(spectrumOffset + (10 * sizeof(float)), -18.5f);

        using var reader = new SharedMemoryTelemetryReader(mapName);
        Assert.True(reader.TryRead(out TelemetrySnapshot? snapshot));
        Assert.NotNull(snapshot);
        Assert.Equal(EngineState.Running, snapshot.State);
        Assert.Equal(48000u, snapshot.Header.SampleRate);
        Assert.Equal(4u, snapshot.Header.BusCount);
        Assert.Equal(32u, snapshot.Header.SpectrumBins);
        Assert.Equal(42UL, snapshot.Header.Sequence);
        Assert.Equal(0.85f, snapshot.Channels[1].PeakLeft, precision: 4);
        Assert.True(snapshot.Channels[1].IsActive);
        Assert.True(snapshot.Channels[1].IsClipping);
        Assert.Equal(0.72f, snapshot.Buses[0].PeakLeft, precision: 4);
        Assert.True(snapshot.Buses[0].IsActive);
        Assert.Equal(-18.5f, snapshot.SpectrumDb[10], precision: 3);
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

    [Fact]
    public void TryRead_ReadsSparseChannelSlot_EvenWhenSlotIndexExceedsChannelCount()
    {
        string mapName = $"Local\\AtrisWaveTest_{Guid.NewGuid():N}";
        using var mmf = MemoryMappedFile.CreateNew(mapName, TelemetryConstants.TotalSnapshotSizeBytes);
        using var writer = mmf.CreateViewAccessor(0, TelemetryConstants.TotalSnapshotSizeBytes, MemoryMappedFileAccess.Write);

        var header = new TelemetryHeader
        {
            Magic = TelemetryConstants.Magic,
            Version = TelemetryConstants.ProtocolVersion,
            ChannelCount = 3,
            BusCount = 4,
            Sequence = 10,
        };
        writer.Write(0, ref header);

        var sparseFrame = new ChannelMeterFrame
        {
            ChannelId = 5,
            PeakLeft = 0.64f,
            PeakRight = 0.61f,
            RmsLeft = 0.32f,
            RmsRight = 0.30f,
            Flags = (uint)ChannelMeterFlags.Active,
        };
        int slot5Offset = TelemetryConstants.HeaderSizeBytes + (5 * TelemetryConstants.ChannelFrameSizeBytes);
        writer.Write(slot5Offset, ref sparseFrame);

        using var reader = new SharedMemoryTelemetryReader(mapName);
        Assert.True(reader.TryRead(out TelemetrySnapshot? snapshot));
        Assert.NotNull(snapshot);
        Assert.Equal(0.64f, snapshot.Channels[5].PeakLeft, precision: 4);
        Assert.True(snapshot.Channels[5].IsActive);
    }
}
