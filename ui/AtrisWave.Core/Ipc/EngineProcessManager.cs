using System.Diagnostics;

namespace AtrisWave.Core.Ipc;

/// <summary>
/// Locates and manages the headless <c>AtrisWaveEngine.exe</c> Session 1+ background daemon.
/// Supports attaching to an already-running instance and never hard-kills the daemon on UI dispose.
/// </summary>
public sealed class EngineProcessManager : IDisposable
{
    public const string SingleInstanceMutexName = @"Local\AtrisWaveEngineSingleInstance";
    public const string ShutdownEventName = @"Local\AtrisWaveEngineShutdown";

    private readonly object _sync = new();
    private Process? _process;
    private bool _disposed;

    public bool IsRunning => IsEngineDetected();

    public bool IsEngineDetected()
    {
        lock (_sync)
        {
            if (_disposed)
            {
                return false;
            }

            try
            {
                if (_process is not null && !_process.HasExited)
                {
                    return true;
                }
            }
            catch
            {
                _process?.Dispose();
                _process = null;
            }
        }

        if (Mutex.TryOpenExisting(SingleInstanceMutexName, out Mutex? mutex))
        {
            mutex.Dispose();
            return true;
        }

        Process[] existing = Process.GetProcessesByName("AtrisWaveEngine");
        try
        {
            foreach (Process p in existing)
            {
                try
                {
                    if (!p.HasExited)
                    {
                        return true;
                    }
                }
                catch
                {
                    // Process exited or inaccessible.
                }
            }
        }
        finally
        {
            foreach (Process p in existing)
            {
                p.Dispose();
            }
        }

        return false;
    }

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

        if (IsEngineDetected())
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

            Process? started = Process.Start(psi);
            lock (_sync)
            {
                _process?.Dispose();
                _process = started;
            }

            error = started is null ? "Failed to start engine process." : null;
            return started is not null;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    public async Task StopEngineAsync(EnginePipeClient? pipeClient = null)
    {
        if (!IsEngineDetected())
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
                // Fall back to named shutdown event below.
            }
        }

        try
        {
            if (EventWaitHandle.TryOpenExisting(ShutdownEventName, out EventWaitHandle? shutdownEvt))
            {
                using (shutdownEvt)
                {
                    shutdownEvt.Set();
                }
            }
        }
        catch
        {
            // Named event may not exist on older builds or already exited.
        }

        // Wait up to 3 seconds for graceful shutdown before PID kill fallback.
        for (int i = 0; i < 30; i++)
        {
            if (!IsEngineDetected())
            {
                break;
            }

            await Task.Delay(100).ConfigureAwait(false);
        }

        if (IsEngineDetected())
        {
            KillResidualEngineProcesses();
        }

        lock (_sync)
        {
            _process?.Dispose();
            _process = null;
        }
    }

    public async Task<(bool Ok, string? Error)> RestartEngineAsync(EnginePipeClient? pipeClient = null)
    {
        await StopEngineAsync(pipeClient).ConfigureAwait(false);
        await Task.Delay(150).ConfigureAwait(false);
        bool ok = TryStartEngine(out string? err);
        return (ok, err);
    }

    private void KillResidualEngineProcesses()
    {
        lock (_sync)
        {
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
        }

        Process[] existing = Process.GetProcessesByName("AtrisWaveEngine");
        try
        {
            foreach (Process p in existing)
            {
                try
                {
                    if (!p.HasExited)
                    {
                        p.Kill(entireProcessTree: true);
                    }
                }
                catch
                {
                }
            }
        }
        finally
        {
            foreach (Process p in existing)
            {
                p.Dispose();
            }
        }
    }

    public void Dispose()
    {
        lock (_sync)
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            _process?.Dispose();
            _process = null;
        }
    }
}
