using System.ComponentModel;
using System.Windows;
using AtrisWave.UI.ViewModels;
using Wpf.Ui.Controls;

namespace AtrisWave.UI;

public partial class MainWindow : FluentWindow
{
    private readonly MainViewModel _viewModel;
    private readonly Services.GlobalHotkeyService _hotkeyService = new();
    private Point _dragStartPoint;

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

    private void OnSessionDeviceSelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: AtrisWave.Core.Models.AudioSessionInfo session })
        {
            _viewModel.RouteAppCommand.Execute(session);
        }
    }

    private void OnSessionsGridPreviewMouseDown(object sender, System.Windows.Input.MouseButtonEventArgs e)
        => _dragStartPoint = e.GetPosition(null);

    private void OnSessionsGridMouseMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (e.LeftButton != System.Windows.Input.MouseButtonState.Pressed)
        {
            return;
        }

        Point pos = e.GetPosition(null);
        Vector diff = _dragStartPoint - pos;
        if (Math.Abs(diff.X) > SystemParameters.MinimumHorizontalDragDistance ||
            Math.Abs(diff.Y) > SystemParameters.MinimumVerticalDragDistance)
        {
            if (sender is System.Windows.Controls.DataGrid { SelectedItem: AtrisWave.Core.Models.AudioSessionInfo session } grid)
            {
                DragDrop.DoDragDrop(grid, new DataObject(typeof(AtrisWave.Core.Models.AudioSessionInfo), session), DragDropEffects.Copy);
            }
        }
    }

    private void OnChannelPillDragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(typeof(AtrisWave.Core.Models.AudioSessionInfo))
            ? DragDropEffects.Copy
            : DragDropEffects.None;
        e.Handled = true;
    }

    private async void OnChannelPillDrop(object sender, DragEventArgs e)
    {
        if (e.Data.GetData(typeof(AtrisWave.Core.Models.AudioSessionInfo)) is AtrisWave.Core.Models.AudioSessionInfo session &&
            sender is FrameworkElement { Tag: string channelTag } &&
            Enum.TryParse<AtrisWave.Core.Models.MixerChannelId>(channelTag, out var channelId))
        {
            try
            {
                await _viewModel.AssignSessionToChannelAsync(session, channelId);
            }
            catch
            {
                // Best-effort UI drop handler; avoid crashing the dispatcher if engine is offline.
            }
        }
    }

    private void OnDynamicChannelDragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(typeof(AtrisWave.Core.Models.AudioSessionInfo))
            ? DragDropEffects.Copy
            : DragDropEffects.None;
        e.Handled = true;
    }

    private async void OnDynamicChannelDrop(object sender, DragEventArgs e)
    {
        if (e.Data.GetData(typeof(AtrisWave.Core.Models.AudioSessionInfo)) is AtrisWave.Core.Models.AudioSessionInfo session &&
            sender is FrameworkElement { DataContext: AtrisWave.Core.Models.DynamicChannelModel channel })
        {
            try
            {
                await _viewModel.BindSessionToDynamicChannelAsync(session, channel);
            }
            catch
            {
            }
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
