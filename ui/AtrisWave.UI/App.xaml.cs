using System.Windows;
using AtrisWave.UI.ViewModels;
using H.NotifyIcon;

namespace AtrisWave.UI;

public partial class App : Application
{
    private const string SingleInstanceMutexName = @"Local\AtrisWaveUISingleInstance";
    private const string ShowWindowEventName = @"Local\AtrisWaveUIShowWindow";

    private Mutex? _singleInstanceMutex;
    private EventWaitHandle? _showWindowEvent;
    private RegisteredWaitHandle? _showWindowWaitHandle;
    private MainViewModel? _viewModel;
    private MainWindow? _mainWindow;
    private TaskbarIcon? _trayIcon;
    private TrayFlyoutWindow? _trayFlyout;

    public bool IsExiting { get; private set; }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        try
        {
            Wpf.Ui.Appearance.ApplicationThemeManager.Apply(Wpf.Ui.Appearance.ApplicationTheme.Dark);
        }
        catch
        {
        }

        _singleInstanceMutex = new Mutex(initiallyOwned: true, SingleInstanceMutexName, out bool createdNew);
        if (!createdNew)
        {
            try
            {
                if (EventWaitHandle.TryOpenExisting(ShowWindowEventName, out EventWaitHandle? existingEvt))
                {
                    using (existingEvt)
                    {
                        existingEvt.Set();
                    }
                }
            }
            catch
            {
            }

            Shutdown();
            return;
        }

        _showWindowEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowWindowEventName);
        _showWindowWaitHandle = ThreadPool.RegisterWaitForSingleObject(
            _showWindowEvent,
            static (state, _) =>
            {
                if (state is App app)
                {
                    app.Dispatcher.BeginInvoke(app.ShowMainWindow);
                }
            },
            this,
            Timeout.Infinite,
            executeOnlyOnce: false);

        _viewModel = new MainViewModel();
        _mainWindow = new MainWindow(_viewModel);
        MainWindow = _mainWindow;

        _trayIcon = (TaskbarIcon)FindResource("AtrisTrayIcon");
        InitializeTrayIconWithRetry();

        bool startMinimized = e.Args.Any(a => string.Equals(a, "--minimized", StringComparison.OrdinalIgnoreCase)) ||
                              _viewModel.StartMinimizedToTray;

        if (startMinimized)
        {
            _ = _viewModel.EnsureEngineAutoStartedAsync();
        }
        else
        {
            _mainWindow.Show();
        }
    }

    private void InitializeTrayIconWithRetry()
    {
        if (TryCreateTrayIcon())
        {
            return;
        }

        // At Windows logon (auto-start) or right after an unclean crash, Explorer's tray may not
        // accept NIM_ADD immediately. Retry in the background without ever crashing app startup.
        _ = Task.Run(async () =>
        {
            for (int attempt = 0; attempt < 5 && !IsExiting; attempt++)
            {
                await Task.Delay(1500).ConfigureAwait(false);
                if (IsExiting)
                {
                    return;
                }

                bool created = false;
                try
                {
                    await Dispatcher.InvokeAsync(() => created = TryCreateTrayIcon());
                }
                catch
                {
                    return;
                }

                if (created)
                {
                    return;
                }
            }
        });
    }

    private bool TryCreateTrayIcon()
    {
        if (_trayIcon is null)
        {
            return false;
        }

        try
        {
            _trayIcon.ForceCreate();
            return true;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"[App] Tray icon creation deferred: {ex.Message}");
            return false;
        }
    }

    public void ShowMainWindow()
    {
        if (_mainWindow is null)
        {
            return;
        }

        if (!_mainWindow.IsVisible)
        {
            _mainWindow.Show();
        }

        if (_mainWindow.WindowState == WindowState.Minimized)
        {
            _mainWindow.WindowState = WindowState.Normal;
        }

        _mainWindow.Activate();
    }

    private void OnTrayShowMixerClick(object sender, RoutedEventArgs e)
        => ShowMainWindow();

    private void OnTrayLeftClick(object sender, RoutedEventArgs e)
    {
        if (_viewModel is null)
        {
            return;
        }

        if (_trayFlyout is not null && _trayFlyout.IsLoaded)
        {
            _trayFlyout.Activate();
            return;
        }

        _trayFlyout = new TrayFlyoutWindow(_viewModel, ShowMainWindow);
        _trayFlyout.Closed += (_, _) => _trayFlyout = null;
        _trayFlyout.ShowNearTray();
    }

    private void OnTrayMuteMicClick(object sender, RoutedEventArgs e)
        => _viewModel?.ToggleMicMuteCommand.Execute(null);

    private void OnTrayLanguageClick(object sender, RoutedEventArgs e)
        => _viewModel?.ToggleLanguageCommand.Execute(null);

    private async void OnTrayExitClick(object sender, RoutedEventArgs e)
    {
        try
        {
            IsExiting = true;
            _trayFlyout?.Close();
            _trayIcon?.Dispose();
            if (_viewModel is not null)
            {
                if (!_viewModel.KeepEngineRunningOnExit)
                {
                    await _viewModel.ShutdownEngineOnExitAsync();
                }

                _viewModel.Dispose();
            }
            _mainWindow?.Close();
        }
        catch
        {
        }
        finally
        {
            Shutdown();
        }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        IsExiting = true;
        _trayFlyout?.Close();
        _trayIcon?.Dispose();
        _viewModel?.Dispose();
        _showWindowWaitHandle?.Unregister(null);
        _showWindowWaitHandle = null;
        _showWindowEvent?.Dispose();
        _showWindowEvent = null;
        if (_singleInstanceMutex is not null)
        {
            try
            {
                _singleInstanceMutex.ReleaseMutex();
            }
            catch
            {
            }
            _singleInstanceMutex.Dispose();
            _singleInstanceMutex = null;
        }
        base.OnExit(e);
    }
}
