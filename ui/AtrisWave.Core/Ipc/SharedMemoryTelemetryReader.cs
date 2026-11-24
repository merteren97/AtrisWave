using System.IO.MemoryMappedFiles;

namespace AtrisWave.Core.Ipc;

/// <summary>
/// Seqlock-safe reader for the engine's Telemetry V2 shared-memory block (<c>Local\AtrisWaveTelemetryV2</c>, 704 bytes).
/// Handles offline state gracefully when the native engine is not running.
/// </summary>
public sealed class SharedMemoryTelemetryReader : IDisposable
{
    private const int SequenceFieldOffset = 24;
    private const int BusesBaseOffset = TelemetryConstants.HeaderSizeBytes +
                                        (TelemetryConstants.MaxChannels * TelemetryConstants.ChannelFrameSizeBytes);
    private const int MaxSeqlockRetries = 8;

    private readonly string _mapName;
    private MemoryMappedFile? _mmf;
    private MemoryMappedViewAccessor? _accessor;
    private bool _disposed;

    public SharedMemoryTelemetryReader(string mapName = TelemetryConstants.SharedMemoryName)
    {
        if (string.IsNullOrWhiteSpace(mapName))
        {
            throw new ArgumentException("Shared memory map name must not be empty.", nameof(mapName));
        }

        _mapName = mapName;
    }

    public bool TryRead(out TelemetrySnapshot? snapshot)
    {
        var channels = new ChannelMeterFrame[TelemetryConstants.MaxChannels];
        var buses = new ChannelMeterFrame[TelemetryConstants.MaxBuses];
        if (!TryRead(out TelemetryHeader header, channels, buses))
        {
            snapshot = null;
            return false;
        }

        snapshot = new TelemetrySnapshot(header, channels, buses);
        return true;
    }

    public bool TryRead(out TelemetryHeader header, Span<ChannelMeterFrame> channels)
    {
        Span<ChannelMeterFrame> dummyBuses = stackalloc ChannelMeterFrame[TelemetryConstants.MaxBuses];
        return TryRead(out header, channels, dummyBuses);
    }

    /// <summary>
    /// Zero-allocation Telemetry V2 overload reading up to 16 channels and 4 output buses.
    /// </summary>
    public bool TryRead(out TelemetryHeader header, Span<ChannelMeterFrame> channels, Span<ChannelMeterFrame> buses)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        header = default;

        if (channels.Length < TelemetryConstants.MaxChannels)
        {
            throw new ArgumentException($"Channel buffer must be at least {TelemetryConstants.MaxChannels} elements.", nameof(channels));
        }

        if (buses.Length < TelemetryConstants.MaxBuses)
        {
            throw new ArgumentException($"Bus buffer must be at least {TelemetryConstants.MaxBuses} elements.", nameof(buses));
        }

        if (!EnsureConnected())
        {
            return false;
        }

        MemoryMappedViewAccessor accessor = _accessor!;

        try
        {
            for (int attempt = 0; attempt < MaxSeqlockRetries; attempt++)
            {
                ulong seqStart = accessor.ReadUInt64(SequenceFieldOffset);
                Thread.MemoryBarrier();

                if ((seqStart & 1UL) != 0)
                {
                    Thread.SpinWait(16);
                    continue;
                }

                accessor.Read(0, out header);

                if (header.Magic != TelemetryConstants.Magic ||
                    header.Version != TelemetryConstants.ProtocolVersion)
                {
                    return false;
                }

                for (int i = 0; i < TelemetryConstants.MaxChannels; i++)
                {
                    int offset = TelemetryConstants.HeaderSizeBytes + (i * TelemetryConstants.ChannelFrameSizeBytes);
                    accessor.Read(offset, out channels[i]);
                }

                for (int i = 0; i < TelemetryConstants.MaxBuses; i++)
                {
                    int offset = BusesBaseOffset + (i * TelemetryConstants.ChannelFrameSizeBytes);
                    accessor.Read(offset, out buses[i]);
                }

                Thread.MemoryBarrier();
                ulong seqEnd = accessor.ReadUInt64(SequenceFieldOffset);

                if (seqStart == seqEnd)
                {
                    return true;
                }
            }
        }
        catch (Exception ex) when (ex is IOException or ObjectDisposedException or UnauthorizedAccessException)
        {
            ResetConnection();
            return false;
        }

        return false;
    }

    private bool EnsureConnected()
    {
        if (_accessor is not null)
        {
            return true;
        }

        try
        {
            _mmf = MemoryMappedFile.OpenExisting(_mapName, MemoryMappedFileRights.Read);
            _accessor = _mmf.CreateViewAccessor(0, TelemetryConstants.TotalSnapshotSizeBytes, MemoryMappedFileAccess.Read);
            return true;
        }
        catch (Exception ex) when (ex is FileNotFoundException or UnauthorizedAccessException or IOException)
        {
            ResetConnection();
            return false;
        }
    }

    private void ResetConnection()
    {
        _accessor?.Dispose();
        _accessor = null;
        _mmf?.Dispose();
        _mmf = null;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        ResetConnection();
    }
}
