using System;
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using KenshiMP.Server.Interop;

namespace KenshiMP.Server.ViewModels;

public partial class LogEntry : ObservableObject
{
    public int    Level { get; init; }
    public string Time  { get; init; } = "";
    public string Text  { get; init; } = "";

    public string LevelTag => Level switch {
        0 => "T", 1 => "D", 2 => "I", 3 => "W", 4 => "E", 5 => "C", _ => "?"
    };
    public string Color => Level switch {
        1 => "#888",   // debug
        3 => "#e5c35c",// warn
        4 or 5 => "#e55a5a",
        _ => "#ddd"
    };
}

public partial class LogViewModel : ViewModelBase
{
    private const int MaxEntries = 4096;

    public ObservableCollection<LogEntry> Entries { get; } = new();

    [ObservableProperty] private bool _showDebug = true;
    [ObservableProperty] private bool _showInfo  = true;
    [ObservableProperty] private bool _showWarn  = true;
    [ObservableProperty] private bool _showError = true;
    [ObservableProperty] private string _filter = "";
    [ObservableProperty] private bool _autoScroll = true;

    public LogViewModel(ServerCore core)
    {
        core.LogReceived += OnLog;
    }

    private void OnLog(LogLine line)
    {
        var dt = DateTimeOffset.FromUnixTimeMilliseconds((long)line.TimeMs).LocalDateTime;
        Entries.Add(new LogEntry {
            Level = line.Level,
            Time  = dt.ToString("HH:mm:ss.fff"),
            Text  = line.Text
        });
        while (Entries.Count > MaxEntries) Entries.RemoveAt(0);
    }
}
