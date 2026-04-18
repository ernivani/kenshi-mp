using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using KenshiMP.Server.Interop;

namespace KenshiMP.Server.ViewModels;

public partial class ConfigViewModel : ViewModelBase
{
    private readonly ServerCore _core;
    public const string DefaultPath = "server_config.json";

    [ObservableProperty] private ushort _port;
    [ObservableProperty] private uint   _maxPlayers;
    [ObservableProperty] private string _serverName = "";
    [ObservableProperty] private float  _viewDistance;
    [ObservableProperty] private string _status = "";

    public ConfigViewModel(ServerCore core, NativeMethods.kmp_server_config cfg)
    {
        _core = core;
        Port          = cfg.port;
        MaxPlayers    = cfg.max_players;
        ServerName    = cfg.server_name ?? "";
        ViewDistance  = cfg.view_distance;
    }

    [RelayCommand]
    private void Save()
    {
        var c = new NativeMethods.kmp_server_config {
            port = Port,
            max_players = MaxPlayers,
            server_name = ServerName,
            view_distance = ViewDistance,
        };
        Status = _core.SaveConfig(DefaultPath, c)
            ? $"Saved to {DefaultPath}"
            : "Save failed";
    }
}
