using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using AtrisWave.Core.Models;
using AtrisWave.UI.ViewModels;

namespace AtrisWave.UI.Views;

public partial class AppRoutingView : UserControl
{
    private Point _dragStartPoint;

    public AppRoutingView()
    {
        InitializeComponent();
    }

    private void OnSessionDeviceSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (DataContext is MainViewModel vm &&
            sender is FrameworkElement { DataContext: AudioSessionInfo session })
        {
            vm.RouteAppCommand.Execute(session);
        }
    }

    private void OnSessionsGridPreviewMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (IsInteractiveChild(e.OriginalSource as DependencyObject))
        {
            _dragStartPoint = new Point(double.NaN, double.NaN);
            return;
        }

        _dragStartPoint = e.GetPosition(null);
    }

    private void OnSessionsGridMouseMove(object sender, MouseEventArgs e)
    {
        if (e.LeftButton != MouseButtonState.Pressed || double.IsNaN(_dragStartPoint.X)) return;
        if (IsInteractiveChild(e.OriginalSource as DependencyObject)) return;

        Point pos = e.GetPosition(null);
        Vector diff = _dragStartPoint - pos;
        if (Math.Abs(diff.X) > SystemParameters.MinimumHorizontalDragDistance ||
            Math.Abs(diff.Y) > SystemParameters.MinimumVerticalDragDistance)
        {
            if (sender is DataGrid { SelectedItem: AudioSessionInfo session } grid)
            {
                DragDrop.DoDragDrop(grid, new DataObject(typeof(AudioSessionInfo), session), DragDropEffects.Copy);
            }
        }
    }

    private static bool IsInteractiveChild(DependencyObject? source)
    {
        while (source is not null)
        {
            if (source is ComboBox or ComboBoxItem or System.Windows.Controls.Primitives.ButtonBase or System.Windows.Controls.Primitives.ScrollBar or System.Windows.Controls.Primitives.Thumb)
            {
                return true;
            }

            if (source is DataGrid)
            {
                return false;
            }

            source = System.Windows.Media.VisualTreeHelper.GetParent(source);
        }

        return false;
    }

    private void OnChannelPillDragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(typeof(AudioSessionInfo))
            ? DragDropEffects.Copy
            : DragDropEffects.None;
        e.Handled = true;
    }

    private async void OnChannelPillDrop(object sender, DragEventArgs e)
    {
        if (DataContext is MainViewModel vm &&
            e.Data.GetData(typeof(AudioSessionInfo)) is AudioSessionInfo session &&
            sender is FrameworkElement { Tag: string channelTag } &&
            Enum.TryParse<MixerChannelId>(channelTag, out var channelId))
        {
            try
            {
                await vm.AssignSessionToChannelAsync(session, channelId);
            }
            catch
            {
            }
        }
    }
}
