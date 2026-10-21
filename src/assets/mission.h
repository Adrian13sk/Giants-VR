// Mission setup stream (wm_<name>.bin): a one-shot list of commands the original engine runs
// when a mission starts. It places the mission's objects and creatures and sets mission state.
// There is no control flow: runtime mission logic is native code in the original (not decoded).
//
//   u32 version = 4; { u8 op; fixed payload } ...; op 2 ends the stream.
// Payload sizes come from the original loader's cursor advances (decompilation used as a
// factual reference). Verified: all 56 shipped wm_*.bin files walk exactly to EOF.
// Field meanings beyond position/orientation/scale are provisional (see FORMATS.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gvr {

struct MissionObject {
    uint32_t type = 0;
    float x = 0, y = 0, z = 0;           // game units (Z up)
    float yaw = 0, pitch = 0, roll = 0;  // degrees
    float scale = 0;                      // 0 = not set (use the type's default)
    int team = -1;                        // op 5 (provisional name)
    uint8_t flags = 0;                    // op 8 (low 4 bits)
    int parent = -1;                      // index of the object this one is mounted on (op 0x16)
    uint8_t mount[2] = {0, 0};            // op 0x16 bytes (attachment point?)
    std::vector<std::string> flicks;      // op 0x14: cutscenes this object takes part in
};

struct MissionData {
    std::vector<MissionObject> objects;
    uint32_t setup[4] = {0, 0, 0, 0};     // op 0x0B: [0] player race (1 Kabuto, 2 Mecc, 3 Reaper)
    std::string startFlick;               // op 0x15
    std::string flick13, flick19;         // ops 0x13 / 0x19
    std::vector<uint32_t> items;          // op 0x0E: starting inventory
};

bool parseMission(const uint8_t* d, size_t n, MissionData& out, std::string* error = nullptr);

}  // namespace gvr
