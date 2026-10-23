// Player options set in the in-headset menu and kept between sessions (plain key=value file).
#pragma once

#include <string>

namespace gvr {

struct UserSettings {
    int turn = 0;            // 0 snap 30, 1 snap 45, 2 smooth slow, 3 smooth fast
    bool leftHanded = false;
    bool thirdPerson = false;
    int hudSize = 1;         // 0 small, 1 normal, 2 large
    int volume = 9;          // 0..10
    int music = 8;           // 0..10
    bool subtitles = true;
    bool showFps = false;    // frame-rate counter at the top of the view
    bool dayNight = true;    // false: always day
    float hudScale() const { return hudSize == 0 ? 0.8f : (hudSize == 2 ? 1.3f : 1.0f); }
};

bool loadSettings(const std::string& path, UserSettings& out);
bool writeSettings(const std::string& path, const UserSettings& s);

}  // namespace gvr
