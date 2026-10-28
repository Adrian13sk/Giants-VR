// GameApp: Missions: setup, objectives, checkpoints, the loading screen and briefing, mission numbers. Same class; see game_app.h.
// (Split out of game_app.cpp, 2026-10-28: moved as it was.)
#include "game/game_app.h"
#include "game/hero_clips.h"
#include "core/log.h"
#include "core/prof.h"
#include "scene/asset_world.h"
#include "audio/mixer.h"
#include "audio/music.h"
#include "assets/gtext.h"
#include "assets/skin_anim.h"
#include "game/cutscene.h"
#include "game/arm_ik.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"
#include <algorithm>
#include <fstream>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>

namespace gvr {
void GameApp::initMission() {
    // Proximity triggers on placed objects (death triggers were added with their characters).
    for (const auto& t : assetWorld_->triggerObjects())
        if (t.kind == 4) {  // "On Vimp Meat Proximity": bring the meat here
            objectives_.push_back({4, t.boundsMin, t.boundsMax});
            LOGI("Objective: bring %d pieces of Vimp meat to type %d", kVimpMeatNeeded, int(t.type));
        }
    if (assetWorld_->startObject_ >= 0)
        for (const auto& o : assetWorld_->mission().objects)
            if (o.group == 3 && o.parent < 0 && toLower(assetWorld_->registry().modelName(o.type)) == "marker") {
                Vec3 p = giantsToRuntime(o.x, o.y, o.z);
                float gy;
                if (assetWorld_->terrain().heightAt(p.x, p.z, gy)) p.y = gy;
                spawnRivalKabuto(p, o.yaw * kPi / 180.0f);
                break;
            }
    // Death triggers on buildings: destroy it. The trigger is matched to the destructible of the
    // same type nearest its centre.
    for (const auto& t : assetWorld_->triggerObjects()) {
        if (t.kind != 2) continue;
        const Vec3 c = (t.boundsMin + t.boundsMax) * 0.5f;
        // Class 97: a group spawner ("1mcsonak_4guards", "1tsonak_4guards", "1tsonak"): a Sonak - the
        // Reapers' war beast (sonak_l0, 350 hp) - and its Reaper guards. Ours: the Sonak stands at
        // the spawner with the guards round it; the group dies with all of them (a lone Sonak
        // spawner gets one guard).
        if (const ObjectType* ot = assetWorld_->registry().type(t.type); ot && ot->objectClass == 97) {
            const std::string nm = assetWorld_->registry().modelName(t.type);
            const int guards = nm.find("4guards") != std::string::npos ? 4 : 1;
            for (int g = 0; g < guards; ++g) {
                const float a = 6.2832f * float(g) / float(guards);
                Vec3 p = c + Vec3{std::cos(a) * 4.0f, 0, std::sin(a) * 4.0f};
                float gy;
                if (assetWorld_->terrain().heightAt(p.x, p.z, gy)) p.y = gy;
                const int ei = enemies_.spawn(p, a);
                enemies_.enemies[size_t(ei)].objective = int(objectives_.size());
                objectives_.push_back({2, p, p});
            }
            {
                const ObjectType* st = assetWorld_->registry().type(575);  // sonak_l0
                Vec3 sp = c;
                float sy;
                if (assetWorld_->terrain().heightAt(sp.x, sp.z, sy)) sp.y = sy;
                spawnBoss(4, "sonak_l0", "sonak_idle.anm", sp, frand() * 6.28f, st && st->hitPoints > 0 ? st->hitPoints : 350.0f);
            }
            missionHostiles_ = true;
            LOGI("Objective: kill the %d guard%s of %s at game (%.0f, %.0f)", guards, guards > 1 ? "s" : "", nm.c_str(), -c.x / gGameUnitsToMeters,
                 c.z / gGameUnitsToMeters);
            continue;
        }
        int best = -1;
        float bestD = 12.0f;
        const auto& objs = assetWorld_->worldObjects();
        for (size_t k = 0; k < destruct_.items.size(); ++k) {
            const auto& o = objs[size_t(destruct_.items[k].object)];
            const float d = length(Vec3{o.centre.x - c.x, 0, o.centre.z - c.z});
            if (o.type == t.type && d < bestD) { bestD = d; best = int(k); }
        }
        if (best < 0) { LOGW("Death trigger on type %d: no destructible found", int(t.type)); continue; }
        Objective ob{5, t.boundsMin, t.boundsMax};
        ob.item = best;
        objectives_.push_back(ob);
        LOGI("Objective: destroy %s at game (%.0f, %.0f)", objs[size_t(destruct_.items[size_t(best)].object)].name.c_str(),
             -c.x / gGameUnitsToMeters, c.z / gGameUnitsToMeters);
    }
    for (const auto& t : assetWorld_->triggerObjects())
        if (t.kind == 1) {
            objectives_.push_back({1, t.boundsMin, t.boundsMax});
            const Vec3 c = (t.boundsMin + t.boundsMax) * 0.5f;
            LOGI("Objective: reach type %d at game (%.0f, %.0f), %.1f m wide", int(t.type), -c.x / gGameUnitsToMeters,
                 c.z / gGameUnitsToMeters, t.boundsMax.x - t.boundsMin.x);
        }
    // (Reaperski riding and races are set up at the end, once the hero is known.)
    // Starting inventory: a Mecc mission lists the items the player is handed (op 0x0E).
    // weapondefs.bin's item table names item 1 the handgun (IconHandgun, object mc_gun_l0), not
    // the jetpack as first read; the jetpack has no item there. We still gate the jetpack on it in
    // Story 1 (absent only in 1a_1_timmy, where Baz loses his kit in the opening cutscene): kept
    // because it plays as before, now known to be our rule rather than the data's.
    const MissionData& m = assetWorld_->mission();
    bool jet = true;
    if (m.setup[0] == 2) jet = std::find(m.items.begin(), m.items.end(), 1u) != m.items.end();
    // Only Story 1 starts without it (Baz loses it in 1a); later chapters' item lists read
    // differently, and he keeps it from then on (2a lists no item 1 either).
    if (!jet && toLower(worldName_) != "story1") jet = true;
    player_.jetpackAvailable = jet;
    // The items he is handed (weapondefs.bin's item table: 3 shotgun, 5 machine gun, 6 sniper gun,
    // 14 rocket launcher; game_app_arsenal.cpp). Item 3 was read as the launcher until the table was
    // decoded; 1c-2c place mc_pickup_shells, the shotgun's ammo, and Timmy's present at the end of 1b
    // (flk_story1_get_rpg*) is animated with "shotgun" / "Ref_shotgun" nodes.
    grantMissionItems(m.items, "mission start");  // (Reaper missions: her bows, items 44-46)
    if (std::getenv("GVR_LAUNCHER") && launcherModel_) { hasLauncher_ = true; launcherAmmo_ = launcherMax_; }  // test hook
    // The world decides the hero (Reaper story: Delphi); she has no jetpack.
    // The mission's race (setup op 0x0B: 1 Kabuto, 2 Mecc, 3 Reaper) picks the hero - kstory5_5 is
    // played as the Meccaryn; the world name decides where the data gives none.
    const uint32_t race = m.setup[0];
    setHero(practice_ ? practiceHero_ : (race == 1 ? Hero::Kabuto : (race == 2 ? Hero::Mecc : (race == 3 ? Hero::Delphi : heroForWorld(worldName_)))));
    if (hero_ != Hero::Mecc) player_.jetpackAvailable = false;
    initBase();   // both depend on the hero just chosen (initCharacters may have run them with the last one)
    initPopup();
    // A level's Shop also sells ammo for the guns he carries / her bows' arrows (ours).
    if (shopOn_ && hero_ != Hero::Kabuto && std::none_of(shopItems_.begin(), shopItems_.end(), [](const ShopItem& i) { return i.kind == 4; }))
        shopItems_.push_back({4, shopAt_, false});
    if (shopOn_ && hero_ == Hero::Mecc && std::none_of(shopItems_.begin(), shopItems_.end(), [](const ShopItem& i) { return i.kind == 5; }))
        shopItems_.push_back({5, shopAt_, false});  // mines (no mission hands them out; ours)
    if (hero_ == Hero::Kabuto) {
        // He comes out of the sea weakened at the start of his story (k1a) and of 2a ("Eat 5
        // Smarties to reach full strength and size"); full size elsewhere. Our reading.
        const std::string mn = toLower(assetWorld_->missionName());
        // k1a: grow to break the wooden gate; k1b: "another growth spurt" for the stone gate;
        // k2a: "grow once more and attain your full magnificent girth".
        kbGrow_ = mn.rfind("k1a", 0) == 0 ? 0.5f : (mn.rfind("k1b", 0) == 0 ? 0.65f : (mn.rfind("k2a", 0) == 0 ? 0.8f : 1.0f));
        if (const char* g = std::getenv("GVR_KABUTO_GROW")) kbGrow_ = std::max(0.3f, std::min(1.0f, float(std::atof(g))));  // test hook
        applyKabutoGrowth(kbGrow_);
        LOGI("Kabuto starts at %.0f%% of his full size", kbGrow_ * 100.0f);
    }
    // Kabuto's state starts afresh each mission (setHero keeps it from one Kabuto mission to the next),
    // and his Offspring, boulders and throws don't outlive him when another hero takes over.
    kb_ = KabutoState{};
    initKabutoPowers();
    initThrows();
    if (hero_ == Hero::Kabuto) kbAdren_ = kbGrow_ >= 0.999f ? 1.0f : 0.0f;  // at full size he comes in ready
    initSkirmish();  // a multiplayer map (w_M_*): teams and computer players
    if (title_) return;  // the main menu's scene: no checkpoint
    if (haveRestore_) {  // what he carried at the checkpoint
        if (pendingRestore_.launcher && launcherModel_) {
            hasLauncher_ = true;
            launcherAmmo_ = std::max(launcherAmmo_, std::min(launcherMax_, pendingRestore_.rockets < 0 ? launcherGive_ : pendingRestore_.rockets));
        }
        grenades_ = std::max(grenades_, std::min(grenadeMax_, pendingRestore_.grenades));
        mines_ = std::max(mines_, std::min(minesMax_, pendingRestore_.mines));
        initBows();
        for (int b = 0; b < kNumBows; ++b)
            if (pendingRestore_.bows[b] && bows_[b].model) {
                bows_[b].have = true;
                if (bows_[b].maxAmmo > 0) bows_[b].ammo = std::max(bows_[b].ammo, std::min(bows_[b].maxAmmo, pendingRestore_.arrows[b]));
            }
        for (int g = 0; g < kNumGuns; ++g)
            if (pendingRestore_.guns[g] && guns_[g].model) {
                guns_[g].have = true;
                guns_[g].ammo = std::max(guns_[g].ammo, std::min(guns_[g].maxAmmo, pendingRestore_.ammo[g]));
            }
        flares_ = std::max(0, std::min(kMaxFlares, pendingRestore_.flares));
        syringes_ = std::max(syringes_, std::min(kMaxSyringes, pendingRestore_.syringes));
        haveRestore_ = false;
    }
    if (practice_) grantPracticeKit();
    else writeCheckpoint();  // (practice: never)
    // The mission's ambient music (the world's race and chapter).
    ambientSet_ = ambientFor(worldName_);
    initJetski();
    if (const char* w = std::getenv("GVR_WEAPON")) weaponSel_ = std::atoi(w);  // test hook: start with this weapon in hand
    if (!weaponOwned(weaponSel_)) weaponSel_ = 0;
    LOGI("Mission %s: %zu objectives, jetpack %s", assetWorld_->missionName().c_str(), objectives_.size(),
         jet ? "available" : "not yet (story)");
}

void GameApp::writeCheckpoint() {
    if (savePath_.empty() || !assetWorld_ || assetWorld_->missionName().empty()) return;
    SaveGame g;
    g.world = worldName_;
    g.mission = assetWorld_->missionName();
    g.syringes = syringes_;
    g.launcher = hasLauncher_;
    g.rockets = launcherAmmo_;
    for (int k = 0; k < kNumGuns; ++k) { g.guns[k] = guns_[k].have; g.ammo[k] = guns_[k].ammo; }
    g.grenades = grenades_;
    g.mines = mines_;
    for (int k = 0; k < kNumBows; ++k) { g.bows[k] = bows_[k].have; g.arrows[k] = bows_[k].ammo; }
    g.flares = flares_;
    if (writeSaveGame(savePath_, g)) LOGI("Checkpoint saved: %s / %s", g.world.c_str(), g.mission.c_str());
    else LOGW("Checkpoint could not be written to %s", savePath_.c_str());
}

void GameApp::debugReloadWithBriefing(const InputFrame& in) {
    if (assetWorld_) beginLoading(assetWorld_->missionName(), in);
}

void GameApp::beginLoading(const std::string& mission, const InputFrame& in, bool load) {
    load_ = LoadState{};
    load_.phase = load ? LoadPhase::FadeOut : LoadPhase::Brief;
    load_.mission = load ? mission : std::string();
    // The briefing faces where the player looks now (head yaw), 2 m away.
    load_.yaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : camYaw_;
    missionText(mission, load_.title, load_.brief);
    LOGI("Loading %s (%s)", mission.c_str(), load_.title.c_str());
}

void GameApp::missionText(const std::string& mission, std::string& title, std::string& brief) const {
    title = mission;
    brief.clear();
    if (!gameText_) return;
    const std::string t = gameText_->get(mission);
    if (!t.empty()) title = t;
    // Objectives text: '^' = new line, '~N' = colour change (dropped).
    const std::string mo = gameText_->get("MO_" + mission);
    for (size_t i = 0; i < mo.size(); ++i) {
        if (mo[i] == '^') brief += '\n';
        else if (mo[i] == '~' && i + 1 < mo.size()) ++i;
        else brief += mo[i];
    }
    while (!brief.empty() && (brief.front() == '\n' || brief.front() == ' ')) brief.erase(0, 1);
    while (!brief.empty() && (brief.back() == '\n' || brief.back() == ' ')) brief.pop_back();
}

void GameApp::renderBriefOverlay() {
    if (!text_.ready() || !assetWorld_) return;
    std::string title, brief;
    missionText(assetWorld_->missionName(), title, brief);
    char prog[64];
    std::snprintf(prog, sizeof prog, "OBJECTIVES %zu / %zu", objectivesDone_, objectives_.size());
    float tw = 0, th = 0, bw = 0, bh = 0, pw = 0, ph = 0;
    const MeshId tm = text_.mesh(title, 0.095f, 1.5f, &tw, &th);
    const MeshId bm = brief.empty() ? 0 : text_.mesh(brief, 0.055f, 1.5f, &bw, &bh);
    const MeshId pm = text_.mesh(prog, 0.055f, 1.5f, &pw, &ph);
    // Head-locked, slightly below eye level, the world still visible around and through it.
    const Quat r = subRot_;
    const float total = th + 0.05f + bh + 0.05f + ph;
    const Vec3 c = subHead_ + r.rotate(Vec3{0, -0.08f, -1.6f});
    const Mat4 base = Mat4::translation(c) * Mat4::rotation(r);
    text_.drawPanel(base, std::max(std::max(tw, bw), pw) + 0.12f, total + 0.12f, {0, 0, 0, 0.5f});
    float y = 0.5f * total;
    auto put = [&](MeshId m, float h, const Vec4& col) {
        if (!m) return;
        y -= 0.5f * h;
        text_.draw(m, base * Mat4::translation({0, y, 0.004f}), col);
        y -= 0.5f * h + 0.05f;
    };
    put(tm, th, {1.0f, 0.85f, 0.4f, 1.0f});
    put(bm, bh, {0.92f, 0.92f, 0.88f, 1.0f});
    put(pm, ph, {0.6f, 0.95f, 0.6f, 1.0f});
}

void GameApp::updateLoading(float dt, const InputFrame& in) {
    load_.t += dt;
    if (in.headValid) headWorldRot_ = rig_.toWorld(in.head).orientation;
    switch (load_.phase) {
        case LoadPhase::FadeOut:
            if (load_.t > 0.5f) { load_.phase = LoadPhase::Show; load_.t = 0; }
            break;
        case LoadPhase::Show:
            // Two frames of the briefing on screen before the (blocking) load, so the headset keeps
            // showing it while the level is rebuilt.
            if (++load_.frames >= 3) {
                if (!load_.world.empty() && assetWorld_ && assetWorld_->switchWorld(load_.world)) {
                    worldName_ = load_.world;
                    practice_ = load_.practice;  // (each world switch: a practice session only when asked for)
                    if (load_.toTitle) title_ = true;  // (loadMission sets the island up as the main menu's scene)
                    streamsIndexed_ = false;
                    streamFiles_.clear();
                    load_.mission = assetWorld_->missions().empty() ? std::string() : assetWorld_->missions().front();
                    for (const auto& m : assetWorld_->missions())  // a checkpoint's mission
                        if (!load_.worldMission.empty() && toLower(m) == toLower(load_.worldMission)) load_.mission = m;
                    missionText(load_.mission, load_.title, load_.brief);
                    if (practice_) {
                        const char* who = practiceHero_ == Hero::Delphi ? "DELPHI" : (practiceHero_ == Hero::Kabuto ? "KABUTO" : "BAZ");
                        load_.title = std::string("PRACTICE: ") + who;
                        load_.brief = "Every weapon of this hero's story. Reapers come in waves.\n"
                                      "Nothing here is saved.\nOptions -> MAIN MENU to leave.";
                    } else if (load_.toTitle) {
                        load_.title = "MAIN MENU";
                        load_.brief.clear();
                    }
                } else if (!load_.world.empty()) {
                    LOGW("Loading: world %s unavailable", load_.world.c_str());
                }
                // A world without missions (the multiplayer maps) is still set up (its scenery, a skirmish).
                if (!load_.mission.empty() || (assetWorld_ && assetWorld_->missions().empty())) loadMission(load_.mission);
                load_.phase = LoadPhase::Brief;
                load_.t = 0;
                if (load_.toTitle && title_) {
                    enterTitle();
                    load_.phase = LoadPhase::FadeIn;  // (no briefing to read)
                }
                LOGI("Briefing: %s (%zu characters of objectives)", load_.title.c_str(), load_.brief.size());
            }
            break;
        case LoadPhase::Brief:
            if (load_.t > (practice_ ? 4.0f : 6.0f)) { load_.phase = LoadPhase::FadeIn; load_.t = 0; }
            break;
        case LoadPhase::FadeIn:
            load_.phase = LoadPhase::Idle;  // the spawn (and its opening cutscene, with its own fade) follows
            lastStats_.valid = false;       // (shown on this one loading screen only - review 2026-10-28)
            break;
        case LoadPhase::Idle:
            break;
    }
}

void GameApp::renderLoading() {
    if (!text_.ready()) return;
    const Vec3 eye = renderer_.currentView().eyePosition;
    const float black = load_.phase == LoadPhase::FadeOut ? std::min(1.0f, load_.t / 0.5f) : 1.0f;
    const Vec3 fwd = headWorldRot_.rotate(Vec3{0, 0, -1});
    text_.drawPanel(Mat4::translation(eye + fwd * 0.1f) * Mat4::rotation(headWorldRot_), 1.0f, 1.0f, {0, 0, 0, black});
    if (load_.phase == LoadPhase::FadeOut) return;
    const Quat r = Quat::fromAxisAngle({0, 1, 0}, load_.yaw);
    const Vec3 base = eye + r.rotate(Vec3{0, 0, -2.0f});
    auto place = [&](float x, float up) { return Mat4::translation(base + r.rotate(Vec3{x, up, 0})) * Mat4::rotation(r); };
    // The original loading art of the story's race on the left (mecc_ / reaper_ / kabuto_loading),
    // the mission's title and objectives on the right.
    const Hero dest = load_.practice ? practiceHero_ : (!load_.world.empty() ? heroForWorld(load_.world) : hero_);
    const TextureId art = loadingArt(dest);
    const float fade = std::min(1.0f, (load_.phase == LoadPhase::Show ? 0.0f : 0.3f) + load_.t / 0.6f);
    float textX = 0.0f, wrap = 2.6f, titleWrap = 1.8f;
    if (art) {
        text_.drawImage(place(-0.66f, -0.08f), 1.12f, 0.84f, art, {1, 1, 1, fade});
        textX = 0.62f;
        wrap = titleWrap = 1.15f;
    }
    float w = 0, h = 0, th = 0;
    float y = art ? 0.32f : 0.42f;
    if (const MeshId t = text_.mesh(load_.title, art ? 0.1f : 0.14f, titleWrap, &w, &th)) {
        text_.draw(t, place(textX, y - 0.5f * th), {1.0f, 0.85f, 0.4f, 1.0f});
        y -= th + 0.06f;
    }
    if (!load_.brief.empty())
        if (const MeshId b = text_.mesh(load_.brief, art ? 0.05f : 0.055f, wrap, &w, &h)) text_.draw(b, place(textX, y - 0.5f * h), {0.9f, 0.9f, 0.85f, 1.0f});
    if (lastStats_.valid && !title_) {  // (2026-10-28) the mission just finished, in numbers
        const int m = int(lastStats_.seconds) / 60, sec = int(lastStats_.seconds) % 60;
        char s[200];
        const std::string acc = lastStats_.shots > 0 ? std::to_string(int(100.0 * double(lastStats_.hits) / double(lastStats_.shots) + 0.5)) + "% HITS" : "NO SHOTS";
        char best[40] = "";
        if (lastStats_.newBest && lastStats_.best > 0) std::snprintf(best, sizeof best, "   NEW BEST (WAS %d:%02d)", int(lastStats_.best) / 60, int(lastStats_.best) % 60);
        else if (lastStats_.best > 0) std::snprintf(best, sizeof best, "   BEST %d:%02d", int(lastStats_.best) / 60, int(lastStats_.best) % 60);
        std::snprintf(s, sizeof s, "DONE: %s  -  %d:%02d%s   %d KILLS   %s   %d %s", lastStats_.title.c_str(), m, sec, best, lastStats_.kills, acc.c_str(),
                      lastStats_.deaths, lastStats_.deaths == 1 ? "DEATH" : "DEATHS");
        if (const MeshId st = text_.mesh(s, 0.045f, 2.6f, &w, &h)) text_.draw(st, place(0.0f, -0.555f), {0.7f, 1.0f, 0.75f, 0.9f});
    }
    const float pulse = 0.55f + 0.45f * std::sin(load_.t * 4.0f);
    const char* label = load_.phase == LoadPhase::Show ? "LOADING..." : "GET READY";
    if (const MeshId l = text_.mesh(label, 0.07f, 1.5f, &w, &h)) text_.draw(l, place(0.0f, lastStats_.valid ? -0.65f : -0.62f), {1, 1, 1, pulse});
}

void GameApp::recordMissionStats() {
    if (!assetWorld_ || practice_) return;
    MissionStats st;
    std::string t, b;
    missionText(assetWorld_->missionName(), t, b);
    st.title = t.empty() ? assetWorld_->missionName() : t;
    for (char& c : st.title) c = char(std::toupper(static_cast<unsigned char>(c)));
    st.seconds = float(time_ - statStartT_);
    for (const auto& e : enemies_.enemies) st.kills += e.state == EnemyState::Dead ? 1 : 0;
    for (const auto& c : creatures_.creatures) st.kills += c.state == CreatureState::Dead ? 1 : 0;
    st.shots = weapon_.shotsFired() - statShots0_;
    st.hits = weapon_.hits() - statHits0_;
    st.deaths = statDeaths_;
    st.valid = true;
    // The fastest finish so far (progress.txt keeps it).
    const std::string key = toLower(assetWorld_->missionName());
    const auto it = bestTimes_.find(key);
    st.best = it != bestTimes_.end() ? it->second : -1.0f;
    st.newBest = st.seconds > 1.0f && (st.best < 0 || st.seconds < st.best);
    if (st.newBest) {
        bestTimes_[key] = st.seconds;
        if (!progressPath_.empty() && !std::getenv("GVR_PROGRESS"))
            if (FILE* f = std::fopen(progressPath_.c_str(), "a")) {
                std::fprintf(f, "best=%s %.1f\n", key.c_str(), st.seconds);
                std::fclose(f);
            }
    }
    lastStats_ = st;
    LOGI("Mission stats: %s, %.0f s, %d kills, %zu shots %zu hits, %d deaths%s", st.title.c_str(), st.seconds, st.kills, st.shots, st.hits, st.deaths,
         st.newBest ? " - a new best time" : "");
}

void GameApp::loadMission(const std::string& name) {
    AssetWorld* w = assetWorld_.get();
    // (An empty name: a world without missions, already placed by switchWorld.)
    if (!w || (name.empty() ? !w->missions().empty() : !w->selectMission(name))) { LOGW("Mission %s unavailable", name.c_str()); return; }
    chars_.clear();
    creatures_.creatures.clear();
    creatures_.spits.clear();
    enemies_.enemies.clear();
    rescue_ = RescueDirector{};
    nests_.clear();
    objectives_.clear();
    objectivesDone_ = 0;
    haveCheckpoint_ = false;
    blazes_.clear();                                  // (review 2026-10-28: a blaze burnt on into the next mission)
    telPhase_ = 0; telFade_ = 0; telAiming_ = false;  // (and a teleport blink in progress)
    shadowFresh_ = false;
    missionComplete_ = false;
    missionHostiles_ = false;
    advanceDone_ = false;
    piranhaKnown_ = false;
    blasts_.clear();
    gibs_.clear();
    playerHealth_ = 100.0f;
    initCharacters(w, w->models());
    initMission();
    needSpawn_ = true;  // to the mission's start marker; its opening cutscene follows
    {
        // Resident memory after each load (Linux/Android): a leak across mission loads would show
        // as steady growth here - the Quest closes apps that grow too large.
        long rssKb = -1;
        if (FILE* f = std::fopen("/proc/self/status", "r")) {
            char line[256];
            while (std::fgets(line, sizeof line, f))
                if (std::strncmp(line, "VmRSS:", 6) == 0) { rssKb = std::atol(line + 6); break; }
            std::fclose(f);
        }
        LOGI("Mission %s loaded (resident memory %ld MB)", name.c_str(), rssKb >= 0 ? rssKb / 1024 : -1L);
        statStartT_ = time_;
        statShots0_ = weapon_.shotsFired();
        statHits0_ = weapon_.hits();
        statDeaths_ = 0;
    }
}

void GameApp::completeObjective(int index) {
    if (index < 0 || size_t(index) >= objectives_.size() || objectives_[size_t(index)].done) return;
    objectives_[size_t(index)].done = true;
    ++objectivesDone_;
    haptics_.push_back({kLeft, 0.4f, 0.08f});
    LOGI("Objective %d done (%zu/%zu)", index, objectivesDone_, objectives_.size());
    // (2026-10-28) A checkpoint: dying after this brings you back here, not to the mission's start - where you
    // stood on firm ground, out of deep water.
    if (playMode_ && !dying_ && player_.onGround && player_.position.y > player_.seaLevel - 0.3f * player_.height) {
        checkpointAt_ = player_.position;
        checkpointYaw_ = player_.yaw;
        haveCheckpoint_ = true;
        LOGI("Checkpoint: objective %d at (%.1f %.1f %.1f)", index, checkpointAt_.x, checkpointAt_.y, checkpointAt_.z);
    }
    if (objectivesDone_ < objectives_.size() || missionComplete_) return;
    missionComplete_ = true;
    LOGI("Mission %s complete", assetWorld_->missionName().c_str());
    markMissionDone(assetWorld_->missionName());
    recordMissionStats();
    haptics_.push_back({kLeft, 0.9f, 0.3f});
    haptics_.push_back({kRight, 0.9f, 0.3f});
    // The outro cutscene (op 0x19); where it can't be played, its soundtrack still carries the
    // dialogue. In 1a_1_timmy that is Timmy handing Baz the jetpack.
    const MissionData& m = assetWorld_->mission();
    if (!m.flick19.empty()) {
        startCutscene(m.flick19);
        if (!cutsceneActive()) playFlickSoundtrack(m.flick19);
    }
    if (!player_.jetpackAvailable) {
        player_.jetpackAvailable = true;
        LOGI("Jetpack received");
    }
    if (music_ && !streamLeft_) { music_->request("mvy"); stingerLeft_ = 8.0f; }
}

void GameApp::updateMission(float dt) {
    if (missionComplete_ || !playMode_) return;
    // Test hook GVR_AUTOCOMPLETE=<s>: each mission is marked complete after that much play (loading and
    // cutscenes don't count) - the whole campaign's progression in one run (the headset's full test).
    static const float autoComplete = std::getenv("GVR_AUTOCOMPLETE") ? float(std::atof(std::getenv("GVR_AUTOCOMPLETE"))) : 0.0f;
    if (autoComplete > 0 && dt > 0 && !cutsceneActive() && load_.phase == LoadPhase::Idle && assetWorld_) {
        if (assetWorld_->missionName() != autoMission_) { autoMission_ = assetWorld_->missionName(); autoPlayT_ = 0; }
        autoPlayT_ += dt;
        if (autoPlayT_ >= autoComplete) {
            LOGI("Test: mission %s completed after %.0f s of play (GVR_AUTOCOMPLETE)", autoMission_.c_str(), autoPlayT_);
            // As in play: each objective done (the log, the outro cutscene, the jetpack after 1a).
            for (size_t i = 0; i < objectives_.size(); ++i) completeObjective(int(i));
            if (!missionComplete_) {
                missionComplete_ = true;
                LOGI("Mission %s complete", assetWorld_->missionName().c_str());
                markMissionDone(assetWorld_->missionName());
                recordMissionStats();
            }
            return;
        }
    }
    // Proximity: within 2 m (10 game units) of the object's bounds.
    const Vec3 p = player_.position + Vec3{0, 0.9f, 0};
    for (size_t i = 0; i < objectives_.size(); ++i) {
        const Objective& o = objectives_[i];
        if (o.done || o.kind != 1) continue;
        const Vec3 c{std::max(o.boundsMin.x, std::min(o.boundsMax.x, p.x)),
                     std::max(o.boundsMin.y, std::min(o.boundsMax.y, p.y)),
                     std::max(o.boundsMin.z, std::min(o.boundsMax.z, p.z))};
        if (length(c - p) <= 10.0f * gGameUnitsToMeters + (player_.radius - 0.35f)) completeObjective(int(i));
    }
    for (size_t i = 0; i < objectives_.size(); ++i) {
        const Objective& o = objectives_[i];
        if (!o.done && o.kind == 5 && o.item >= 0 && size_t(o.item) < destruct_.items.size() && !destruct_.items[size_t(o.item)].alive())
            completeObjective(int(i));
    }
}

}  // namespace gvr
