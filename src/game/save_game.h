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
};

bool loadSaveGame(const std::string& path, SaveGame& out);
bool writeSaveGame(const std::string& path, const SaveGame& s);

}  // namespace gvr
