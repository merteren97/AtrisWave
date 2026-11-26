using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Media;
using AtrisWave.UI.ViewModels;

namespace AtrisWave.UI;

public partial class TrayFlyoutWindow : Window
{
    private const uint MonitorDefaultToNearest = 0x00000002;

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
        Rect workAreaDip = GetActiveMonitorWorkAreaDip();
        double h = ActualHeight > 0 ? ActualHeight : (DesiredSize.Height > 0 ? DesiredSize.Height : 260);
        Left = Math.Clamp(workAreaDip.Right - Width - 12, workAreaDip.Left + 12, Math.Max(workAreaDip.Left + 12, workAreaDip.Right - Width - 12));
        Top = Math.Clamp(workAreaDip.Bottom - h - 12, workAreaDip.Top + 12, Math.Max(workAreaDip.Top + 12, workAreaDip.Bottom - h - 12));
    }

    private Rect GetActiveMonitorWorkAreaDip()
    {
        try
        {
            if (GetCursorPos(out POINT cursor))
            {
                nint monitor = MonitorFromPoint(cursor, MonitorDefaultToNearest);
                var info = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
                if (monitor != nint.Zero && GetMonitorInfoW(monitor, ref info))
                {
                    PresentationSource? source = PresentationSource.FromVisual(this);
                    Matrix transform = source?.CompositionTarget?.TransformFromDevice ?? Matrix.Identity;
                    Point topLeft = transform.Transform(new Point(info.rcWork.Left, info.rcWork.Top));
                    Point bottomRight = transform.Transform(new Point(info.rcWork.Right, info.rcWork.Bottom));
                    return new Rect(topLeft, bottomRight);
                }
            }
        }
        catch
        {
        }

        return SystemParameters.WorkArea;
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

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
    }

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out POINT lpPoint);

    [DllImport("user32.dll")]
    private static extern nint MonitorFromPoint(POINT pt, uint dwFlags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetMonitorInfoW(nint hMonitor, ref MONITORINFO lpmi);
}
