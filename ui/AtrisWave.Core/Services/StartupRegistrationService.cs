using Microsoft.Win32;

namespace AtrisWave.Core.Services;

public interface IRegistryRunStore
{
    string? GetValue(string name);
    void SetValue(string name, string commandLine);
    void DeleteValue(string name);
}

public sealed class WindowsRegistryRunStore : IRegistryRunStore
{
    private const string RunSubKey = @"Software\Microsoft\Windows\CurrentVersion\Run";

    public string? GetValue(string name)
    {
        using RegistryKey? key = Registry.CurrentUser.OpenSubKey(RunSubKey, writable: false);
        return key?.GetValue(name) as string;
    }

    public void SetValue(string name, string commandLine)
    {
        using RegistryKey? key = Registry.CurrentUser.CreateSubKey(RunSubKey, writable: true);
        key?.SetValue(name, commandLine, RegistryValueKind.String);
    }

    public void DeleteValue(string name)
    {
        using RegistryKey? key = Registry.CurrentUser.OpenSubKey(RunSubKey, writable: true);
        key?.DeleteValue(name, throwOnMissingValue: false);
    }
}

/// <summary>
/// Manages independent Windows logon auto-start entries in <c>HKCU\...\Run</c>
/// for the background audio engine and desktop UI.
/// </summary>
public sealed class StartupRegistrationService
{
    public const string EngineValueName = "AtrisWaveEngine";
    public const string UiValueName = "AtrisWaveUI";

    private readonly IRegistryRunStore _store;

    public StartupRegistrationService(IRegistryRunStore? store = null)
    {
        _store = store ?? new WindowsRegistryRunStore();
    }

    public bool IsEngineAutoStartEnabled()
        => !string.IsNullOrWhiteSpace(_store.GetValue(EngineValueName));

    public bool IsAppAutoStartEnabled()
        => !string.IsNullOrWhiteSpace(_store.GetValue(UiValueName));

    public void SetEngineAutoStart(bool enabled, string? engineExePath)
    {
        if (!enabled)
        {
            _store.DeleteValue(EngineValueName);
            return;
        }

        if (string.IsNullOrWhiteSpace(engineExePath))
        {
            throw new ArgumentException("Engine executable path is required to enable auto-start.", nameof(engineExePath));
        }

        _store.SetValue(EngineValueName, $"\"{engineExePath}\"");
    }

    public void SetAppAutoStart(bool enabled, string? uiExePath, bool startMinimized)
    {
        if (!enabled)
        {
            _store.DeleteValue(UiValueName);
            return;
        }

        if (string.IsNullOrWhiteSpace(uiExePath))
        {
            throw new ArgumentException("UI executable path is required to enable auto-start.", nameof(uiExePath));
        }

        string cmd = startMinimized ? $"\"{uiExePath}\" --minimized" : $"\"{uiExePath}\"";
        _store.SetValue(UiValueName, cmd);
    }
}
