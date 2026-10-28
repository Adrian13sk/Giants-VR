#include "game/test_script.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace gvr {

namespace {
constexpr float kDeg = 3.14159265f / 180.0f;
const char* const kButtons[] = {"trigger_r", "trigger_l", "grip_r", "grip_l", "a", "b", "x", "y", "lclick", "rclick", "menu"};
bool isButton(const std::string& s) {
    for (const char* b : kButtons)
        if (s == b) return true;
    return false;
}
}  // namespace

bool TestScript::parse(const std::string& text, std::string& error) {
    std::istringstream lines(text);
    std::string line;
    int no = 0;
    while (std::getline(lines, line)) {
        ++no;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ss(line);
        std::string cmd;
        if (!(ss >> cmd)) continue;
        bool ok = true;
        auto fail = [&]() { error = "line " + std::to_string(no) + ": " + line; return false; };
        if (cmd == "name") ok = bool(ss >> name);
        else if (cmd == "world") ok = bool(ss >> world);
        else if (cmd == "mission") ok = bool(ss >> mission);
        else if (cmd == "env") { std::string k, v; ok = bool(ss >> k >> v); if (ok) env.emplace_back(k, v); }
        else if (cmd == "view") {
            std::string v;
            ok = bool(ss >> v) && (v == "first" || v == "third");
            firstPerson = v == "first";
        } else if (cmd == "at") { ok = bool(ss >> atX >> atY); hasAt = ok; }
        else if (cmd == "look") { ok = bool(ss >> lookX >> lookY); hasLook = ok; }
        else if (cmd == "pitch") ok = bool(ss >> pitchDeg);
        else if (cmd == "aim") ok = bool(ss >> aimYawDeg >> aimPitchDeg);
        else if (cmd == "duration") ok = bool(ss >> duration) && duration > 0;
        else if (cmd == "perf") ok = bool(ss >> perfFrom >> perfTo);
        else if (cmd == "briefing") briefing = true;
        else if (cmd == "title") title = true;
        else if (cmd == "move" || cmd == "turn") {
            Span s;
            s.what = cmd;
            ok = bool(ss >> s.x >> s.y >> s.t0 >> s.t1);
            if (ok) spans.push_back(s);
        } else if (cmd == "hold") {
            Span s;
            ok = bool(ss >> s.what >> s.t0 >> s.t1) && isButton(s.what);
            if (ok) spans.push_back(s);
        } else if (cmd == "hand_r" || cmd == "hand_l") {
            Span s;
            s.what = cmd;
            ok = bool(ss >> s.x >> s.y >> s.z >> s.t0 >> s.t1) && s.t1 > s.t0;
            if (ok) spans.push_back(s);
        } else if (cmd == "yaw_r" || cmd == "yaw_l") {
            // (2026-10-28) the controller turns about the vertical by x degrees over [t0, t1] and stays (+ = left)
            Span s;
            s.what = cmd;
            ok = bool(ss >> s.x >> s.t0 >> s.t1) && s.t1 > s.t0;
            if (ok) spans.push_back(s);
        } else if (cmd == "pitch_r" || cmd == "pitch_l") {
            // (rework) the controller pitches by x degrees over [t0, t1] and stays there (a wrist twist)
            Span s;
            s.what = cmd;
            ok = bool(ss >> s.x >> s.t0 >> s.t1) && s.t1 > s.t0;
            if (ok) spans.push_back(s);
        } else if (cmd == "blast") {
            Blast b;
            ok = bool(ss >> b.x >> b.y >> b.h >> b.n >> b.t);
            if (ok) blasts.push_back(b);
        } else if (cmd == "reapers") {
            ReaperEvent e;
            float tk = 0;
            ok = bool(ss >> e.n >> e.dist >> e.t >> tk) && e.n > 0 && e.n <= 12 && tk > e.t;
            if (ok) {
                reaperEvents.push_back(e);
                e.t = tk;
                e.kill = true;
                reaperEvents.push_back(e);
            }
        } else if (cmd == "autoaim") {
            Span s;
            std::string hand;
            ok = bool(ss >> hand >> s.t0 >> s.t1) && s.t1 > s.t0 && (hand == "r" || hand == "l");
            s.what = hand == "l" ? "autoaim_l" : "autoaim_r";
            if (ok) spans.push_back(s);
        } else if (cmd == "complete") {
            float t = 0;
            ok = bool(ss >> t);
            if (ok) completes.push_back(t);
        } else if (cmd == "shot") {
            Shot s;
            ok = bool(ss >> s.t >> s.label);
            if (ok) shots.push_back(s);
        } else {
            ok = false;
        }
        if (!ok) return fail();
    }
    return true;
}

float TestScript::startYaw() const {
    // Runtime yaw (0 = facing -Z): runtime x = -game x, runtime z = game y.
    if (!hasLook) return 0.0f;
    return std::atan2(lookX - atX, -(lookY - atY));
}

Pose TestScript::headPose() const {
    Pose p;
    p.position = {0, 1.65f, 0};
    p.orientation = Quat::fromAxisAngle({1, 0, 0}, pitchDeg * kDeg);
    return p;
}

Pose TestScript::eyePose(int eye, float ipd) const {
    Pose p = headPose();
    p.position = p.position + p.orientation.rotate(Vec3{(eye == 0 ? -0.5f : 0.5f) * ipd, 0, 0});
    return p;
}

void TestScript::inputAt(float t, InputFrame& in) const {
    in.headValid = true;
    in.head = headPose();
    for (int h = 0; h < 2; ++h) {
        ControllerState& c = in.hand[h];
        c = ControllerState{};
        c.active = c.gripPoseValid = c.aimPoseValid = true;
        const float side = h == kLeft ? -1.0f : 1.0f;
        c.gripPose.position = {0.2f * side, 1.2f, -0.35f};
        c.gripPose.orientation = Quat::fromAxisAngle({0, 1, 0}, aimYawDeg * kDeg) * Quat::fromAxisAngle({1, 0, 0}, aimPitchDeg * kDeg);
        c.aimPose = c.gripPose;
    }
    for (const Span& s : spans) {
        if (s.what == "yaw_r" || s.what == "yaw_l") {
            const float k = std::max(0.0f, std::min(1.0f, (t - s.t0) / (s.t1 - s.t0)));
            ControllerState& c = in.hand[s.what == "yaw_r" ? kRight : kLeft];
            c.gripPose.orientation = Quat::fromAxisAngle({0, 1, 0}, s.x * k * kDeg) * c.gripPose.orientation;
            c.aimPose.orientation = c.gripPose.orientation;
            continue;
        }
        if (s.what == "pitch_r" || s.what == "pitch_l") {
            const float k = std::max(0.0f, std::min(1.0f, (t - s.t0) / (s.t1 - s.t0)));
            ControllerState& c = in.hand[s.what == "pitch_r" ? kRight : kLeft];
            c.gripPose.orientation = c.gripPose.orientation * Quat::fromAxisAngle({1, 0, 0}, s.x * k * kDeg);
            c.aimPose.orientation = c.gripPose.orientation;
            continue;
        }
        if (s.what == "hand_r" || s.what == "hand_l") {
            // Cumulative: the controller moves by (x, y, z) over [t0, t1] and stays there.
            const float k = std::max(0.0f, std::min(1.0f, (t - s.t0) / (s.t1 - s.t0)));
            ControllerState& c = in.hand[s.what == "hand_r" ? kRight : kLeft];
            c.gripPose.position = c.gripPose.position + Vec3{s.x, s.y, s.z} * k;
            c.aimPose.position = c.gripPose.position;
            continue;
        }
        if (t < s.t0 || t >= s.t1) continue;
        if (s.what == "move") in.hand[kLeft].thumbstick = {s.x, s.y};
        else if (s.what == "turn") in.hand[kRight].thumbstick = {s.x, s.y};
        else if (s.what == "trigger_r") in.hand[kRight].trigger = 1.0f;
        else if (s.what == "trigger_l") in.hand[kLeft].trigger = 1.0f;
        else if (s.what == "grip_r") in.hand[kRight].squeeze = 1.0f;
        else if (s.what == "grip_l") in.hand[kLeft].squeeze = 1.0f;
        else if (s.what == "a") in.hand[kRight].primary = true;
        else if (s.what == "b") in.hand[kRight].secondary = true;
        else if (s.what == "x") in.hand[kLeft].primary = true;
        else if (s.what == "y") in.hand[kLeft].secondary = true;
        else if (s.what == "lclick") in.hand[kLeft].thumbstickClick = true;
        else if (s.what == "rclick") in.hand[kRight].thumbstickClick = true;
        else if (s.what == "menu") in.hand[kLeft].menu = true;
    }
}

int TestScript::autoAimAt(float t) const {
    int m = 0;
    for (const Span& s : spans)
        if (t >= s.t0 && t < s.t1) {
            if (s.what == "autoaim_r") m |= 1;
            else if (s.what == "autoaim_l") m |= 2;
        }
    return m;
}

int TestScript::completesIn(float t0, float t1) const {
    int n = 0;
    for (float c : completes) n += c > t0 && c <= t1;
    return n;
}

std::vector<const TestScript::Blast*> TestScript::blastsIn(float t0, float t1) const {
    std::vector<const Blast*> r;
    for (const auto& b : blasts)
        if (b.t > t0 && b.t <= t1) r.push_back(&b);
    return r;
}

std::vector<const TestScript::ReaperEvent*> TestScript::reapersIn(float t0, float t1) const {
    std::vector<const ReaperEvent*> r;
    for (const auto& e : reaperEvents)
        if (e.t > t0 && e.t <= t1) r.push_back(&e);
    return r;
}

std::vector<const TestScript::Shot*> TestScript::shotsIn(float t0, float t1) const {
    std::vector<const Shot*> r;
    for (const auto& s : shots)
        if (s.t > t0 && s.t <= t1) r.push_back(&s);
    return r;
}

}  // namespace gvr
