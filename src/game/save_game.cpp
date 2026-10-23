#include "game/save_game.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gvr {

bool loadSaveGame(const std::string& path, SaveGame& out) {
    std::ifstream f(path);
    if (!f) return false;
    SaveGame s;
    std::string line;
    bool versionOk = false;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "version") versionOk = v == "1";
        else if (k == "world") s.world = v;
        else if (k == "mission") s.mission = v;
        else if (k == "syringes") s.syringes = std::atoi(v.c_str());
        else if (k == "launcher") s.launcher = v == "1";
    }
    if (!versionOk || s.world.empty() || s.mission.empty()) return false;
    out = s;
    return true;
}

bool writeSaveGame(const std::string& path, const SaveGame& s) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << "version=1\nworld=" << s.world << "\nmission=" << s.mission << "\nsyringes=" << s.syringes
          << "\nlauncher=" << (s.launcher ? 1 : 0) << "\n";
        if (!f.good()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace gvr
