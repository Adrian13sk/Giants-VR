// Scripted on-device test runs ("autotest"): a small text script that picks a world and mission,
// places the player, then drives the controllers on a timeline - the same idea as the headless
// harness's flags, but in seconds so it works at whatever frame rate the headset runs.
//
// The Quest build looks for <app files>/autotest.txt at launch; scripts/devtest.ps1 pushes one,
// starts the app and pulls the results (frame times, GPU time, screenshots) when it's done.
//
// Script lines ("#" starts a comment; times in seconds from the start of play):
//   name <id>                        results are named after it
//   world <World> / mission <name>   e.g. world Story1, mission 1a_... (GVR_MISSION)
//   env <VAR> <value>                any GVR_* test hook
//   view first|third                 camera (default first)
//   at <gx> <gy> / look <gx> <gy>    game coordinates to start at / face
//   pitch <deg>                      head pitch (default -8.6)
//   duration <s>                     run length (default 20)
//   perf <t0> <t1>                   window for the frame statistics (default 3 .. duration)
//   move <x> <y> <t0> <t1>           left stick
//   turn <x> <y> <t0> <t1>           right stick
//   hold <button> <t0> <t1>          trigger_r trigger_l grip_r grip_l a b x y lclick rclick menu
//   hand_r|hand_l <dx> <dy> <dz> <t0> <t1>   move that controller (tracking metres) over [t0, t1]
//   aim <yawDeg> <pitchDeg>          right controller direction (default 0, -17.2)
//   blast <gx> <gy> <h> <n> <t>      n rocket blasts at a point (as --blast-at)
//   reapers <n> <dist> <t> <tkill>   n Reapers in a row <dist> m in front of the player at t (facing
//                                    him), each blown up by a rocket blast at tkill (their drops, deaths)
//   shot <t> <label>                 left-eye screenshot at t
//   briefing                         reload the mission through the loading screen + briefing first
//   autoaim r|l <t0> <t1>            that controller points at the nearest living enemy, creature or boss
//   complete <t>                     the current mission is marked complete at t (campaign progression)
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/math.h"
#include "game/input_state.h"

namespace gvr {

class TestScript {
public:
    // Parse; false with `error` set on a line it doesn't understand.
    bool parse(const std::string& text, std::string& error);

    std::string name = "test", world = "Story1", mission;
    std::vector<std::pair<std::string, std::string>> env;
    bool firstPerson = true;
    bool hasAt = false, hasLook = false;
    float atX = 0, atY = 0, lookX = 0, lookY = 0;
    float pitchDeg = -8.6f, aimYawDeg = 0.0f, aimPitchDeg = -17.2f;
    float duration = 20.0f, perfFrom = 3.0f, perfTo = -1.0f;  // perfTo < 0: the end
    bool title = false;     // (2026-10-27) start at the main menu (title scene) instead of in the mission
    bool briefing = false;  // start through the loading screen and briefing (as the game does); `at` is ignored

    struct Span {
        std::string what;  // "move", "turn", a button name, "hand_r", "hand_l"
        float x = 0, y = 0, z = 0, t0 = 0, t1 = 0;
    };
    std::vector<Span> spans;
    struct Blast { float x = 0, y = 0, h = 1; int n = 1; float t = 0; };
    std::vector<Blast> blasts;
    struct ReaperEvent { float t = 0; int n = 0; float dist = 0; bool kill = false; };
    std::vector<ReaperEvent> reaperEvents;
    std::vector<const ReaperEvent*> reapersIn(float t0, float t1) const;
    struct Shot { float t = 0; std::string label; };
    std::vector<Shot> shots;
    std::vector<float> completes;
    // Bit 0: the right controller auto-aims at t; bit 1: the left.
    int autoAimAt(float t) const;
    // Completions whose time falls in (t0, t1].
    int completesIn(float t0, float t1) const;

    // The start yaw (radians, runtime frame) from at/look.
    float startYaw() const;
    // Head, eye and controller poses of a standing user (tracking space), as the harness uses.
    Pose headPose() const;
    Pose eyePose(int eye, float ipd = 0.064f) const;
    // The input at time t: the scripted poses, sticks and buttons.
    void inputAt(float t, InputFrame& in) const;
    // Blasts and shots whose time falls in (t0, t1].
    std::vector<const Blast*> blastsIn(float t0, float t1) const;
    std::vector<const Shot*> shotsIn(float t0, float t1) const;
    float perfEnd() const { return perfTo < 0 ? duration : perfTo; }
};

}  // namespace gvr
