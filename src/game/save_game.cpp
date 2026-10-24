#include "game/save_game.h"

#include <algorithm>
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
        else if (k == "flares") s.flares = std::atoi(v.c_str());
        else if (k == "grenades") s.grenades = std::max(0, std::atoi(v.c_str()));
        else if (k.size() == 4 && k.compare(0, 3, "bow") == 0 && k[3] >= '0' && k[3] <= '2') {
            const int b = k[3] - '0';
            s.bows[b] = true;
            s.arrows[b] = std::max(-1, std::atoi(v.c_str()));
        }
        else if (k.size() == 4 && k.compare(0, 3, "gun") == 0 && k[3] >= '0' && k[3] <= '3') {
            const int g = k[3] - '0';
            s.guns[g] = std::atoi(v.c_str()) >= 0;
            s.ammo[g] = std::max(0, std::atoi(v.c_str()));
        }
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
          << "\nlauncher=" << (s.launcher ? 1 : 0) << "\nflares=" << s.flares << "\n";
        // gunN=<ammo> for each gun carried (absent = not carried).
        if (s.grenades > 0) f << "grenades=" << s.grenades << "\n";
        for (int b = 0; b < 3; ++b)
            if (s.bows[b]) f << "bow" << b << "=" << std::max(-1, s.arrows[b]) << "\n";
        for (int g = 0; g < 4; ++g)
            if (s.guns[g]) f << "gun" << g << "=" << std::max(0, s.ammo[g]) << "\n";
        if (!f.good()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace gvr
