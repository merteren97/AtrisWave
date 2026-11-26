using AtrisWave.Core.Services;
using Xunit;

namespace AtrisWave.Core.Tests;

public sealed class AppSettingsAndStartupTests
{
    private sealed class InMemoryRunStore : IRegistryRunStore
    {
        private readonly Dictionary<string, string> _values = new(StringComparer.OrdinalIgnoreCase);

        public string? GetValue(string name)
            => _values.TryGetValue(name, out string? v) ? v : null;

        public void SetValue(string name, string commandLine)
            => _values[name] = commandLine;

        public void DeleteValue(string name)
            => _values.Remove(name);
    }

    [Fact]
    public void AppSettingsService_RoundtripsSettingsAtomically()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"AtrisWaveSettingsTest_{Guid.NewGuid():N}");
        string settingsPath = Path.Combine(tempDir, "ui-settings.json");

        try
        {
            var svc = new AppSettingsService(settingsPath);
            AppSettings defaults = svc.Load();
            Assert.True(defaults.KeepEngineRunningOnExit);
            Assert.False(defaults.StartMinimizedToTray);

            var updated = new AppSettings
            {
                AutoStartEngineOnLogon = true,
                AutoStartAppOnLogon = true,
                StartMinimizedToTray = true,
                KeepEngineRunningOnExit = false,
                Language = "tr",
                IsStreamerMode = true,
                GlobalHotkeysEnabled = false,
                RpcServerEnabled = true,
            };

            svc.Save(updated);
            AppSettings loaded = svc.Load();

            Assert.True(loaded.AutoStartEngineOnLogon);
            Assert.True(loaded.AutoStartAppOnLogon);
            Assert.True(loaded.StartMinimizedToTray);
            Assert.False(loaded.KeepEngineRunningOnExit);
            Assert.Equal("tr", loaded.Language);
            Assert.True(loaded.IsStreamerMode);
            Assert.False(loaded.GlobalHotkeysEnabled);
            Assert.True(loaded.RpcServerEnabled);
        }
        finally
        {
            if (Directory.Exists(tempDir))
            {
                Directory.Delete(tempDir, recursive: true);
            }
        }
    }

    [Fact]
    public void StartupRegistrationService_ManagesEngineAndUiEntriesIndependently()
    {
        var store = new InMemoryRunStore();
        var svc = new StartupRegistrationService(store);

        Assert.False(svc.IsEngineAutoStartEnabled());
        Assert.False(svc.IsAppAutoStartEnabled());

        svc.SetEngineAutoStart(true, @"C:\Program Files\AtrisWave\AtrisWaveEngine.exe");
        Assert.True(svc.IsEngineAutoStartEnabled());
        Assert.Equal("\"C:\\Program Files\\AtrisWave\\AtrisWaveEngine.exe\"", store.GetValue(StartupRegistrationService.EngineValueName));
        Assert.False(svc.IsAppAutoStartEnabled());

        svc.SetAppAutoStart(true, @"C:\Program Files\AtrisWave\AtrisWave.UI.exe", startMinimized: true);
        Assert.True(svc.IsAppAutoStartEnabled());
        Assert.Equal("\"C:\\Program Files\\AtrisWave\\AtrisWave.UI.exe\" --minimized", store.GetValue(StartupRegistrationService.UiValueName));

        svc.SetAppAutoStart(true, @"C:\Program Files\AtrisWave\AtrisWave.UI.exe", startMinimized: false);
        Assert.Equal("\"C:\\Program Files\\AtrisWave\\AtrisWave.UI.exe\"", store.GetValue(StartupRegistrationService.UiValueName));

        svc.SetEngineAutoStart(false, null);
        Assert.False(svc.IsEngineAutoStartEnabled());
        Assert.True(svc.IsAppAutoStartEnabled());

        svc.SetAppAutoStart(false, null, false);
        Assert.False(svc.IsAppAutoStartEnabled());
    }
}
