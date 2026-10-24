// Progress checkpoint: the story world and mission to continue from, and what the Meccaryn
// carries. Written when a mission starts (a checkpoint per mission), read at launch. Plain
// "key=value" text, written to a temporary file and renamed so a crash can't leave half a save.
#pragma once

#include <string>

namespace gvr {

struct SaveGame {
    std::string world, mission;
    int syringes = 0;
    bool launcher = false;
    int flares = 0;
    // The other guns (weapondefs items 3 shotgun, 5 machine gun, 6 sniper gun, 7 mortar) and their ammo.
    bool guns[4] = {false, false, false, false};
    int ammo[4] = {0, 0, 0, 0};
    int grenades = 0;
    // Delphi's bows (items 44 bow, 45 RPG bow, 46 sniper bow) and their arrows (-1: unlimited).
    bool bows[3] = {false, false, false};
    int arrows[3] = {-1, -1, -1};
};

bool loadSaveGame(const std::string& path, SaveGame& out);
bool writeSaveGame(const std::string& path, const SaveGame& s);

}  // namespace gvr
