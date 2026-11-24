using System.Globalization;
using System.Windows;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AtrisWave.UI.Services;

/// <summary>
/// Live runtime localization service that swaps EN/TR string dictionaries without restarting.
/// </summary>
public sealed partial class LocalizationService : ObservableObject
{
    private const string EnglishCode = "en";
    private const string TurkishCode = "tr";

    public static LocalizationService Instance { get; } = new();

    private ResourceDictionary? _activeStringsDictionary;

    [ObservableProperty]
    private string _currentLanguage = EnglishCode;

    [ObservableProperty]
    private int _revision;

    private LocalizationService()
    {
    }

    public void ToggleLanguage()
    {
        string next = string.Equals(CurrentLanguage, EnglishCode, StringComparison.OrdinalIgnoreCase)
            ? TurkishCode
            : EnglishCode;
        SetLanguage(next);
    }

    public void SetLanguage(string languageCode)
    {
        string normalized = string.Equals(languageCode, TurkishCode, StringComparison.OrdinalIgnoreCase)
            ? TurkishCode
            : EnglishCode;

        var uri = new Uri($"pack://application:,,,/AtrisWave.UI;component/Resources/Strings.{normalized}.xaml", UriKind.Absolute);
        var newDict = new ResourceDictionary { Source = uri };

        if (Application.Current is not null)
        {
            var merged = Application.Current.Resources.MergedDictionaries;

            if (_activeStringsDictionary is not null)
            {
                merged.Remove(_activeStringsDictionary);
            }
            else
            {
                ResourceDictionary? existing = merged.FirstOrDefault(d =>
                    d.Source is not null &&
                    d.Source.OriginalString.Contains("Resources/Strings.", StringComparison.OrdinalIgnoreCase));
                if (existing is not null)
                {
                    merged.Remove(existing);
                }
            }

            merged.Add(newDict);
        }

        _activeStringsDictionary = newDict;
        CurrentLanguage = normalized;

        var culture = new CultureInfo(normalized == TurkishCode ? "tr-TR" : "en-US");
        CultureInfo.CurrentUICulture = culture;

        Revision++;
    }

    public string GetString(string key)
    {
        if (Application.Current?.TryFindResource(key) is string value)
        {
            return value;
        }

        return key;
    }
}
