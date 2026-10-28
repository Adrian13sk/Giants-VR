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
    bool cutsceneScreen = true;  // rework: cutscenes on a cinema screen (the original framing); false: around you
    bool showFps = false;    // frame-rate counter at the top of the view
    bool dayNight = true;    // false: always day
    bool originalFx = false;  // effects from the game's fx.bin (experimental) instead of our particles
    bool classicLook = false; // rework: the classic lighting instead of the rework's (to compare)
    bool aimLaser = true;    // rework: a red laser from the gun's muzzle
    bool ambushes = true;    // rework: Reaper squads come at you after a long quiet stretch
    int vignette = 1;        // comfort tunnel while moving / turning: 0 off, 1 low, 2 high
    int hintsShown = 0;      // rework: one-time hand-mechanics hints already shown (bits, game_app_hints.cpp)
    int ambience = 2;        // rework: 0 off, 1 the ambient sounds (shore, wind, leaves, river, volcano), 2 and the weather (rain, snow)
    bool reachAssist = true; // rework: ground pickups (stones, bodies, Smarties) reachable from a stoop, not only a crouch
    bool micRoar = false;    // (2026-10-28) Kabuto roars when you roar into the headset's microphone (asks for the permission when turned on)
    int foveation = 2;       // fixed foveated rendering 0 off .. 3 high (2: measured on the Quest 2 to hold 72 fps)
    float hudScale() const { return hudSize == 0 ? 0.8f : (hudSize == 2 ? 1.3f : 1.0f); }
};

bool loadSettings(const std::string& path, UserSettings& out);
bool writeSettings(const std::string& path, const UserSettings& s);

}  // namespace gvr
