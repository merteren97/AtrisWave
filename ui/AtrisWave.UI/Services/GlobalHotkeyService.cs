using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace AtrisWave.UI.Services;

/// <summary>
/// Native Win32 <c>RegisterHotKey</c> service for global shortcuts (<c>Ctrl+Alt+M</c>, <c>Ctrl+Alt+S</c>).
/// </summary>
public sealed class GlobalHotkeyService : IDisposable
{
    private const int WmHotkey = 0x0312;
    private const uint ModAlt = 0x0001;
    private const uint ModControl = 0x0002;
    private const uint VkM = 0x4D;
    private const uint VkS = 0x53;

    private const int HotkeyMicMuteId = 1;
    private const int HotkeyDirectMonitorId = 2;

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterHotKey(nint hWnd, int id, uint fsModifiers, uint vk);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnregisterHotKey(nint hWnd, int id);

    private HwndSource? _hwndSource;
    private bool _registered;
    private bool _disposed;

    public event EventHandler? ToggleMicMuteRequested;
    public event EventHandler? ToggleDirectMonitorRequested;

    public void Attach(Window window)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(window);

        var helper = new WindowInteropHelper(window);
        nint handle = helper.EnsureHandle();
        _hwndSource = HwndSource.FromHwnd(handle);
        _hwndSource?.AddHook(WndProc);
    }

    public void SetEnabled(bool enabled)
    {
        if (_hwndSource is null || _disposed)
        {
            return;
        }

        if (enabled && !_registered)
        {
            RegisterHotKey(_hwndSource.Handle, HotkeyMicMuteId, ModControl | ModAlt, VkM);
            RegisterHotKey(_hwndSource.Handle, HotkeyDirectMonitorId, ModControl | ModAlt, VkS);
            _registered = true;
        }
        else if (!enabled && _registered)
        {
            UnregisterAll();
        }
    }

    private nint WndProc(nint hwnd, int msg, nint wParam, nint lParam, ref bool handled)
    {
        if (msg == WmHotkey)
        {
            int id = wParam.ToInt32();
            if (id == HotkeyMicMuteId)
            {
                ToggleMicMuteRequested?.Invoke(this, EventArgs.Empty);
                handled = true;
            }
            else if (id == HotkeyDirectMonitorId)
            {
                ToggleDirectMonitorRequested?.Invoke(this, EventArgs.Empty);
                handled = true;
            }
        }

        return nint.Zero;
    }

    private void UnregisterAll()
    {
        if (_hwndSource is not null && _registered)
        {
            UnregisterHotKey(_hwndSource.Handle, HotkeyMicMuteId);
            UnregisterHotKey(_hwndSource.Handle, HotkeyDirectMonitorId);
            _registered = false;
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        UnregisterAll();
        _hwndSource?.RemoveHook(WndProc);
        _hwndSource = null;
    }
}
