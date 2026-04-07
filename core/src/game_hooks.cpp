// game_hooks.cpp — KenshiLib accessor wrappers
//
// Provides game state access to other subsystems.
// All KenshiLib-specific code is isolated here.

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RootObjectFactory.h>

namespace kmp {

// ---------------------------------------------------------------------------
// Get the local player's primary Character.
// Returns nullptr if the game world isn't loaded yet or player has no characters.
// ---------------------------------------------------------------------------
Character* game_get_player_character() {
    if (!ou) return nullptr;
    if (!ou->player) return nullptr;

    const auto& chars = ou->player->getAllPlayerCharacters();
    if (chars.count <= 0) return nullptr;

    return chars.stuff[0];
}

// ---------------------------------------------------------------------------
// Get the RootObjectFactory for spawning NPCs.
// ---------------------------------------------------------------------------
RootObjectFactory* game_get_factory() {
    if (!ou) return nullptr;
    return ou->theFactory;
}

// ---------------------------------------------------------------------------
// Check if the game world is fully loaded and the player has characters.
// ---------------------------------------------------------------------------
bool game_is_world_loaded() {
    if (!ou) return false;
    if (!ou->player) return false;

    const auto& chars = ou->player->getAllPlayerCharacters();
    return chars.count > 0;
}

// ---------------------------------------------------------------------------
// Get the GameWorld pointer (for destroy calls, etc.)
// ---------------------------------------------------------------------------
GameWorld* game_get_world() {
    return ou;
}

} // namespace kmp
