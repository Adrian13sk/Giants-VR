// Cutscene shots (flk_<name>.bin): a bytecode timeline that drives a "director" animation
// (an embedded .anm image whose nodes move the actors and the camera), plus soundtrack and
// subtitle cues. Decoded from the original interpreter's cursor advances and field uses
// (decompiled FLK_FlickProcess / flick_file_load used as references; see FORMATS.md).
//   header: u32 version=3, u32 totalSize, u32 nDirectors, u32 nActors, u32 nBlocks,
//           u32 nextShotNameOffset (0 = last shot of the cutscene)
//   records from 0x18: {u32 op, u32 sizeInDwords, payload}; strings are file offsets.
// The port plays shots from a fixed viewpoint (user choice for VR comfort): the camera node's
// position at the start of the shot, head look free, fades between shots.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assets/skin_anim.h"

namespace gvr {

struct FlickActor {
    int index = -1;
    int director = -1;
    std::string node;          // director node that moves this actor ("timmy", "lander")
    int type = -1;             // object type (registry), -1 = none
    int parentActor = -1;      // op 3: attached to another actor's node
    std::string parentNode;    // e.g. "ref_lhand"
    std::string anim;          // op 3: the attached actor's own clip (e.g. "mcjetpacka1")
};

struct FlickShot {
    std::string next;                 // following shot, empty = cutscene ends
    bool hasDirector = false;
    AnmAnimation director;
    std::string cameraNode;           // op 4 (usually "camera01")
    std::vector<FlickActor> actors;
    std::string stream;               // op 0x34: soundtrack started by this shot ("f_start")
    std::vector<std::string> subtitles;  // op 0x10: GText keys shown during the shot
    float duration() const { return hasDirector ? director.durationSeconds() : 0.0f; }
};

bool parseFlick(const uint8_t* d, size_t n, FlickShot& out, std::string* error = nullptr);

}  // namespace gvr
