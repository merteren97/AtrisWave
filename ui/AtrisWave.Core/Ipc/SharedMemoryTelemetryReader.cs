using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;

namespace AtrisWave.Core.Ipc;

/// <summary>
/// Seqlock-safe reader for the engine's shared-memory telemetry block (<c>Local\AtrisWaveTelemetryV1</c>).
/// Handles offline state gracefully when the native engine is not running.
/// </summary>
public sealed class SharedMemoryTelemetryReader : IDisposable
{
    private const int SequenceFieldOffset = 24;
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

    /// <summary>
    /// Attempts to read a consistent telemetry snapshot using seqlock validation.
    /// Returns <c>false</c> when the engine is offline, the header is invalid, or a torn write is detected.
    /// </summary>
    public bool TryRead(out TelemetrySnapshot? snapshot)
    {
        var frames = new ChannelMeterFrame[TelemetryConstants.MaxChannels];
        if (!TryRead(out TelemetryHeader header, frames))
        {
            snapshot = null;
            return false;
        }

        snapshot = new TelemetrySnapshot(header, frames);
        return true;
    }

    /// <summary>
    /// Zero-allocation overload that writes channel meter frames into a caller-provided buffer.
    /// </summary>
    public bool TryRead(out TelemetryHeader header, Span<ChannelMeterFrame> channels)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        header = default;

        if (channels.Length < TelemetryConstants.MaxChannels)
        {
            throw new ArgumentException($"Channel buffer must be at least {TelemetryConstants.MaxChannels} elements.", nameof(channels));
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

                int channelCount = (int)Math.Min(header.ChannelCount, TelemetryConstants.MaxChannels);
                for (int i = 0; i < channelCount; i++)
                {
                    int offset = TelemetryConstants.HeaderSizeBytes + (i * TelemetryConstants.ChannelFrameSizeBytes);
                    accessor.Read(offset, out channels[i]);
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
