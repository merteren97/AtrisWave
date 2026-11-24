using System.ComponentModel;
using System.Windows;
using AtrisWave.UI.ViewModels;
using Wpf.Ui.Controls;

namespace AtrisWave.UI;

public partial class MainWindow : FluentWindow
{
    private readonly MainViewModel _viewModel;
    private readonly Services.GlobalHotkeyService _hotkeyService = new();

    public MainWindow(MainViewModel viewModel)
    {
        _viewModel = viewModel;
        DataContext = viewModel;
        InitializeComponent();
        StateChanged += OnWindowStateChanged;
        IsVisibleChanged += OnWindowVisibilityChanged;
        Loaded += OnMainWindowLoaded;
        Closed += OnMainWindowClosed;
    }

    private void OnMainWindowLoaded(object sender, RoutedEventArgs e)
    {
        _hotkeyService.Attach(this);
        _hotkeyService.ToggleMicMuteRequested += (_, _) => _viewModel.ToggleMicMuteCommand.Execute(null);
        _hotkeyService.ToggleDirectMonitorRequested += (_, _) => _viewModel.ToggleDirectMonitor();
        _hotkeyService.SetEnabled(_viewModel.GlobalHotkeysEnabled);
        _viewModel.GlobalHotkeysEnabledChanged += (_, enabled) => _hotkeyService.SetEnabled(enabled);
    }

    private void OnMainWindowClosed(object? sender, EventArgs e)
        => _hotkeyService.Dispose();

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
