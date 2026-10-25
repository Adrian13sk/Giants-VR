// Name tags and Smartie lines. Smarties carry names built from the game's own name syllables
// (GTextEnglish smnamesyla/b/c: "Bor" + "fiz" + "zee"), shown over their heads when the player is
// close or looks at them; enemies show what they are (Ripper, Sea Reaper, ...). A Smartie picked up
// says one of the game's own lines (smchpickup: "Sure, I'll work for you."), now and then a ride
// line while carried (smchride) and a goodbye when set down (smchdropoff), as a subtitle with its
// name. When the original shows these is not known; the rules here are ours.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "core/log.h"
#include "assets/gtext.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

std::string GameApp::smartieName(uint32_t seed) const {
    if (!gameText_) return "Smartie";
    uint32_t h = seed * 2654435761u + 0x9e3779b9u;
    auto syl = [&](char part) {
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        char key[24];
        std::snprintf(key, sizeof key, "smnamesyl%c%u", part, 1 + h % 4u);
        return gameText_->get(key);
    };
    std::string n = syl('a') + syl('b') + syl('c');
    return n.empty() ? std::string("Smartie") : n;
}

void GameApp::smartieSay(const std::string& name, const char* key) {
    if (!gameText_) return;
    std::string line = gameText_->get(key);
    if (line.empty()) return;
    for (auto& ch : line)
        if (static_cast<unsigned char>(ch) == 0x92) ch = '\'';  // the text's Windows apostrophe
    timmyLine_ = name + ": " + line;
    timmyLineT_ = 3.0f;
    LOGI("%s says (%s): %s", name.c_str(), key, line.c_str());
}

void GameApp::updateSmartieLines(float dt) {
    timmyLineT_ = std::max(0.0f, timmyLineT_ - dt);  // the subtitle line (Timmy's or a Smartie's)
    // While carrying a Smartie: a ride line now and then.
    bool carrying = false;
    int who = -1;
    for (size_t i = 0; i < rescue_.smarties.size(); ++i)
        if (rescue_.smarties[i].state == SmartieState::Carried) { carrying = true; who = int(i); }
    if (!carrying) { rideLineT_ = 8.0f + 6.0f * frand(); return; }
    rideLineT_ -= dt;
    if (rideLineT_ > 0 || timmyLineT_ > 0) return;
    rideLineT_ = 12.0f + 8.0f * frand();
    int k = 1 + int(frand() * 3.99f);
    if (k == 3 && !player_.thrusting()) k = 1;  // "Do that jetpack thing again!" only after flying
    char key[16];
    std::snprintf(key, sizeof key, "smchride%d", k);
    smartieSay(smartieName(uint32_t(who) + 1000u), key);
}

void GameApp::renderTags() {
    if (!text_.ready() || !settings_.subtitles) return;
    const Pose head = rig_.toWorld(lastInput_.head);
    const Vec3 eye = head.position;
    const Vec3 gaze = head.orientation.rotate(Vec3{0, 0, -1});
    struct Tag { uint64_t id; Vec3 at; std::string label; Vec4 color; float d; };
    std::vector<Tag> cand;
    const Vec4 smartieCol{1.0f, 0.78f, 0.16f, 1.0f};  // the game's Smartie chat colour (ffc220)
    const Vec4 enemyCol{1.0f, 0.42f, 0.32f, 1.0f}, bossCol{1.0f, 0.62f, 0.2f, 1.0f}, beastCol{0.75f, 0.95f, 0.7f, 1.0f};
    auto consider = [&](uint64_t id, const Vec3& top, const std::string& label, const Vec4& col, float range = 45.0f) {
        const float d = length(top - eye);
        if (d > range || d < 0.4f) return;
        cand.push_back({id, top, label, col, d});
    };
    // Skirmish players, in their team's colour (who's who on a map of identical Meccaryns), further out.
    if (skirmish_)
        for (const auto& p : sk_.players) {
            if (p.local || !p.alive) continue;
            static const Vec4 teamCol[3] = {{0.45f, 0.65f, 1.0f, 1.0f}, {0.45f, 1.0f, 0.45f, 1.0f}, {1.0f, 0.4f, 0.3f, 1.0f}};
            consider(7000000u + uint64_t(p.id), p.body.position + Vec3{0, p.body.height + 0.35f, 0},
                     p.name + (p.carrying >= 0 ? " (FLAG)" : ""), teamCol[p.team % 3], 90.0f);
        }
    for (const auto& ci : chars_) {
        if (ci.rescue < 0 || !ci.visible) continue;
        const SmartieState st = rescue_.smarties[size_t(ci.rescue)].state;
        if (st == SmartieState::Carried || st == SmartieState::Gone) continue;
        consider(1000000u + uint64_t(ci.rescue), ci.pos + Vec3{0, 1.15f * ci.scale + 0.25f, 0}, smartieName(uint32_t(ci.rescue) + 1000u), smartieCol);
    }
    for (size_t i = 0; i < villagers_.villagers.size(); ++i)
        if (VillagerDirector::alive(villagers_.villagers[i]))
            consider(2000000u + i, villagers_.villagers[i].pos + Vec3{0, 1.35f, 0}, smartieName(uint32_t(i) + 5000u), smartieCol);
    for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
        const Creature& c = creatures_.creatures[i];
        if (c.state == CreatureState::Dead || CreatureDirector::underground(c) || c.state == CreatureState::Latched) continue;
        const char* n = c.size == CreatureSize::Large ? "Giant Ripper" : (c.size == CreatureSize::Medium ? "Ripper" : (c.scale < 0.7f ? "Mini Ripper" : "Small Ripper"));
        consider(3000000u + i, c.pos + Vec3{0, 1.7f * c.scale + 0.2f, 0}, n, enemyCol);
    }
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const Enemy& e = enemies_.enemies[i];
        if (e.state == EnemyState::Dead) continue;
        consider(4000000u + i, e.pos + Vec3{0, 2.3f, 0}, "Sea Reaper", enemyCol);
    }
    for (size_t i = 0; i < vimps_.vimps.size(); ++i) {
        const Vimp& v = vimps_.vimps[i];
        if (v.state == VimpState::Dead) continue;
        consider(5000000u + i, v.pos + Vec3{0, 2.2f, 0}, "Vimp", beastCol);
    }
    for (size_t i = 0; i < chars_.size(); ++i) {
        const CharacterInst& ci = chars_[i];
        if (!ci.boss || !ci.visible || ci.hp <= 0) continue;
        static const char* kinds[] = {nullptr, "Kabuto", "Offspring", "Charger", "Sonak"};
        std::string n = ci.bossKind > 0 && ci.bossKind <= 4 ? kinds[ci.bossKind] : std::string();
        if (n.empty() && assetWorld_ && ci.type >= 0) {
            n = assetWorld_->registry().modelName(uint32_t(ci.type));
            const size_t us = n.find('_');
            if (us != std::string::npos) n = n.substr(0, us);
            if (!n.empty()) n[0] = char(std::toupper(static_cast<unsigned char>(n[0])));
        }
        if (!n.empty()) consider(6000000u + i, ci.pos + Vec3{0, ci.bossH + 0.4f, 0}, n, bossCol);
    }
    // Shown: close by (within 5 m), or looked at (within a few degrees of the gaze). Fades by
    // game time (both eyes see the same frame: the second adds nothing).
    const float step = std::min(0.1f, std::max(0.0f, float(time_ - tagTime_)));
    tagTime_ = time_;
    std::vector<Tag> shown;
    for (auto& t : cand) {
        const Vec3 to = (t.at - eye) * (1.0f / t.d);
        const float cosA = dot(to, gaze);
        const float cone = std::cos(std::max(0.06f, std::atan(1.5f / t.d)));  // the target's own size, at least 3 deg
        const bool want = (t.d < 5.0f && cosA > 0.2f) || cosA > cone;
        float& a = tagAlpha_[t.id];
        a = want ? std::min(1.0f, a + step * 5.0f) : std::max(0.0f, a - step * 2.5f);  // in over 0.2 s, out over 0.4 s
        if (a > 0.01f) shown.push_back(t);
    }
    static const bool trace = std::getenv("GVR_TAG_TRACE") != nullptr;  // test hook
    if (trace && !cand.empty()) {
        const Vec3 to = normalize(cand[0].at - eye);
        LOGI("tags: %zu candidates, %zu shown, first %s %.1f m; eye (%.1f %.1f %.1f) gaze (%.2f %.2f %.2f) to (%.2f %.2f %.2f)", cand.size(), shown.size(),
             cand[0].label.c_str(), cand[0].d, eye.x, eye.y, eye.z, gaze.x, gaze.y, gaze.z, to.x, to.y, to.z);
    }
    std::sort(shown.begin(), shown.end(), [](const Tag& a, const Tag& b) { return a.d < b.d; });
    if (shown.size() > 6) shown.resize(6);
    const Quat face = Quat::fromAxisAngle({0, 1, 0}, yawOf(head.orientation));
    for (const auto& t : shown) {
        const float a = tagAlpha_[t.id];
        const float lh = std::min(1.4f, std::max(0.08f, 0.035f * t.d));  // about the same size seen from any distance
        float w = 0, h = 0;
        const MeshId m = text_.mesh(t.label, 0.1f, 20.0f, &w, &h);  // one mesh per label, scaled to size
        if (!m) continue;
        const float k = lh / 0.1f;
        w *= k;
        h *= k;
        const Mat4 xf = Mat4::translation(t.at) * Mat4::rotation(face);
        if (trace) LOGI("tag '%s' mesh %u size %.2f x %.2f alpha %.2f at (%.1f %.1f %.1f)", t.label.c_str(), unsigned(m), w, h, a, t.at.x, t.at.y, t.at.z);
        text_.drawPanel(xf, w + 0.6f * lh, h + 0.3f * lh, {0, 0, 0, 0.45f * a}, true);
        text_.draw(m, xf * Mat4::translation({0, 0, 0.002f}) * Mat4::scale({k, k, k}), {t.color.x, t.color.y, t.color.z, a}, true);
    }
}

}  // namespace gvr
