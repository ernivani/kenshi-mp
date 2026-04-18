using CommunityToolkit.Mvvm.ComponentModel;

namespace KenshiMP.Server.ViewModels;

public partial class PlayerRow : ObservableObject
{
    [ObservableProperty] private uint   _id;
    [ObservableProperty] private string _name    = "";
    [ObservableProperty] private string _model   = "";
    [ObservableProperty] private string _address = "";
    [ObservableProperty] private uint   _pingMs;
    [ObservableProperty] private bool   _isHost;
    [ObservableProperty] private uint   _idleMs;
    [ObservableProperty] private uint   _animationId;
    [ObservableProperty] private byte   _postureFlags;

    public string PostureLabel => KenshiMP.Server.Interop.PostureFlags.Label(PostureFlags);
    public string IdleSeconds  => $"{IdleMs / 1000.0:F1}s";
    public string AnimHex      => $"0x{AnimationId:X8}";

    partial void OnPostureFlagsChanged(byte value) => OnPropertyChanged(nameof(PostureLabel));
    partial void OnIdleMsChanged(uint value)       => OnPropertyChanged(nameof(IdleSeconds));
    partial void OnAnimationIdChanged(uint value)  => OnPropertyChanged(nameof(AnimHex));
}
