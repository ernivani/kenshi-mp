// game_hooks.cpp — KenshiLib accessor wrappers
//
// All functions run on the game thread (via AddHook), so no
// thread-safety guards are needed.

#include <kenshi/Globals.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RootObjectFactory.h>

namespace kmp {

Character* game_get_player_character() {
    if (!ou) return NULL;
    if (!ou->player) return NULL;

    const lektor<Character*>& chars = ou->player->getAllPlayerCharacters();
    if (chars.count <= 0) return NULL;

    return chars.stuff[0];
}

RootObjectFactory* game_get_factory() {
    if (!ou) return NULL;
    return ou->theFactory;
}

bool game_is_world_loaded() {
    if (!ou) return false;
    if (!ou->player) return false;

    const lektor<Character*>& chars = ou->player->getAllPlayerCharacters();
    return chars.count > 0;
}

GameWorld* game_get_world() {
    return ou;
}

} // namespace kmp
