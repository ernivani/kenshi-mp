// ServerCore.cs — high-level wrapper around NativeMethods.
// Keeps callback delegates pinned so the GC doesn't move them while the native
// worker thread is calling back. Marshals events to the UI thread via Dispatcher.

using System;
using System.Collections.Generic;
using Avalonia.Threading;
using KenshiMP.Server.Interop;

namespace KenshiMP.Server.Interop;

public record LogLine(int Level, ulong TimeMs, string Text);

public record CoreEvent(
    NativeMethods.kmp_event_type Type,
    uint PlayerId,
    ulong TimeMs,
    byte PostureOld,
    byte PostureNew,
    string Author,
    string Text);

public sealed class ServerCore : IDisposable
{
    // Hold references so delegates survive GC cycles.
    private readonly NativeMethods.LogCallback   _logCb;
    private readonly NativeMethods.EventCallback _eventCb;
    private bool _started;

    public event Action<LogLine>?   LogReceived;
    public event Action<CoreEvent>? EventReceived;

    public ServerCore()
    {
        _logCb = OnNativeLog;
        _eventCb = OnNativeEvent;
        NativeMethods.kmp_register_log_cb(_logCb, IntPtr.Zero);
        NativeMethods.kmp_register_event_cb(_eventCb, IntPtr.Zero);
    }

    public NativeMethods.kmp_server_config DefaultConfig()
    {
        var c = new NativeMethods.kmp_server_config();
        NativeMethods.kmp_default_config(ref c);
        return c;
    }

    public NativeMethods.kmp_server_config LoadConfig(string path)
    {
        var c = new NativeMethods.kmp_server_config();
        NativeMethods.kmp_load_config(path, ref c);
        return c;
    }

    public bool SaveConfig(string path, NativeMethods.kmp_server_config cfg)
        => NativeMethods.kmp_save_config(path, ref cfg) == 0;

    public bool Start(NativeMethods.kmp_server_config cfg)
    {
        if (_started) return true;
        var c = cfg;
        int rc = NativeMethods.kmp_server_start(ref c);
        _started = rc == 0;
        return _started;
    }

    public void Stop()
    {
        if (!_started) return;
        NativeMethods.kmp_server_stop();
        _started = false;
    }

    public bool IsRunning => NativeMethods.kmp_server_running() != 0;

    public IReadOnlyList<NativeMethods.kmp_player_info> GetPlayers(uint max = 128)
    {
        var buf = new NativeMethods.kmp_player_info[max];
        uint n = NativeMethods.kmp_get_players(buf, max);
        var list = new List<NativeMethods.kmp_player_info>((int)n);
        for (uint i = 0; i < n; ++i) list.Add(buf[i]);
        return list;
    }

    public NativeMethods.kmp_stats GetStats()
    {
        NativeMethods.kmp_get_stats(out var s);
        return s;
    }

    public void Kick(uint id, string reason) =>
        NativeMethods.kmp_kick(id, reason ?? string.Empty);

    public void BroadcastChat(string text) =>
        NativeMethods.kmp_broadcast_chat(text ?? string.Empty);

    public void InjectPosture(uint id, byte flags, bool sticky) =>
        NativeMethods.kmp_inject_posture(id, flags, sticky ? 1 : 0);

    public void ClearStickyPosture() =>
        NativeMethods.kmp_clear_sticky_posture();

    public bool StickyActive => NativeMethods.kmp_sticky_active() != 0;
    public uint StickyTarget => NativeMethods.kmp_sticky_target();
    public byte StickyFlags  => NativeMethods.kmp_sticky_flags();

    // --- Callback trampolines (native worker thread → UI thread) --------------

    private void OnNativeLog(int level, ulong timeMs, string text, IntPtr user)
    {
        var line = new LogLine(level, timeMs, text ?? string.Empty);
        Dispatcher.UIThread.Post(() => LogReceived?.Invoke(line));
    }

    private void OnNativeEvent(ref NativeMethods.kmp_event e, IntPtr user)
    {
        var ev = new CoreEvent(
            (NativeMethods.kmp_event_type)e.type,
            e.player_id, e.time_ms,
            e.posture_old, e.posture_new,
            e.author ?? string.Empty,
            e.text   ?? string.Empty);
        Dispatcher.UIThread.Post(() => EventReceived?.Invoke(ev));
    }

    // --- Spawn -----------------------------------------------------------------
    public uint SpawnNpc(string name, string race, string weapon, string armour,
                         float x, float y, float z, float yaw, bool enableAi)
    {
        var r = new NativeMethods.kmp_npc_spawn_request {
            name = name ?? "", race = race ?? "", weapon = weapon ?? "", armour = armour ?? "",
            x = x, y = y, z = z, yaw = yaw,
            enable_ai = (byte)(enableAi ? 1 : 0),
        };
        return NativeMethods.kmp_spawn_npc(ref r);
    }

    public uint SpawnBuilding(string stringID, float x, float y, float z,
                              float qw, float qx, float qy, float qz,
                              bool completed, bool isFoliage, short floor)
    {
        var r = new NativeMethods.kmp_building_spawn_request {
            stringID = stringID ?? "",
            x = x, y = y, z = z,
            qw = qw, qx = qx, qy = qy, qz = qz,
            completed = (byte)(completed ? 1 : 0),
            is_foliage = (byte)(isFoliage ? 1 : 0),
            floor = floor,
        };
        return NativeMethods.kmp_spawn_building(ref r);
    }

    public bool DespawnNpc(uint id)      => NativeMethods.kmp_despawn_npc(id) == 0;
    public bool DespawnBuilding(uint id) => NativeMethods.kmp_despawn_building(id) == 0;

    public IReadOnlyList<NativeMethods.kmp_npc_spawned> ListSpawnedNpcs(uint max = 256)
    {
        var buf = new NativeMethods.kmp_npc_spawned[max];
        uint n = NativeMethods.kmp_list_spawned_npcs(buf, max);
        var list = new List<NativeMethods.kmp_npc_spawned>((int)n);
        for (uint i = 0; i < n; ++i) list.Add(buf[i]);
        return list;
    }

    public IReadOnlyList<NativeMethods.kmp_building_catalog_item> ListBuildingCatalog(uint max = 4096)
    {
        var buf = new NativeMethods.kmp_building_catalog_item[max];
        uint n = NativeMethods.kmp_list_building_catalog(buf, max);
        var list = new List<NativeMethods.kmp_building_catalog_item>((int)n);
        for (uint i = 0; i < n; ++i) list.Add(buf[i]);
        return list;
    }

    public IReadOnlyList<NativeMethods.kmp_building_spawned> ListSpawnedBuildings(uint max = 256)
    {
        var buf = new NativeMethods.kmp_building_spawned[max];
        uint n = NativeMethods.kmp_list_spawned_buildings(buf, max);
        var list = new List<NativeMethods.kmp_building_spawned>((int)n);
        for (uint i = 0; i < n; ++i) list.Add(buf[i]);
        return list;
    }

    public void Dispose() => Stop();
}
