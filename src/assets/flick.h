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
    bool placed = false;       // op 0x20: a free actor at a world position (game units) and yaw
    float px = 0, py = 0, pz = 0, pyaw = 0;
    // op 0x19 {actorIdx, nodeOff} + op 0x22 {actorIdx, 6, 5, f32 x0, y0, x1, y1}: read as the
    // actor following the ground (the node it is held up by, and a footprint rectangle in game
    // units around it). Inferred: every walking character in the Story 1 shots carries the pair,
    // props and vehicles don't, and our terrain leaves exactly those actors floating or sunk.
    bool groundFollow = false;
    std::string groundNode;
    float footMin[2] = {0, 0}, footMax[2] = {0, 0};
};

struct FlickShot {
    std::string next;                 // following shot, empty = cutscene ends
    bool hasDirector = false;
    AnmAnimation director;
    std::string cameraNode;           // op 4 (usually "camera01")
    std::vector<FlickActor> actors;
    std::string stream;               // op 0x34: soundtrack started by this shot ("f_start")
    std::vector<std::string> subtitles;  // op 0x10: GText keys shown during the shot
    // op 0x2C: a stream (voice clip, sound effect) started at a time in the shot; op 0x10 subtitles
    // likewise. Times come from the preceding op 9 waits, which name an absolute frame of the
    // shared Max timeline (story1_start_2 runs from frame 210; its "wait 265" = 1.83 s in).
    struct Cue { float time; std::string stream; };
    std::vector<Cue> cues;
    std::vector<float> subtitleTimes;  // per subtitles[i]
    float duration() const { return hasDirector ? director.durationSeconds() : 0.0f; }
};

bool parseFlick(const uint8_t* d, size_t n, FlickShot& out, std::string* error = nullptr);

}  // namespace gvr
