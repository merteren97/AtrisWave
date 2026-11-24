using System.Windows;
using System.Windows.Controls;
using AtrisWave.UI.ViewModels;

namespace AtrisWave.UI.Views;

public partial class MicStudioView : UserControl
{
    public MicStudioView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        Unloaded += OnUnloaded;
    }

    private void OnDataContextChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (e.OldValue is MainViewModel oldVm)
        {
            oldVm.SpectrumUpdated -= OnSpectrumUpdated;
        }

        if (e.NewValue is MainViewModel newVm)
        {
            newVm.SpectrumUpdated += OnSpectrumUpdated;
        }
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        if (DataContext is MainViewModel vm)
        {
            vm.SpectrumUpdated -= OnSpectrumUpdated;
        }
    }

    private void OnSpectrumUpdated(object? sender, EventArgs e)
        => EqCurve.RefreshSpectrum();
}
