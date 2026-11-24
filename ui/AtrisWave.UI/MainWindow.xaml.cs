using System.ComponentModel;
using System.Windows;
using AtrisWave.UI.ViewModels;
using Wpf.Ui.Controls;

namespace AtrisWave.UI;

public partial class MainWindow : FluentWindow
{
    private readonly MainViewModel _viewModel;

    public MainWindow(MainViewModel viewModel)
    {
        _viewModel = viewModel;
        DataContext = viewModel;
        InitializeComponent();
        StateChanged += OnWindowStateChanged;
        IsVisibleChanged += OnWindowVisibilityChanged;
    }

    private void OnWindowStateChanged(object? sender, EventArgs e)
    {
        if (WindowState == WindowState.Minimized)
        {
            Hide();
        }

        UpdateTelemetryState();
    }

    private void OnWindowVisibilityChanged(object sender, DependencyPropertyChangedEventArgs e)
        => UpdateTelemetryState();

    private void UpdateTelemetryState()
        => _viewModel.SetTelemetryActive(IsVisible && WindowState != WindowState.Minimized);

    private void OnSessionDeviceSelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: AtrisWave.Core.Models.AudioSessionInfo session })
        {
            _viewModel.RouteAppCommand.Execute(session);
        }
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (Application.Current is App app && !app.IsExiting)
        {
            e.Cancel = true;
            Hide();
            return;
        }

        base.OnClosing(e);
    }
}
