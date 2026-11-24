using System.Windows;
using System.Windows.Controls;
using AtrisWave.Core.Models;
using AtrisWave.UI.ViewModels;

namespace AtrisWave.UI.Views;

public partial class MixerView : UserControl
{
    public MixerView()
    {
        InitializeComponent();
    }

    private void OnDynamicChannelDragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(typeof(AudioSessionInfo))
            ? DragDropEffects.Copy
            : DragDropEffects.None;
        e.Handled = true;
    }

    private async void OnDynamicChannelDrop(object sender, DragEventArgs e)
    {
        if (DataContext is MainViewModel vm &&
            e.Data.GetData(typeof(AudioSessionInfo)) is AudioSessionInfo session &&
            sender is FrameworkElement { DataContext: DynamicChannelModel channel })
        {
            try
            {
                await vm.BindSessionToDynamicChannelAsync(session, channel);
            }
            catch
            {
            }
        }
    }
}
