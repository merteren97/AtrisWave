using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using AtrisWave.Core.Models;
using AtrisWave.UI.ViewModels;

namespace AtrisWave.UI.Views;

public partial class MixerView : UserControl
{
    private const string ProcessDragFormat = "AtrisWave.ProcessName";
    private static readonly Brush DropHighlightBrush = new SolidColorBrush(Color.FromRgb(0x38, 0xBD, 0xF8));

    private Point _dragStartPoint;
    private string? _pendingDragProcess;
    private Border? _highlightedBorder;
    private Brush? _savedBorderBrush;

    public MixerView()
    {
        InitializeComponent();
    }

    private void OnAppChipMouseDown(object sender, MouseButtonEventArgs e)
    {
        // Do not start a drag if the user clicked the chip's inline '×' unbind button
        if (e.OriginalSource is DependencyObject dep && FindAncestor<Button>(dep) is not null)
        {
            _pendingDragProcess = null;
            return;
        }

        _dragStartPoint = e.GetPosition(null);
        _pendingDragProcess = sender switch
        {
            FrameworkElement { DataContext: AudioSessionInfo session } => session.AppName,
            FrameworkElement { DataContext: RoutedAppChip chip } => chip.ProcessName,
            _ => null,
        };
    }

    private void OnAppChipMouseMove(object sender, MouseEventArgs e)
    {
        if (e.LeftButton != MouseButtonState.Pressed || string.IsNullOrWhiteSpace(_pendingDragProcess))
        {
            return;
        }

        Point current = e.GetPosition(null);
        if (Math.Abs(current.X - _dragStartPoint.X) < SystemParameters.MinimumHorizontalDragDistance &&
            Math.Abs(current.Y - _dragStartPoint.Y) < SystemParameters.MinimumVerticalDragDistance)
        {
            return;
        }

        string proc = _pendingDragProcess;
        _pendingDragProcess = null;

        var data = new DataObject();
        data.SetData(ProcessDragFormat, proc);
        if (sender is FrameworkElement { DataContext: AudioSessionInfo session })
        {
            data.SetData(typeof(AudioSessionInfo), session);
        }

        DragDrop.DoDragDrop((DependencyObject)sender, data, DragDropEffects.Move | DragDropEffects.Copy);
    }

    private void OnChannelDragEnter(object sender, DragEventArgs e)
    {
        if (HasDraggableApp(e.Data) && sender is Border border)
        {
            if (_highlightedBorder is not null && !ReferenceEquals(_highlightedBorder, border))
            {
                RestoreBorderHighlight(_highlightedBorder);
            }

            if (!ReferenceEquals(_highlightedBorder, border))
            {
                _highlightedBorder = border;
                _savedBorderBrush = border.BorderBrush;
            }
            border.BorderBrush = DropHighlightBrush;
            border.BorderThickness = new Thickness(1.5);
        }
    }

    private void OnChannelDragLeave(object sender, DragEventArgs e)
    {
        RestoreBorderHighlight(sender);
    }

    private void OnChannelDragOver(object sender, DragEventArgs e)
    {
        e.Effects = HasDraggableApp(e.Data) ? DragDropEffects.Move : DragDropEffects.None;
        e.Handled = true;
    }

    private async void OnDynamicChannelDrop(object sender, DragEventArgs e)
    {
        RestoreBorderHighlight(sender);
        if (DataContext is not MainViewModel vm ||
            sender is not FrameworkElement { DataContext: DynamicChannelModel channel })
        {
            return;
        }

        string? proc = ExtractProcessName(e.Data);
        if (string.IsNullOrWhiteSpace(proc))
        {
            return;
        }

        try
        {
            await vm.DropProcessOnChannelAsync(proc, channel);
        }
        catch
        {
        }
    }

    private async void OnSystemChannelDrop(object sender, DragEventArgs e)
    {
        RestoreBorderHighlight(sender);
        if (DataContext is not MainViewModel vm || vm.SystemChannel is null)
        {
            return;
        }

        string? proc = ExtractProcessName(e.Data);
        if (string.IsNullOrWhiteSpace(proc))
        {
            return;
        }

        try
        {
            await vm.DropProcessOnChannelAsync(proc, vm.SystemChannel);
        }
        catch
        {
        }
    }

    private void OnChatMixBarMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2 && DataContext is MainViewModel vm)
        {
            vm.ResetChatMixCommand.Execute(null);
        }
    }

    private static bool HasDraggableApp(IDataObject data)
        => data.GetDataPresent(ProcessDragFormat) || data.GetDataPresent(typeof(AudioSessionInfo));

    private static string? ExtractProcessName(IDataObject data)
    {
        if (data.GetDataPresent(ProcessDragFormat) && data.GetData(ProcessDragFormat) is string proc && !string.IsNullOrWhiteSpace(proc))
        {
            return proc;
        }

        if (data.GetDataPresent(typeof(AudioSessionInfo)) && data.GetData(typeof(AudioSessionInfo)) is AudioSessionInfo session)
        {
            return string.IsNullOrWhiteSpace(session.AppName) ? session.EffectiveLabel : session.AppName;
        }

        return null;
    }

    private void RestoreBorderHighlight(object sender)
    {
        if (sender is Border border)
        {
            if (ReferenceEquals(_highlightedBorder, border))
            {
                border.BorderBrush = _savedBorderBrush;
                _highlightedBorder = null;
                _savedBorderBrush = null;
            }
            border.BorderThickness = new Thickness(1);
        }
    }

    private static T? FindAncestor<T>(DependencyObject? current) where T : DependencyObject
    {
        while (current is not null)
        {
            if (current is T match)
            {
                return match;
            }
            current = VisualTreeHelper.GetParent(current);
        }
        return null;
    }
}
