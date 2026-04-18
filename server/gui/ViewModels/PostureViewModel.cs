using System;
using System.Collections.ObjectModel;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using KenshiMP.Server.Interop;

namespace KenshiMP.Server.ViewModels;

public class PostureTransitionEntry
{
    public string Time      { get; init; } = "";
    public uint   PlayerId  { get; init; }
    public string OldLabel  { get; init; } = "";
    public string NewLabel  { get; init; } = "";
}

public partial class PostureViewModel : ViewModelBase
{
    private readonly ServerCore _core;
    private readonly PlayersViewModel _playersVm;

    public ObservableCollection<PostureTransitionEntry> Transitions { get; } = new();

    [ObservableProperty] private PlayerRow? _target;
    [ObservableProperty] private bool _flagDown;
    [ObservableProperty] private bool _flagKo;
    [ObservableProperty] private bool _flagRag;
    [ObservableProperty] private bool _flagDead;
    [ObservableProperty] private bool _flagChained;
    [ObservableProperty] private bool _sticky;

    [ObservableProperty] private string _stickyLabel = "Sticky: inactive";

    public ObservableCollection<PlayerRow> Players => _playersVm.Players;

    public PostureViewModel(ServerCore core, PlayersViewModel playersVm)
    {
        _core = core;
        _playersVm = playersVm;
        core.EventReceived += e => {
            if (e.Type != NativeMethods.kmp_event_type.PostureTransition) return;
            var dt = DateTimeOffset.FromUnixTimeMilliseconds((long)e.TimeMs).LocalDateTime;
            Transitions.Add(new PostureTransitionEntry {
                Time     = dt.ToString("HH:mm:ss.fff"),
                PlayerId = e.PlayerId,
                OldLabel = Interop.PostureFlags.Label(e.PostureOld),
                NewLabel = Interop.PostureFlags.Label(e.PostureNew),
            });
            while (Transitions.Count > 512) Transitions.RemoveAt(0);
        };
    }

    private byte BuildFlags()
    {
        byte f = 0;
        if (FlagDown)    f |= Interop.PostureFlags.Down;
        if (FlagKo)      f |= Interop.PostureFlags.Unconscious;
        if (FlagRag)     f |= Interop.PostureFlags.Ragdoll;
        if (FlagDead)    f |= Interop.PostureFlags.Dead;
        if (FlagChained) f |= Interop.PostureFlags.Chained;
        return f;
    }

    [RelayCommand]
    private void Inject()
    {
        if (Target == null) return;
        _core.InjectPosture(Target.Id, BuildFlags(), Sticky);
        UpdateStickyLabel();
    }

    [RelayCommand]
    private void Clear()
    {
        _core.ClearStickyPosture();
        UpdateStickyLabel();
    }

    public void UpdateStickyLabel()
    {
        StickyLabel = _core.StickyActive
            ? $"Sticky: target={_core.StickyTarget} flags=0x{_core.StickyFlags:X2}"
            : "Sticky: inactive";
    }
}
