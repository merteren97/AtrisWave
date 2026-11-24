using System.Windows;
using AtrisWave.UI.ViewModels;

namespace AtrisWave.UI;

public partial class TrayFlyoutWindow : Window
{
    private readonly Action _openStudioAction;
    private bool _isClosing;

    public TrayFlyoutWindow(MainViewModel viewModel, Action openStudioAction)
    {
        DataContext = viewModel;
        _openStudioAction = openStudioAction;
        InitializeComponent();

        Closing += (_, _) => _isClosing = true;
        Deactivated += (_, _) => SafeClose();
        ContentRendered += (_, _) => PositionNearTray();
    }

    public void ShowNearTray()
    {
        Show();
        PositionNearTray();
        Activate();
    }

    private void PositionNearTray()
    {
        Rect workArea = SystemParameters.WorkArea;
        double h = ActualHeight > 0 ? ActualHeight : DesiredSize.Height;
        Left = Math.Max(workArea.Left + 12, workArea.Right - Width - 12);
        Top = Math.Max(workArea.Top + 12, workArea.Bottom - h - 12);
    }

    private void SafeClose()
    {
        if (_isClosing)
        {
            return;
        }

        _isClosing = true;
        Close();
    }

    private void OnOpenStudioClick(object sender, RoutedEventArgs e)
    {
        SafeClose();
        _openStudioAction();
    }
}
