using CommunityToolkit.Mvvm.ComponentModel;
using KenshiMP.Server.Interop;

namespace KenshiMP.Server.ViewModels;

public partial class StatsViewModel : ViewModelBase
{
    private readonly ServerCore _core;

    [ObservableProperty] private string _uptime       = "0s";
    [ObservableProperty] private string _players      = "0 / 0";
    [ObservableProperty] private string _packetsInOut = "0 / 0";
    [ObservableProperty] private string _bytesInOut   = "0 / 0";

    public StatsViewModel(ServerCore core) { _core = core; }

    public void Refresh(uint maxPlayers)
    {
        var s = _core.GetStats();
        int h = (int)(s.uptime_seconds / 3600);
        int m = (int)((s.uptime_seconds % 3600) / 60);
        int sec = (int)(s.uptime_seconds % 60);
        Uptime = $"{h}h {m:00}m {sec:00}s";
        Players = $"{s.player_count} / {maxPlayers}";
        PacketsInOut = $"{s.packets_in} / {s.packets_out}";
        BytesInOut   = $"{s.bytes_in / 1024.0:F1} KB / {s.bytes_out / 1024.0:F1} KB";
    }
}
