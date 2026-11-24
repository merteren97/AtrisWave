using System.Windows;
using AtrisWave.UI.ViewModels;
using H.NotifyIcon;

namespace AtrisWave.UI;

public partial class App : Application
{
    private MainViewModel? _viewModel;
    private MainWindow? _mainWindow;
    private TaskbarIcon? _trayIcon;

    public bool IsExiting { get; private set; }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        _viewModel = new MainViewModel();
        _mainWindow = new MainWindow(_viewModel);
        MainWindow = _mainWindow;

        _trayIcon = (TaskbarIcon)FindResource("AtrisTrayIcon");
        _trayIcon.ForceCreate();

        _mainWindow.Show();
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

    private void OnTrayMuteMicClick(object sender, RoutedEventArgs e)
        => _viewModel?.ToggleMicMuteCommand.Execute(null);

    private void OnTrayLanguageClick(object sender, RoutedEventArgs e)
        => _viewModel?.ToggleLanguageCommand.Execute(null);

    private void OnTrayExitClick(object sender, RoutedEventArgs e)
    {
        IsExiting = true;
        _trayIcon?.Dispose();
        _viewModel?.Dispose();
        _mainWindow?.Close();
        Shutdown();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        IsExiting = true;
        _trayIcon?.Dispose();
        _viewModel?.Dispose();
        base.OnExit(e);
    }
}
