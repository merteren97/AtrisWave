using System.Text.Json;
using System.Text.Json.Serialization;

namespace AtrisWave.Core.Services;

public sealed class AppSettings
{
    [JsonPropertyName("auto_start_engine_on_logon")]
    public bool AutoStartEngineOnLogon { get; set; }

    [JsonPropertyName("auto_start_app_on_logon")]
    public bool AutoStartAppOnLogon { get; set; }

    [JsonPropertyName("start_minimized_to_tray")]
    public bool StartMinimizedToTray { get; set; }

    [JsonPropertyName("keep_engine_running_on_exit")]
    public bool KeepEngineRunningOnExit { get; set; } = true;

    [JsonPropertyName("language")]
    public string Language { get; set; } = "en";

    [JsonPropertyName("is_streamer_mode")]
    public bool IsStreamerMode { get; set; }

    [JsonPropertyName("global_hotkeys_enabled")]
    public bool GlobalHotkeysEnabled { get; set; } = true;

    [JsonPropertyName("rpc_server_enabled")]
    public bool RpcServerEnabled { get; set; }
}

/// <summary>
/// Atomically persists UI preferences to <c>%LOCALAPPDATA%\AtrisWave\ui-settings.json</c>.
/// </summary>
public sealed class AppSettingsService
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
    };

    private readonly string _settingsPath;
    private readonly object _ioLock = new();

    public AppSettingsService(string? settingsPath = null)
    {
        if (!string.IsNullOrWhiteSpace(settingsPath))
        {
            _settingsPath = settingsPath;
        }
        else
        {
            string localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            _settingsPath = Path.Combine(localAppData, "AtrisWave", "ui-settings.json");
        }
    }

    public string SettingsPath => _settingsPath;

    public AppSettings Load()
    {
        lock (_ioLock)
        {
            try
            {
                if (!File.Exists(_settingsPath))
                {
                    return new AppSettings();
                }

                string json = File.ReadAllText(_settingsPath);
                return JsonSerializer.Deserialize<AppSettings>(json, JsonOptions) ?? new AppSettings();
            }
            catch
            {
                return new AppSettings();
            }
        }
    }

    public void Save(AppSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);

        lock (_ioLock)
        {
            string? dir = Path.GetDirectoryName(_settingsPath);
            if (!string.IsNullOrWhiteSpace(dir))
            {
                Directory.CreateDirectory(dir);
            }

            string tempPath = $"{_settingsPath}.{Guid.NewGuid():N}.tmp";
            string json = JsonSerializer.Serialize(settings, JsonOptions);
            File.WriteAllText(tempPath, json);
            File.Move(tempPath, _settingsPath, overwrite: true);
        }
    }
}
