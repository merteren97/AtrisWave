using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using AtrisWave.UI.ViewModels;

namespace AtrisWave.UI;

public partial class MainWindow : Window
{
    private const int WmGetMinMaxInfo = 0x0024;
    private const int WmNcHitTest = 0x0084;
    private const int WmNcMouseMove = 0x00A0;
    private const int WmNcLButtonDown = 0x00A1;
    private const int WmNcLButtonUp = 0x00A2;
    private const int WmMouseMove = 0x0200;
    private const int WmNcMouseLeave = 0x02A2;
    private const int HtMaxButton = 9;
    private const uint TmeLeave = 0x00000002;
    private const uint TmeNonClient = 0x00000010;
    private const uint MonitorDefaultToNearest = 0x00000002;
    private const int DwmwaUseImmersiveDarkMode = 20;
    private const int DwmwaWindowCornerPreference = 33;
    private const int DwmwcpRound = 2;
    private const uint AbmGetState = 0x00000004;
    private const uint AbsAutoHide = 0x00000001;

    private static readonly Brush CaptionHoverBackground = new SolidColorBrush(Color.FromArgb(0x1E, 0xFF, 0xFF, 0xFF));
    private static readonly Brush CaptionHoverForeground = new SolidColorBrush(Color.FromRgb(0xF8, 0xFA, 0xFC));
    private static readonly Geometry MaximizeGeometry = Geometry.Parse("M 0,0 L 10,0 L 10,10 L 0,10 Z");
    private static readonly Geometry RestoreGeometry = Geometry.Parse("M 0,2.5 L 7.5,2.5 L 7.5,10 L 0,10 Z M 2.5,2.5 L 2.5,0 L 10,0 L 10,7.5 L 7.5,7.5");

    private readonly MainViewModel _viewModel;
    private readonly Services.GlobalHotkeyService _hotkeyService = new();
    private HwndSource? _hwndSource;
    private bool _isMaximizeHovered;

    public MainWindow(MainViewModel viewModel)
    {
        _viewModel = viewModel;
        DataContext = viewModel;
        InitializeComponent();
        SourceInitialized += OnSourceInitialized;
        StateChanged += OnWindowStateChanged;
        IsVisibleChanged += OnWindowVisibilityChanged;
        Loaded += OnMainWindowLoaded;
        Closed += OnMainWindowClosed;
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        nint handle = new WindowInteropHelper(this).Handle;
        _hwndSource = HwndSource.FromHwnd(handle);
        _hwndSource?.AddHook(WndProc);
        ApplyDwmAttributes(handle);
    }

    private static void ApplyDwmAttributes(nint hwnd)
    {
        try
        {
            int dark = 1;
            _ = DwmSetWindowAttribute(hwnd, DwmwaUseImmersiveDarkMode, ref dark, sizeof(int));

            int corner = DwmwcpRound;
            _ = DwmSetWindowAttribute(hwnd, DwmwaWindowCornerPreference, ref corner, sizeof(int));
        }
        catch
        {
            // Older Windows 10 builds may not support Win11 corner attributes; ignore safely.
        }
    }

    private nint WndProc(nint hwnd, int msg, nint wParam, nint lParam, ref bool handled)
    {
        switch (msg)
        {
            case WmGetMinMaxInfo:
                ApplyWorkAreaMinMaxInfo(hwnd, lParam);
                handled = true;
                return nint.Zero;

            case WmNcHitTest when IsPointOverMaximizeButton(lParam):
                handled = true;
                return HtMaxButton;

            case WmNcMouseMove:
                if ((int)wParam.ToInt64() == HtMaxButton)
                {
                    SetMaximizeButtonHover(true, hwnd);
                }
                else
                {
                    SetMaximizeButtonHover(false, hwnd);
                }
                break;

            case WmNcMouseLeave:
            case WmMouseMove:
                SetMaximizeButtonHover(false, hwnd);
                break;

            case WmNcLButtonDown when (int)wParam.ToInt64() == HtMaxButton:
                SetMaximizeButtonHover(false, hwnd);
                handled = true;
                return nint.Zero;

            case WmNcLButtonUp when (int)wParam.ToInt64() == HtMaxButton:
                ToggleMaximizeRestore();
                handled = true;
                return nint.Zero;
        }

        return nint.Zero;
    }

    private void SetMaximizeButtonHover(bool hovered, nint hwnd)
    {
        if (MaximizeButton is null || _isMaximizeHovered == hovered)
        {
            return;
        }

        _isMaximizeHovered = hovered;
        if (hovered)
        {
            MaximizeButton.Background = CaptionHoverBackground;
            MaximizeButton.Foreground = CaptionHoverForeground;
            var tme = new TRACKMOUSEEVENT
            {
                cbSize = (uint)Marshal.SizeOf<TRACKMOUSEEVENT>(),
                dwFlags = TmeNonClient | TmeLeave,
                hwndTrack = hwnd,
                dwHoverTime = 0,
            };
            TrackMouseEvent(ref tme);
        }
        else
        {
            MaximizeButton.ClearValue(BackgroundProperty);
            MaximizeButton.ClearValue(ForegroundProperty);
        }
    }

    private bool IsPointOverMaximizeButton(nint lParam)
    {
        if (MaximizeButton is null || !MaximizeButton.IsVisible || MaximizeButton.ActualWidth <= 0)
        {
            return false;
        }

        try
        {
            long raw = lParam.ToInt64();
            int screenX = (short)(raw & 0xFFFF);
            int screenY = (short)((raw >> 16) & 0xFFFF);
            Point pt = MaximizeButton.PointFromScreen(new Point(screenX, screenY));
            return pt.X >= 0 && pt.X <= MaximizeButton.ActualWidth &&
                   pt.Y >= 0 && pt.Y <= MaximizeButton.ActualHeight;
        }
        catch
        {
            return false;
        }
    }

    private static void ApplyWorkAreaMinMaxInfo(nint hwnd, nint lParam)
    {
        nint monitor = MonitorFromWindow(hwnd, MonitorDefaultToNearest);
        if (monitor == nint.Zero)
        {
            return;
        }

        var monitorInfo = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
        if (!GetMonitorInfoW(monitor, ref monitorInfo))
        {
            return;
        }

        RECT work = monitorInfo.rcWork;
        RECT full = monitorInfo.rcMonitor;

        // Preserve auto-hide taskbar reveal strip when maximized
        if (work.Bottom == full.Bottom && IsTaskbarAutoHide())
        {
            work.Bottom -= 1;
        }

        var mmi = Marshal.PtrToStructure<MINMAXINFO>(lParam);
        mmi.ptMaxPosition.X = Math.Abs(work.Left - full.Left);
        mmi.ptMaxPosition.Y = Math.Abs(work.Top - full.Top);
        mmi.ptMaxSize.X = Math.Abs(work.Right - work.Left);
        mmi.ptMaxSize.Y = Math.Abs(work.Bottom - work.Top);
        Marshal.StructureToPtr(mmi, lParam, true);
    }

    private static bool IsTaskbarAutoHide()
    {
        var abd = new APPBARDATA { cbSize = Marshal.SizeOf<APPBARDATA>() };
        uint state = SHAppBarMessage(AbmGetState, ref abd);
        return (state & AbsAutoHide) != 0;
    }

    private async void OnMainWindowLoaded(object sender, RoutedEventArgs e)
    {
        _hotkeyService.Attach(this);
        _hotkeyService.ToggleMicMuteRequested += (_, _) => _viewModel.ToggleMicMuteCommand.Execute(null);
        _hotkeyService.ToggleDirectMonitorRequested += (_, _) => _viewModel.ToggleDirectMonitor();
        _hotkeyService.SetEnabled(_viewModel.GlobalHotkeysEnabled);
        _viewModel.GlobalHotkeysEnabledChanged += (_, enabled) => _hotkeyService.SetEnabled(enabled);

        try
        {
            await _viewModel.EnsureEngineAutoStartedAsync();
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"MainWindow startup engine sync failed: {ex.Message}");
        }
    }

    private void OnMainWindowClosed(object? sender, EventArgs e)
    {
        _hwndSource?.RemoveHook(WndProc);
        _hwndSource = null;
        _hotkeyService.Dispose();
    }

    private void OnMinimizeClick(object sender, RoutedEventArgs e)
        => WindowState = WindowState.Minimized;

    private void OnMaximizeRestoreClick(object sender, RoutedEventArgs e)
        => ToggleMaximizeRestore();

    private void ToggleMaximizeRestore()
        => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void OnCloseClick(object sender, RoutedEventArgs e)
        => Close();

    private void OnWindowStateChanged(object? sender, EventArgs e)
    {
        if (WindowState == WindowState.Minimized)
        {
            Hide();
        }

        UpdateMaximizeIcon();
        UpdateTelemetryState();
    }

    private void UpdateMaximizeIcon()
    {
        if (MaximizeRestoreIcon is null || MaximizeButton is null)
        {
            return;
        }

        bool isMaximized = WindowState == WindowState.Maximized;
        MaximizeRestoreIcon.Data = isMaximized ? RestoreGeometry : MaximizeGeometry;
        MaximizeButton.ToolTip = TryFindResource(isMaximized ? "Caption_Restore" : "Caption_Maximize");
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
    private struct MINMAXINFO
    {
        public POINT ptReserved;
        public POINT ptMaxSize;
        public POINT ptMaxPosition;
        public POINT ptMinTrackSize;
        public POINT ptMaxTrackSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct APPBARDATA
    {
        public int cbSize;
        public nint hWnd;
        public uint uCallbackMessage;
        public uint uEdge;
        public RECT rc;
        public nint lParam;
    }

    [DllImport("user32.dll")]
    private static extern nint MonitorFromWindow(nint hwnd, uint dwFlags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetMonitorInfoW(nint hMonitor, ref MONITORINFO lpmi);

    [DllImport("shell32.dll")]
    private static extern uint SHAppBarMessage(uint dwMessage, ref APPBARDATA pData);

    [StructLayout(LayoutKind.Sequential)]
    private struct TRACKMOUSEEVENT
    {
        public uint cbSize;
        public uint dwFlags;
        public nint hwndTrack;
        public uint dwHoverTime;
    }

    [DllImport("user32.dll")]
    private static extern bool TrackMouseEvent(ref TRACKMOUSEEVENT lpEventTrack);

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(nint hwnd, int attr, ref int attrValue, int attrSize);
}
