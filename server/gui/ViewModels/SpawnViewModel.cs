using System.Collections.ObjectModel;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using KenshiMP.Server.Interop;

namespace KenshiMP.Server.ViewModels;

public partial class SpawnedNpcRow : ObservableObject
{
    [ObservableProperty] private uint   _id;
    [ObservableProperty] private string _name = "";
    [ObservableProperty] private string _race = "";
    [ObservableProperty] private float  _x, _y, _z;
    public string Position => $"({X:F1}, {Y:F1}, {Z:F1})";
    partial void OnXChanged(float v) => OnPropertyChanged(nameof(Position));
    partial void OnYChanged(float v) => OnPropertyChanged(nameof(Position));
    partial void OnZChanged(float v) => OnPropertyChanged(nameof(Position));
}

public partial class SpawnedBuildingRow : ObservableObject
{
    [ObservableProperty] private uint   _id;
    [ObservableProperty] private string _stringID = "";
    [ObservableProperty] private float  _x, _y, _z;
    [ObservableProperty] private short  _floor;
    public string Position => $"({X:F1}, {Y:F1}, {Z:F1})";
    partial void OnXChanged(float v) => OnPropertyChanged(nameof(Position));
    partial void OnYChanged(float v) => OnPropertyChanged(nameof(Position));
    partial void OnZChanged(float v) => OnPropertyChanged(nameof(Position));
}

public partial class SpawnViewModel : ViewModelBase
{
    private readonly ServerCore _core;

    // --- NPC form --------------------------------------------------------------
    [ObservableProperty] private string _npcName   = "Test Dummy";
    [ObservableProperty] private string _npcRace   = "Greenlander";
    [ObservableProperty] private string _npcWeapon = "";
    [ObservableProperty] private string _npcArmour = "";
    [ObservableProperty] private float  _npcX = 0f;
    [ObservableProperty] private float  _npcY = 0f;
    [ObservableProperty] private float  _npcZ = 0f;
    [ObservableProperty] private float  _npcYaw = 0f;
    // Default off — most uses are "spawn a static target", enable for real AI.
    [ObservableProperty] private bool   _npcEnableAi = false;

    // --- Building form ---------------------------------------------------------
    // Example stringID of a base-game wall segment (seen in sync logs).
    // Format is "{internal_id}-{source_file}". Check the server log while a
    // host is connected and you'll see real ones streaming past.
    [ObservableProperty] private string _bldStringID = "16851-gamedata.base";
    [ObservableProperty] private float  _bldX = 0f;
    [ObservableProperty] private float  _bldY = 0f;
    [ObservableProperty] private float  _bldZ = 0f;
    [ObservableProperty] private float  _bldQw = 1f;
    [ObservableProperty] private float  _bldQx = 0f;
    [ObservableProperty] private float  _bldQy = 0f;
    [ObservableProperty] private float  _bldQz = 0f;
    [ObservableProperty] private bool   _bldCompleted = true;
    [ObservableProperty] private bool   _bldIsFoliage = false;
    [ObservableProperty] private short  _bldFloor = 0;

    [ObservableProperty] private string _status = "";

    public ObservableCollection<SpawnedNpcRow>      Npcs      { get; } = new();
    public ObservableCollection<SpawnedBuildingRow> Buildings { get; } = new();

    // Catalog of building types published by the connected host.
    public ObservableCollection<CatalogItem> BuildingCatalog { get; } = new();
    [ObservableProperty] private CatalogItem? _selectedCatalogEntry;
    partial void OnSelectedCatalogEntryChanged(CatalogItem? v)
    {
        if (v != null) BldStringID = v.StringID;
    }

    public SpawnViewModel(ServerCore core) { _core = core; }

    [RelayCommand]
    private void SpawnNpc()
    {
        uint id = _core.SpawnNpc(NpcName, NpcRace, NpcWeapon, NpcArmour,
                                  NpcX, NpcY, NpcZ, NpcYaw, NpcEnableAi);
        Status = id != 0 ? $"Spawned NPC {id}" : "Spawn failed";
        Refresh();
    }

    [RelayCommand]
    private void SpawnBuilding()
    {
        uint id = _core.SpawnBuilding(BldStringID, BldX, BldY, BldZ,
                                       BldQw, BldQx, BldQy, BldQz,
                                       BldCompleted, BldIsFoliage, BldFloor);
        Status = id != 0 ? $"Spawned Building {id}" : "Spawn failed";
        Refresh();
    }

    // Pull the host's current position out of the players snapshot and copy
    // it into whichever form's coord fields the caller wants.
    private bool TryGetHostPos(out float x, out float y, out float z)
    {
        x = y = z = 0f;
        foreach (var p in _core.GetPlayers())
        {
            if (p.is_host != 0) { x = p.x; y = p.y; z = p.z; return true; }
        }
        return false;
    }

    [RelayCommand]
    private void UseHostPosNpc()
    {
        if (TryGetHostPos(out var x, out var y, out var z))
        {
            NpcX = x; NpcY = y; NpcZ = z;
            Status = $"NPC pos set to host ({x:F0},{y:F0},{z:F0})";
        }
        else Status = "No host connected";
    }

    [RelayCommand]
    private void UseHostPosBuilding()
    {
        if (TryGetHostPos(out var x, out var y, out var z))
        {
            BldX = x; BldY = y; BldZ = z;
            Status = $"Building pos set to host ({x:F0},{y:F0},{z:F0})";
        }
        else Status = "No host connected";
    }

    [RelayCommand]
    private void DespawnNpc(SpawnedNpcRow? row)
    {
        if (row == null) return;
        _core.DespawnNpc(row.Id);
        Refresh();
    }

    [RelayCommand]
    private void DespawnBuilding(SpawnedBuildingRow? row)
    {
        if (row == null) return;
        _core.DespawnBuilding(row.Id);
        Refresh();
    }

    public class CatalogItem
    {
        public string StringID { get; init; } = "";
        public string Name     { get; init; } = "";
        public string Display  => string.IsNullOrEmpty(Name) ? StringID : $"{Name}  [{StringID}]";
        public override string ToString() => Display;
    }

    public void Refresh()
    {
        var ns = _core.ListSpawnedNpcs();
        for (int i = Npcs.Count - 1; i >= 0; --i)
            if (!ns.Any(n => n.id == Npcs[i].Id)) Npcs.RemoveAt(i);
        foreach (var n in ns)
        {
            var row = Npcs.FirstOrDefault(r => r.Id == n.id);
            if (row == null) { row = new SpawnedNpcRow { Id = n.id }; Npcs.Add(row); }
            row.Name = n.name ?? ""; row.Race = n.race ?? "";
            row.X = n.x; row.Y = n.y; row.Z = n.z;
        }

        var bs = _core.ListSpawnedBuildings();
        for (int i = Buildings.Count - 1; i >= 0; --i)
            if (!bs.Any(b => b.id == Buildings[i].Id)) Buildings.RemoveAt(i);
        foreach (var b in bs)
        {
            var row = Buildings.FirstOrDefault(r => r.Id == b.id);
            if (row == null) { row = new SpawnedBuildingRow { Id = b.id }; Buildings.Add(row); }
            row.StringID = b.stringID ?? "";
            row.X = b.x; row.Y = b.y; row.Z = b.z; row.Floor = b.floor;
        }

        // Refresh building catalog (rarely changes, but cheap to poll).
        var catalog = _core.ListBuildingCatalog();
        if (catalog.Count != BuildingCatalog.Count)
        {
            BuildingCatalog.Clear();
            foreach (var c in catalog.OrderBy(c => c.name))
                BuildingCatalog.Add(new CatalogItem { StringID = c.stringID ?? "", Name = c.name ?? "" });
        }
    }
}
