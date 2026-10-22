#include "game/smartie.h"

#include <algorithm>
#include <cmath>

namespace gvr {

float RescueDirector::rnd() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return float(rng_ >> 8) / 16777216.0f;
}

int RescueDirector::add(const Vec3& feet, float yaw, float height) {
    RescueSmartie s;
    s.home = s.pos = feet;
    s.yaw = yaw;
    s.height = height;
    smarties.push_back(s);
    return int(smarties.size()) - 1;
}

void RescueDirector::hangAgain(RescueSmartie& s) {
    s.state = SmartieState::Hanging;
    s.stateTime = 0;
    s.vel = {0, 0, 0};
    s.pos = s.hangPoint - Vec3{0, s.height, 0};
    s.holdFor = holdMin + (holdMax - holdMin) * rnd();
}

void RescueDirector::setHang(int index, const Vec3& hands) {
    RescueSmartie& s = smarties[size_t(index)];
    s.hangPoint = hands;
    s.hangs = true;
    hangAgain(s);
    // Staggered: the first ones give out after holdMin, the rest spread up to holdMax.
}

size_t RescueDirector::savedCount() const {
    size_t n = 0;
    for (const auto& s : smarties) n += s.state == SmartieState::Saved;
    return n;
}

bool RescueDirector::holding(int hand) const {
    for (const auto& s : smarties)
        if (s.state == SmartieState::Carried && s.hand == hand) return true;
    return false;
}

bool RescueDirector::inPad(const Vec3& p) const {
    return hasPad && p.x >= padMin.x && p.x <= padMax.x && p.z >= padMin.z && p.z <= padMax.z && p.y <= padMax.y + 0.5f;
}

void RescueDirector::update(float dt, const Vec3 hands[2], const float grip[2], const Vec3& playerPos,
                            std::vector<SmartieEvent>& events) {
    Vec3 handVel[2] = {{0, 0, 0}, {0, 0, 0}};
    if (havePrev_ && dt > 1e-4f)
        for (int h = 0; h < 2; ++h) handVel[h] = (hands[h] - prevHand_[h]) * (1.0f / dt);
    for (int h = 0; h < 2; ++h) prevHand_[h] = hands[h];
    havePrev_ = true;
    auto ground = [&](float x, float z) { return groundHeight ? groundHeight(x, z) : 0.0f; };

    for (int h = 0; h < 2; ++h) {
        const bool held = grip[h] > (gripHeld_[h] ? 0.35f : 0.6f);  // hysteresis
        const bool pressed = held && !gripHeld_[h], released = !held && gripHeld_[h];
        gripHeld_[h] = held;
        if (pressed && !holding(h)) {
            // Nearest Smartie within reach of this hand.
            int best = -1;
            float bestD = 1e9f;
            for (size_t i = 0; i < smarties.size(); ++i) {
                const RescueSmartie& s = smarties[i];
                const bool hanging = s.state == SmartieState::Hanging;
                if (s.state != SmartieState::Idle && s.state != SmartieState::Falling && !hanging) continue;
                const float dx = hands[h].x - s.pos.x, dz = hands[h].z - s.pos.z;
                const float horiz = std::sqrt(dx * dx + dz * dz);
                const float above = hands[h].y - (s.pos.y + s.height);
                if (horiz > reach) continue;
                if (hanging ? (above > 0.4f || hands[h].y < s.pos.y - 0.4f) : (above > reachAbove || hands[h].y < s.pos.y - 0.3f))
                    continue;
                const float d = horiz + std::max(0.0f, above) * 0.5f;
                if (d < bestD) { bestD = d; best = int(i); }
            }
            if (best >= 0) {
                RescueSmartie& s = smarties[size_t(best)];
                s.state = SmartieState::Carried;
                s.hand = h;
                s.stateTime = 0;
                events.push_back({SmartieEvent::Grabbed, best, h, s.pos});
            }
        }
        if (released)
            for (size_t i = 0; i < smarties.size(); ++i) {
                RescueSmartie& s = smarties[i];
                if (s.state != SmartieState::Carried || s.hand != h) continue;
                s.state = SmartieState::Falling;
                s.stateTime = 0;
                // Thrown with the hand's motion (capped: a gentle toss, not a launch).
                Vec3 v = handVel[h];
                const float sp = length(v);
                if (sp > 6.0f) v = v * (6.0f / sp);
                s.vel = v;
                s.hand = -1;
                events.push_back({SmartieEvent::Released, int(i), h, s.pos});
            }
    }

    for (size_t i = 0; i < smarties.size(); ++i) {
        RescueSmartie& s = smarties[i];
        s.stateTime += dt;
        switch (s.state) {
            case SmartieState::Hanging: {
                // Turns its face toward the player; holds on with both hands, then one, then lets go.
                const Vec3 d = playerPos - s.pos;
                if (d.x * d.x + d.z * d.z > 1e-4f) s.yaw = std::atan2(-d.x, -d.z);
                const bool slipping = s.stateTime > s.holdFor - warnTime;
                if (slipping && s.clip != SmartieClip::HangOneArm) events.push_back({SmartieEvent::Slipping, int(i), -1, s.pos});
                s.clip = slipping ? SmartieClip::HangOneArm : SmartieClip::Hang;
                if (s.stateTime > s.holdFor) {
                    s.state = SmartieState::Falling;
                    s.stateTime = 0;
                    s.vel = {0, 0, 0};
                    events.push_back({SmartieEvent::LetGo, int(i), -1, s.pos});
                }
                break;
            }
            case SmartieState::Idle: {
                // Faces the player; waves when the player is near enough to be seen.
                const Vec3 d = playerPos - s.pos;
                if (d.x * d.x + d.z * d.z > 1e-4f) {
                    const float want = std::atan2(-d.x, -d.z);
                    float diff = want - s.yaw;
                    while (diff > kPi) diff -= 2 * kPi;
                    while (diff < -kPi) diff += 2 * kPi;
                    s.yaw += diff * std::min(1.0f, dt * 3.0f);
                }
                const float dist = length(d);
                s.clip = dist < 25.0f && dist > 2.0f ? SmartieClip::Wave : SmartieClip::Stand;
                if (inPad(s.pos)) {  // e.g. carried straight onto the pad and let go at ground level
                    s.state = SmartieState::Saved;
                    s.stateTime = 0;
                    events.push_back({SmartieEvent::Saved, int(i), -1, s.pos});
                }
                break;
            }
            case SmartieState::Carried: {
                // Held by the scruff: the head just below the hand, a little away from the player
                // so it hangs in view instead of in the player's face.
                const Vec3 hp = hands[s.hand];
                Vec3 away{hp.x - playerPos.x, 0, hp.z - playerPos.z};
                const float al = length(away);
                away = al > 1e-3f ? away * (0.3f / al) : Vec3{0, 0, 0};
                s.pos = hp + away - Vec3{0, s.height * 0.85f, 0};
                const Vec3 d = playerPos - s.pos;
                if (d.x * d.x + d.z * d.z > 1e-4f) s.yaw = std::atan2(-d.x, -d.z);
                s.clip = SmartieClip::Squirm;
                s.vel = handVel[s.hand];
                break;
            }
            case SmartieState::Falling: {
                s.vel.y -= 9.8f * dt;
                s.pos += s.vel * dt;
                s.clip = SmartieClip::Fall;
                const float g = ground(s.pos.x, s.pos.z);
                const bool water = g < seaLevel - 0.15f;
                const float floor = water ? seaLevel - 0.6f : g;  // in water: up to the chest
                if (s.pos.y <= floor) {
                    s.pos.y = floor;
                    s.vel = {0, 0, 0};
                    s.stateTime = 0;
                    if (water) {
                        s.state = SmartieState::Drowning;
                        events.push_back({SmartieEvent::Splashed, int(i), -1, s.pos});
                    } else if (inPad(s.pos)) {
                        s.state = SmartieState::Saved;
                        events.push_back({SmartieEvent::Saved, int(i), -1, s.pos});
                    } else {
                        s.state = SmartieState::Idle;
                        events.push_back({SmartieEvent::Landed, int(i), -1, s.pos});
                    }
                }
                break;
            }
            case SmartieState::Drowning:
                // The piranhas: it thrashes, then is pulled under.
                s.clip = SmartieClip::Drowned;
                if (s.stateTime > 2.0f) s.pos.y -= 0.6f * dt;
                if (s.stateTime > 4.0f) {
                    s.state = SmartieState::Gone;
                    s.stateTime = 0;
                    events.push_back({SmartieEvent::Died, int(i), -1, s.pos});
                }
                break;
            case SmartieState::Gone:
                if (s.stateTime > respawnTime) {
                    if (s.hangs) hangAgain(s);
                    else { s.state = SmartieState::Idle; s.pos = s.home; s.stateTime = 0; }
                    events.push_back({SmartieEvent::Returned, int(i), -1, s.pos});
                }
                break;
            case SmartieState::Saved:
                s.clip = SmartieClip::Celebrate;
                break;
        }
    }
}

}  // namespace gvr
