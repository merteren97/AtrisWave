using System.Globalization;
using System.Windows.Data;
using AtrisWave.UI.Services;

namespace AtrisWave.UI.Converters;

/// <summary>
/// Resolves a localization resource key and re-evaluates whenever <see cref="LocalizationService.Revision"/> changes.
/// </summary>
public sealed class LocKeyConverter : IMultiValueConverter
{
    public object Convert(object[] values, Type targetType, object parameter, CultureInfo culture)
    {
        if (values.Length > 0 && values[0] is string key && !string.IsNullOrEmpty(key))
        {
            return LocalizationService.Instance.GetString(key);
        }

        return string.Empty;
    }

    public object[] ConvertBack(object value, Type[] targetTypes, object parameter, CultureInfo culture)
        => throw new NotSupportedException();
}
