using System.Diagnostics;

namespace AtrisWave.Core.Ipc;

/// <summary>
/// Locates and manages the headless <c>AtrisWaveEngine.exe</c> Session 1+ child process.
/// </summary>
public sealed class EngineProcessManager : IDisposable
{
    private Process? _process;
    private bool _disposed;

    public bool IsRunning => _process is not null && !_process.HasExited;

    public string? FindEngineExecutable()
    {
        string baseDir = AppContext.BaseDirectory;
        string localCandidate = Path.Combine(baseDir, "AtrisWaveEngine.exe");
        if (File.Exists(localCandidate))
        {
            return localCandidate;
        }

        DirectoryInfo? dir = new(baseDir);
        while (dir is not null)
        {
            string releaseCandidate = Path.Combine(dir.FullName, "engine", "build", "msvc-release", "AtrisWaveEngine.exe");
            if (File.Exists(releaseCandidate))
            {
                return releaseCandidate;
            }

            string debugCandidate = Path.Combine(dir.FullName, "engine", "build", "msvc-debug", "AtrisWaveEngine.exe");
            if (File.Exists(debugCandidate))
            {
                return debugCandidate;
            }

            dir = dir.Parent;
        }

        return null;
    }

    public bool TryStartEngine(out string? error)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (IsRunning)
        {
            error = null;
            return true;
        }

        string? exePath = FindEngineExecutable();
        if (exePath is null)
        {
            error = "AtrisWaveEngine.exe not found.";
            return false;
        }

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = exePath,
                WorkingDirectory = Path.GetDirectoryName(exePath)!,
                CreateNoWindow = true,
                UseShellExecute = false,
            };

            _process = Process.Start(psi);
            error = _process is null ? "Failed to start engine process." : null;
            return _process is not null;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    public async Task StopEngineAsync(EnginePipeClient? pipeClient = null)
    {
        if (!IsRunning)
        {
            return;
        }

        if (pipeClient is not null)
        {
            try
            {
                await pipeClient.ShutdownAsync().ConfigureAwait(false);
            }
            catch
            {
                // Fall back to process termination below.
            }
        }

        try
        {
            if (_process is not null && !_process.HasExited)
            {
                if (!_process.WaitForExit(1500))
                {
                    _process.Kill(entireProcessTree: true);
                }
            }
        }
        catch
        {
            // Best-effort cleanup on shutdown.
        }
        finally
        {
            _process?.Dispose();
            _process = null;
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        try
        {
            if (_process is not null && !_process.HasExited)
            {
                _process.Kill(entireProcessTree: true);
            }
        }
        catch
        {
        }
        finally
        {
            _process?.Dispose();
            _process = null;
        }
    }
}
