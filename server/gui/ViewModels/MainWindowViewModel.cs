using System;
using System.Threading.Tasks;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using KenshiMP.Server.Interop;

namespace KenshiMP.Server.ViewModels;

public partial class MainWindowViewModel : ViewModelBase, IDisposable
{
    public ServerCore Core { get; }

    public PlayersViewModel PlayersVm { get; }
    public LogViewModel     LogVm     { get; }
    public ChatViewModel    ChatVm    { get; }
    public StatsViewModel   StatsVm   { get; }
    public PostureViewModel PostureVm { get; }
    public ConfigViewModel  ConfigVm  { get; }
    public SpawnViewModel   SpawnVm   { get; }

    [ObservableProperty] private string _statusText = "Starting...";

    private readonly DispatcherTimer _tick;
    private uint _maxPlayers;

    public MainWindowViewModel()
    {
        Core = new ServerCore();

        var cfg = Core.LoadConfig(ConfigViewModel.DefaultPath);
        _maxPlayers = cfg.max_players;

        PlayersVm = new PlayersViewModel(Core);
        LogVm     = new LogViewModel(Core);
        ChatVm    = new ChatViewModel(Core);
        StatsVm   = new StatsViewModel(Core);
        PostureVm = new PostureViewModel(Core, PlayersVm);
        ConfigVm  = new ConfigViewModel(Core, cfg);
        SpawnVm   = new SpawnViewModel(Core);

        if (Core.Start(cfg))
            StatusText = $"Listening on port {cfg.port}";
        else
            StatusText = "Server start failed — check log";

        _tick = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        _tick.Tick += (_, _) => OnTick();
        _tick.Start();
    }

    private void OnTick()
    {
        PlayersVm.Refresh();
        StatsVm.Refresh(_maxPlayers);
        PostureVm.UpdateStickyLabel();
        SpawnVm.Refresh();
    }

    [RelayCommand]
    private void Shutdown()
    {
        Core.Stop();
        StatusText = "Stopped";
    }

    public void Dispose()
    {
        _tick.Stop();
        Core.Dispose();
    }
}
