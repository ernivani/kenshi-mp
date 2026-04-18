using System;
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using KenshiMP.Server.Interop;

namespace KenshiMP.Server.ViewModels;

public class ChatEntry
{
    public string Author  { get; init; } = "";
    public string Text    { get; init; } = "";
    public string Time    { get; init; } = "";
    public bool   IsServer { get; init; }
}

public partial class ChatViewModel : ViewModelBase
{
    private readonly ServerCore _core;

    public ObservableCollection<ChatEntry> Entries { get; } = new();

    [ObservableProperty] private string _input = "";

    public ChatViewModel(ServerCore core)
    {
        _core = core;
        core.EventReceived += e => {
            if (e.Type != NativeMethods.kmp_event_type.ChatMessage) return;
            var dt = DateTimeOffset.FromUnixTimeMilliseconds((long)e.TimeMs).LocalDateTime;
            Entries.Add(new ChatEntry {
                Author = e.PlayerId == 0 ? "<server>" : e.Author,
                Text   = e.Text,
                Time   = dt.ToString("HH:mm:ss"),
                IsServer = e.PlayerId == 0,
            });
            while (Entries.Count > 512) Entries.RemoveAt(0);
        };
    }

    [RelayCommand]
    private void Send()
    {
        if (string.IsNullOrWhiteSpace(Input)) return;
        _core.BroadcastChat(Input);
        Input = "";
    }
}
