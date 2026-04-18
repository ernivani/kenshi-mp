# KenshiMP

Multiplayer mod for [Kenshi](https://store.steampowered.com/app/233860/Kenshi/).
One player hosts, others join.

## What's in this zip

| File | Purpose |
| ---- | ------- |
| `KenshiMP.dll`          | The Kenshi plugin. Loaded at game start by RE_Kenshi. |
| `kenshi-mp-server.exe`  | Dedicated server with admin GUI. |
| `RE_Kenshi.json`        | Tells RE_Kenshi to load `KenshiMP.dll`. |
| `KenshiMP.mod`          | Placeholder .mod file so Kenshi's launcher lists the mod. |

## Prerequisites

1. Kenshi (Steam).
2. RE_Kenshi: https://github.com/BFrizzleFoShizzle/RE_Kenshi/releases
3. Windows 10/11 x64.

## Installation

1. Install RE_Kenshi.
2. Open your Kenshi `mods/` folder. Default Steam install:

   ```
   C:\Program Files (x86)\Steam\steamapps\common\Kenshi\mods\
   ```

3. Copy this unzipped folder into `mods/`:

   ```
   Kenshi\mods\KenshiMP\
       KenshiMP.dll
       KenshiMP.mod
       RE_Kenshi.json
       kenshi-mp-server.exe
   ```

4. Start **Kenshi**, Go into the Mods tab and select **KenshiMP**.

## Hosting

1. Start **Kenshi** with the mod enabled.

2. Start a **NEW GAME** or load a previous save.

3. Press **F8** to open the connect dialog.

4. Click the **HOST** button.

5. The server will start and you will be able to join.


## Joining

1. Start **Kenshi** with the mod enabled.

2. Create a new character using the **NEW GAME** to have a brand new character.

3. Press **F8** to open the connect dialog.

4. Click the **JOIN** button.

5. Enter the host's IP and port using the inputs Box and click **CONNECT**.


## Links

- Source: https://github.com/ernivani/kenshi-mp
- RE_Kenshi: https://github.com/BFrizzleFoShizzle/RE_Kenshi/releases
- Kenshi on Steam: https://store.steampowered.com/app/233860/Kenshi/
- Bugs: https://github.com/ernivani/kenshi-mp/issues

## Licence

See LICENSE in the source repo.
