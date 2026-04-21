// NativeMethods.cs — P/Invoke surface for kenshi-mp-server-core.dll.
// Mirror of server/core/include/server_api.h.

using System;
using System.Runtime.InteropServices;

namespace KenshiMP.Server.Interop;

public static class NativeMethods
{
    private const string Dll = "kenshi-mp-server-core";

    // ---------- POD structs (pack=1 mirrors #pragma pack(push,1)) --------------

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct kmp_server_config
    {
        public ushort port;
        public uint   max_players;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string server_name;
        public float  view_distance;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct kmp_player_info
    {
        public uint   id;
        public byte   is_host;
        public uint   ping_ms;
        public uint   idle_ms;
        public float  x, y, z;
        public float  yaw;
        public float  speed;
        public uint   last_animation_id;
        public byte   last_posture_flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]  public string name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]  public string model;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]  public string address;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct kmp_stats
    {
        public ulong packets_in;
        public ulong packets_out;
        public ulong bytes_in;
        public ulong bytes_out;
        public uint  uptime_seconds;
        public uint  player_count;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct kmp_event
    {
        public int    type;
        public uint   player_id;
        public ulong  time_ms;
        public byte   posture_old;
        public byte   posture_new;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]  public string author;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string text;
    }

    public enum kmp_event_type
    {
        PlayerConnected    = 1,
        PlayerDisconnected = 2,
        ChatMessage        = 3,
        PostureTransition  = 4,
    }

    // ---------- Callback delegates ---------------------------------------------
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public delegate void LogCallback(int level, ulong time_ms,
        [MarshalAs(UnmanagedType.LPStr)] string text, IntPtr user);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void EventCallback(ref kmp_event evt, IntPtr user);

    // ---------- Lifecycle -------------------------------------------------------
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int kmp_server_start(ref kmp_server_config cfg);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void kmp_server_stop();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int kmp_server_running();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void kmp_register_log_cb(LogCallback cb, IntPtr user);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void kmp_register_event_cb(EventCallback cb, IntPtr user);

    // ---------- Snapshot --------------------------------------------------------
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern uint kmp_get_players(
        [Out] kmp_player_info[] outArr, uint max);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void kmp_get_stats(out kmp_stats stats);

    // ---------- Admin -----------------------------------------------------------
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern int kmp_kick(uint player_id,
        [MarshalAs(UnmanagedType.LPStr)] string reason);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern int kmp_broadcast_chat(
        [MarshalAs(UnmanagedType.LPStr)] string text);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int kmp_inject_posture(uint player_id, byte flags, int sticky);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void kmp_clear_sticky_posture();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int kmp_sticky_active();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern uint kmp_sticky_target();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern byte kmp_sticky_flags();

    // ---------- Config ----------------------------------------------------------
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern int kmp_save_config(
        [MarshalAs(UnmanagedType.LPStr)] string path,
        ref kmp_server_config cfg);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern int kmp_load_config(
        [MarshalAs(UnmanagedType.LPStr)] string path,
        ref kmp_server_config outCfg);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void kmp_default_config(ref kmp_server_config outCfg);

    // ---------- Spawn structs ---------------------------------------------------
    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct kmp_npc_spawn_request
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string race;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string weapon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string armour;
        public float x, y, z, yaw;
        public byte  enable_ai;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct kmp_npc_spawned
    {
        public uint id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string name;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string race;
        public float x, y, z, yaw;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct kmp_building_spawn_request
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string stringID;
        public float x, y, z;
        public float qw, qx, qy, qz;
        public byte  completed;
        public byte  is_foliage;
        public short floor;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct kmp_building_spawned
    {
        public uint id;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string stringID;
        public float x, y, z;
        public short floor;
        public byte  completed;
        public byte  is_foliage;
    }

    // ---------- Spawn DllImports -----------------------------------------------
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern uint kmp_spawn_npc(ref kmp_npc_spawn_request req);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern uint kmp_spawn_building(ref kmp_building_spawn_request req);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int kmp_despawn_npc(uint id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int kmp_despawn_building(uint id);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern uint kmp_list_spawned_npcs([Out] kmp_npc_spawned[] outArr, uint max);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern uint kmp_list_spawned_buildings([Out] kmp_building_spawned[] outArr, uint max);

    [StructLayout(LayoutKind.Sequential, Pack = 1, CharSet = CharSet.Ansi)]
    public struct kmp_building_catalog_item
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string stringID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string name;
    }

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern uint kmp_list_building_catalog([Out] kmp_building_catalog_item[] outArr, uint max);
}

// Posture flag constants (mirror of packets.h POSTURE_*).
public static class PostureFlags
{
    public const byte Down        = 0x01;
    public const byte Unconscious = 0x02;
    public const byte Ragdoll     = 0x04;
    public const byte Dead        = 0x08;
    public const byte Chained     = 0x10;

    public static string Label(byte flags)
    {
        if (flags == 0) return "CLEAR";
        var parts = new System.Collections.Generic.List<string>();
        if ((flags & Down)        != 0) parts.Add("DOWN");
        if ((flags & Unconscious) != 0) parts.Add("KO");
        if ((flags & Ragdoll)     != 0) parts.Add("RAG");
        if ((flags & Dead)        != 0) parts.Add("DEAD");
        if ((flags & Chained)     != 0) parts.Add("CHAIN");
        return string.Join("|", parts);
    }
}
