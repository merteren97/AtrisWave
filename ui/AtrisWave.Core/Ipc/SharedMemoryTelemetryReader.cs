using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;

namespace AtrisWave.Core.Ipc;

/// <summary>
/// Seqlock-safe reader for the engine's Telemetry V3 shared-memory block (<c>Local\AtrisWaveTelemetryV3</c>, 832 bytes).
/// </summary>
public sealed class SharedMemoryTelemetryReader : IDisposable
{
    private const int SequenceFieldOffset = 24;
    private const int BusesBaseOffset = TelemetryConstants.HeaderSizeBytes +
                                        (TelemetryConstants.MaxChannels * TelemetryConstants.ChannelFrameSizeBytes);
    private const int SpectrumBaseOffset = BusesBaseOffset +
                                           (TelemetryConstants.MaxBuses * TelemetryConstants.ChannelFrameSizeBytes);
    private const int MaxSeqlockRetries = 8;
    private const long ReconnectCooldownMs = 750;
    private const long StaleTelemetryThresholdMs = 1500;

    private readonly string _mapName;
    private readonly byte[] _snapshotBuffer = new byte[TelemetryConstants.TotalSnapshotSizeBytes];
    private MemoryMappedFile? _mmf;
    private MemoryMappedViewAccessor? _accessor;
    private long _lastConnectAttemptTickMs;
    private ulong _lastSeenSequence;
    private ulong _lastSeenQpc;
    private long _lastFreshTickMs;
    private bool _hasSeenSnapshot;
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
        var spectrum = new float[TelemetryConstants.SpectrumBins];
        if (!TryRead(out TelemetryHeader header, channels, buses, spectrum))
        {
            snapshot = null;
            return false;
        }

        snapshot = new TelemetrySnapshot(header, channels, buses, spectrum);
        return true;
    }

    public bool TryRead(out TelemetryHeader header, Span<ChannelMeterFrame> channels, Span<ChannelMeterFrame> buses)
    {
        Span<float> dummySpectrum = stackalloc float[TelemetryConstants.SpectrumBins];
        return TryRead(out header, channels, buses, dummySpectrum);
    }

    /// <summary>
    /// Zero-allocation Telemetry V3 overload reading all 16 channels, 4 buses, and 32 spectrum bins.
    /// </summary>
    public bool TryRead(
        out TelemetryHeader header,
        Span<ChannelMeterFrame> channels,
        Span<ChannelMeterFrame> buses,
        Span<float> spectrumDb)
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

        if (spectrumDb.Length < TelemetryConstants.SpectrumBins)
        {
            throw new ArgumentException($"Spectrum buffer must be at least {TelemetryConstants.SpectrumBins} elements.", nameof(spectrumDb));
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

                accessor.ReadArray(0, _snapshotBuffer, 0, TelemetryConstants.TotalSnapshotSizeBytes);

                Thread.MemoryBarrier();
                ulong seqEnd = accessor.ReadUInt64(SequenceFieldOffset);

                if (seqStart != seqEnd)
                {
                    continue;
                }

                ReadOnlySpan<byte> buf = _snapshotBuffer;
                header = MemoryMarshal.Read<TelemetryHeader>(buf[..TelemetryConstants.HeaderSizeBytes]);

                if (header.Magic != TelemetryConstants.Magic ||
                    header.Version != TelemetryConstants.ProtocolVersion)
                {
                    return false;
                }

                MemoryMarshal.Cast<byte, ChannelMeterFrame>(
                    buf.Slice(TelemetryConstants.HeaderSizeBytes, TelemetryConstants.MaxChannels * TelemetryConstants.ChannelFrameSizeBytes))
                    .CopyTo(channels);

                MemoryMarshal.Cast<byte, ChannelMeterFrame>(
                    buf.Slice(BusesBaseOffset, TelemetryConstants.MaxBuses * TelemetryConstants.ChannelFrameSizeBytes))
                    .CopyTo(buses);

                MemoryMarshal.Cast<byte, float>(
                    buf.Slice(SpectrumBaseOffset, TelemetryConstants.SpectrumBytes))
                    .CopyTo(spectrumDb);

                long nowTick = Environment.TickCount64;
                if (!_hasSeenSnapshot || header.Sequence != _lastSeenSequence || header.TimestampQpc != _lastSeenQpc)
                {
                    _hasSeenSnapshot = true;
                    _lastSeenSequence = header.Sequence;
                    _lastSeenQpc = header.TimestampQpc;
                    _lastFreshTickMs = nowTick;
                }
                else if (nowTick - _lastFreshTickMs > StaleTelemetryThresholdMs)
                {
                    ResetConnection();
                    return false;
                }

                return true;
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

        long now = Environment.TickCount64;
        if (_lastConnectAttemptTickMs != 0 && (now - _lastConnectAttemptTickMs) < ReconnectCooldownMs)
        {
            return false;
        }
        _lastConnectAttemptTickMs = now;

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
        _hasSeenSnapshot = false;
        _lastSeenSequence = 0;
        _lastSeenQpc = 0;
        _lastFreshTickMs = 0;
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
