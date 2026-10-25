#include "game/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace gvr {

bool loadSettings(const std::string& path, UserSettings& out) {
    std::ifstream f(path);
    if (!f) return false;
    UserSettings s;
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const int v = std::atoi(line.c_str() + eq + 1);
        if (k == "turn") s.turn = std::max(0, std::min(3, v));
        else if (k == "left_handed") s.leftHanded = v != 0;
        else if (k == "third_person") s.thirdPerson = v != 0;
        else if (k == "hud_size") s.hudSize = std::max(0, std::min(2, v));
        else if (k == "volume") s.volume = std::max(0, std::min(10, v));
        else if (k == "music") s.music = std::max(0, std::min(10, v));
        else if (k == "subtitles") s.subtitles = v != 0;
        else if (k == "cutscene_screen") s.cutsceneScreen = v != 0;
        else if (k == "show_fps") s.showFps = v != 0;
        else if (k == "day_night") s.dayNight = v != 0;
        else if (k == "original_fx") s.originalFx = v != 0;
        else if (k == "foveation") s.foveation = std::max(0, std::min(3, v));
        else if (k == "vignette") s.vignette = std::max(0, std::min(2, v));
        else if (k == "ambushes") s.ambushes = v != 0;
        else if (k == "aim_laser") s.aimLaser = v != 0;
        else if (k == "classic_look") s.classicLook = v != 0;
    }
    out = s;
    return true;
}

bool writeSettings(const std::string& path, const UserSettings& s) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << "turn=" << s.turn << "\nleft_handed=" << int(s.leftHanded) << "\nthird_person=" << int(s.thirdPerson)
          << "\nhud_size=" << s.hudSize << "\nvolume=" << s.volume << "\nmusic=" << s.music << "\nsubtitles=" << int(s.subtitles) << "\ncutscene_screen=" << int(s.cutsceneScreen) << "\nshow_fps=" << int(s.showFps) << "\nday_night=" << int(s.dayNight) << "\noriginal_fx=" << int(s.originalFx) << "\nfoveation=" << s.foveation << "\nvignette=" << s.vignette << "\nambushes=" << int(s.ambushes) << "\naim_laser=" << int(s.aimLaser) << "\nclassic_look=" << int(s.classicLook) << "\n";
        if (!f.good()) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace gvr
