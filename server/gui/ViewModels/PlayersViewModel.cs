using System.Collections.ObjectModel;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using KenshiMP.Server.Interop;

namespace KenshiMP.Server.ViewModels;

public partial class PlayersViewModel : ViewModelBase
{
    private readonly ServerCore _core;

    public ObservableCollection<PlayerRow> Players { get; } = new();

    [ObservableProperty] private PlayerRow? _selected;

    public PlayersViewModel(ServerCore core) { _core = core; }

    public void Refresh()
    {
        var snapshot = _core.GetPlayers();

        // Remove stale
        for (int i = Players.Count - 1; i >= 0; --i)
        {
            if (!snapshot.Any(p => p.id == Players[i].Id))
                Players.RemoveAt(i);
        }

        foreach (var p in snapshot)
        {
            var row = Players.FirstOrDefault(r => r.Id == p.id);
            if (row == null)
            {
                row = new PlayerRow
                {
                    Id = p.id,
                    Name = p.name ?? "",
                    Model = p.model ?? "",
                };
                Players.Add(row);
            }
            row.Address = p.address ?? "";
            row.PingMs  = p.ping_ms;
            row.IsHost  = p.is_host != 0;
            row.IdleMs  = p.idle_ms;
            row.AnimationId  = p.last_animation_id;
            row.PostureFlags = p.last_posture_flags;
        }
    }

    [RelayCommand]
    private void Kick(PlayerRow? row)
    {
        if (row == null) return;
        _core.Kick(row.Id, "Kicked by admin");
    }
}
