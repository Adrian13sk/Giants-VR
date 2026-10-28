// GameApp: Sound effects, the room echo, the audio mix, music and voice streams. Same class; see game_app.h.
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
bool GameApp::ensureSound(const char* name) {
    // Sounds not in the preloaded list are loaded the first time they're asked for (several of the
    // rework's were never in that list and played nothing: birds, splashes, Kabuto's throws...).
    if (!audio_ || !name || !*name) return false;
    if (audio_->hasSound(name)) return true;
    if (missingSounds_.count(name) || !assetWorld_) return false;
    std::vector<uint8_t> bytes;
    WavData w;
    std::string err;
    if (assetWorld_->fs().read(std::string(name) + ".wav", bytes) && parseWav(bytes.data(), bytes.size(), w, &err) &&
        audio_->addSound(name, std::move(w))) {
        LOGI("Sound %s loaded on first use", name);
        return true;
    }
    missingSounds_.insert(name);
    LOGW("Sound %s unavailable %s", name, err.c_str());
    return false;
}

void GameApp::sfx(const char* name, const Vec3& pos, float vol, float pitch) {
    if (!audio_) return;
    ensureSound(name);
    const uint32_t id = audio_->play(name, pos, vol, pitch);
    static const bool trace = std::getenv("GVR_SFX_TRACE") != nullptr;  // (tests: which sounds play)
    if (trace) LOGI("Sfx %s %s vol %.2f at (%.1f %.1f %.1f)", name, id ? "played" : "MISSING", vol, pos.x, pos.y, pos.z);
}
void GameApp::setAudio(AudioMixer* mixer) {
    audio_ = mixer;
    ambVoices_.clear();  // (voices of another mixer)
    ambRain_ = 0;
    speedWind_ = 0;
    makeAmbienceSounds();
    applySettings();  // volumes
    if (!audio_ || !assetWorld_) return;
    static const char* names[] = {
        "me_peas1", "me_peas2", "bi_dirt1", "bi_hit1", "bi_hit2", "ex_bdys1", "rh_sprsh", "rh_fsld1",
        "rpman_hit1", "rpman_hit2", "rpman_hit3", "rpman_hit4", "rpman_die1", "rpman_die2", "rpman_die3",
        "rpman_die4", "rpman_die5", "me_shrt1", "me_shrt2", "me_shrt3", "me_jpkst", "me_jpklp", "me_jpken",
        "me_jpkno", "me_rsgsh", "me_rsgex", "me_sprsh", "me_mchst", "me_mchlp", "me_mchen", "me_outam", "pu_ammo", "me_mtrsh", "me_mtrht", "me_grnth", "me_grnex", "me_minth", "me_minlk", "me_minml", "me_minex", "sr_hbwpl", "sr_hbwsh", "sr_pbwpl", "sr_pbwsh", "sr_stohb", "sr_stopb", "sr_rbwpl", "sr_rbwsh", "sr_rpgpl", "sr_rpgsh", "sr_sbwpl", "sr_sbwsh", "sr_storb", "sr_storp", "sr_stosb", "sr_btosw", "me_fsgr1", "me_fsgr2", "me_hdfl1", "me_hdfl2", "me_sld1", "me_slden",
        // Footsteps by surface, the swimming loop.
        "me_fsgr1", "me_fsgr2", "me_fssa1", "me_fssa2", "me_fsst1", "me_fsst2", "me_fswd1", "me_fswd2", "me_fswa1", "me_fswa2", "me_swmlp",
        // Cleaners eating a body.
        "cl_eatst", "cl_eatlp", "cl_eaten",
        // Rippers: rs_ small, rm_ medium, rl_ large (inferred from the names and the three Ripper types).
        "rs_attk1", "rs_attk2", "rs_chitt", "rs_deat1", "rs_deat2", "rs_feet", "rs_injr1", "rs_injr2",
        "rm_atfr1", "rm_atfr2", "rm_attk1", "rm_chitt", "rm_deat1", "rm_deat2", "rm_feet", "rm_frexp", "rm_inj01",
        "rm_inj02", "rl_atfr1", "rl_atfr2", "rl_attk1", "rl_chitt", "rl_deat1", "rl_deat2", "rl_feet", "rl_frexp1",
        "rl_frexp2", "rl_injr1", "rl_injr2", "ex_bdyl1", "ex_bdyl2",
        // Ragdoll landings, a cut limb, a dropped gun.
        "ex_bdys2", "sr_fall1", "sr_fall2", "me_drpwp",
        // Scenery breaking: trees, stone, wood, metal (large and small).
        "ex_treel", "ex_trees", "ex_stonl", "ex_stons", "ex_woodl", "ex_woods", "ex_metll", "ex_metls", "ex_dirtl", "ex_dirts",
        // Carrying Smarties (1b): pick up / let go / drop off, splash, piranhas.
        "me_smton", "me_smtof", "me_smtdp", "se_splsh", "pi_swrm1", "pi_swrm2",
        // Rocket launcher: launch, explosion.
        "me_msls1", "me_mslex",
        // Delphi (Sea Reaper): sword swish and hits, fire spell (names read as such; provisional).
        "sr_swsh1", "sr_swsh2", "sr_swht1", "sr_swht2", "sr_swht3", "sr_swht4", "sr_spfsh",
        // Ripper nests: a Ripper hatching.
        "rs_spawn", "rm_spawn", "rl_spawn",
        // HUD weapon select, health pickup, injection.
        "ph_selct", "pu_hlth", "me_syrng",
        // Kabuto: footsteps by surface, punches and hits, stomp, landing, eating, roars, death.
        "ka_fsgr1", "ka_fsgr2", "ka_fssa1", "ka_fssa2", "ka_fsst1", "ka_fsst2", "ka_fswa1", "ka_fswa2", "ka_pnch1", "ka_pnch2",
        "ka_ltht1", "ka_mdht1", "ka_hvht1", "ka_stomp", "ka_lndhd", "ka_grab", "ka_eat1", "ka_eat2", "ka_belch", "ka_sniff",
        "ka_roar1", "ka_roar2", "ka_roar3", "ka_roar4", "ka_deth1", "ka_deth2", "ka_wound", "ka_smrt1", "ka_smrt2",
        // Vimps and their meat.
        "vi_deat1", "vi_deat2", "vi_injr1", "vi_injr2", "vi_moo1", "vi_moo2", "vi_squi1", "pu_meat1", "pu_meat2", "ka_vimp1", "ka_vimp2",
        // The Reaperski: engine, rider on/off, gates, bumps, turbo, start / win / lose.
        "js_ron", "js_roff", "js_ridl1", "js_rmve1", "js_rrvu1", "js_rrvd1", "js_buoy", "js_mbuoy", "js_bump1", "js_bump2", "js_tbact",
        "js_start", "js_finis", "js_win", "js_lose",
        // Offspring and Charger (bosses).
        "of_claw1", "of_claw2", "of_roar1", "of_roar2", "of_dhit1", "of_dhit2", "of_deth1", "of_deth2", "ca_snort", "ca_roar", "ca_injr1",
        "ca_injr2", "ca_deat1", "ca_deat2",
        // Sonak.
        "so_atst1", "so_atsol", "so_snrt1", "so_roar1", "so_roar2", "so_injr1", "so_injr2", "so_deat1", "so_deat2", "pr_wsmil"};
    std::vector<std::string> all(std::begin(names), std::end(names));
    for (int k = 1; k <= 15; ++k)  // Smartie voice pairs gsmNN_1 / gsmNN_2 (no 07 in the data)
        for (int j = 1; j <= 2; ++j) {
            if (k == 7) continue;
            char n[16];
            std::snprintf(n, sizeof n, "gsm%02d_%d", k, j);
            all.push_back(n);
        }
    size_t ok = 0;
    for (const std::string& ns : all) {
        const char* n = ns.c_str();
        std::vector<uint8_t> bytes;
        WavData w;
        std::string err;
        if (assetWorld_->fs().read(std::string(n) + ".wav", bytes) && parseWav(bytes.data(), bytes.size(), w, &err) &&
            audio_->addSound(n, std::move(w)))
            ++ok;
        else
            LOGW("Sound %s unavailable %s", n, err.c_str());
    }
    LOGI("Audio: %zu/%zu original sounds loaded", ok, all.size());
    initMusic();
}

// Rework: room echo. Every quarter second, twelve level rays from the eyes (40 m, more for a giant) and
// one straight up: walls all round (a canyon, a gorge) and a roof (a cave, a hall) bring the sounds
// back as reverb; open ground is nearly dry. GVR_ECHO_TRACE logs it.
void GameApp::updateRoomEcho(float dt) {
    echoT_ -= dt;
    if (echoT_ > 0) return;
    echoT_ = 0.25f;
    if (!worldRay_ || !assetWorld_ || cutsceneActive() || !lastInput_.headValid) { audio_->setReverb(0.05f); return; }
    const Vec3 eye = rig_.toWorld(lastInput_.head).position;
    const float reach = 40.0f * std::sqrt(std::max(1.0f, heroScale_));
    int walls = 0;
    for (int k = 0; k < 12; ++k) {
        const float a = float(k) * 0.5236f;
        float t;
        Vec3 n;
        if (worldRay_(eye, Vec3{std::sin(a), 0.08f, std::cos(a)}, reach, t, n)) ++walls;
    }
    float t;
    Vec3 n;
    const bool roof = worldRay_(eye, Vec3{0, 1, 0}, 30.0f * std::sqrt(std::max(1.0f, heroScale_)), t, n);
    const float enclosed = float(walls) / 12.0f;
    const float wet = 0.05f + 0.32f * enclosed * enclosed + (roof ? 0.15f : 0.0f);
    audio_->setReverb(wet);
    static const bool trace = std::getenv("GVR_ECHO_TRACE") != nullptr;
    if (trace) LOGI("echo: %d/12 walls, roof %d -> reverb %.2f", walls, int(roof), wet);
}

void GameApp::updateAudio(float dt) {
    updateMusic(dt);
    if (!audio_) return;
    hurtCooldown_ -= dt;
    audio_->setListener(rig_.toWorld(lastInput_.head).position, headWorldRot_);
    updateRoomEcho(dt);
    if (!playMode_) {
        if (jetLoop_) { audio_->stop(jetLoop_); jetLoop_ = 0; }
        if (slideLoop_) { audio_->stop(slideLoop_); slideLoop_ = 0; }
        if (swimLoop_) { audio_->stop(swimLoop_); swimLoop_ = 0; }
        return;
    }
    // Jetpack: start, loop while thrusting, end; a sputter when the tank runs dry.
    const bool thr = player_.thrusting();
    if (thr && !wasThrusting_) {
        sfx2D("me_jpkst", 0.6f);
        jetLoop_ = audio_->play2D("me_jpklp", 0.45f, 1.0f, true);
    }
    if (!thr && wasThrusting_) {
        if (jetLoop_) { audio_->stop(jetLoop_); jetLoop_ = 0; }
        sfx2D(player_.fuel() <= 0.0f ? "me_jpkno" : "me_jpken", 0.6f);
    }
    wasThrusting_ = thr;
    // Slide loop on steep ground.
    const bool sl = player_.sliding();
    if (sl && !wasSliding_) slideLoop_ = audio_->play2D("me_sld1", 0.5f, 1.0f, true);
    if (!sl && wasSliding_) {
        if (slideLoop_) { audio_->stop(slideLoop_); slideLoop_ = 0; }
        sfx2D("me_slden", 0.5f);
    }
    wasSliding_ = sl;
    // Landing after a real fall.
    if (!player_.onGround) {
        airTime_ += dt;
        fallSpeed_ = std::max(fallSpeed_, -player_.velocity.y);
    } else {
        if (!wasOnGround_ && airTime_ > 0.3f) sfx2D(fallSpeed_ > 6.0f ? "me_hdfl1" : "me_hdfl2", fallSpeed_ > 6.0f ? 0.8f : 0.45f);
        airTime_ = 0;
        fallSpeed_ = 0;
    }
    wasOnGround_ = player_.onGround;
    // Footsteps on the run/strafe cycle (two per cycle).
    const PlayerAnim pa = player_.anim();
    if (player_.onGround && (pa == PlayerAnim::Run || pa == PlayerAnim::Strafe || pa == PlayerAnim::Turbo) && playerModel_) {
        const float dur = std::max(0.1f, playerModel_->animDuration(clip_[int(pa)]));
        // Half-cycle counter: a footstep each time the clip enters a new half (either direction).
        const float halfIndex = std::floor(player_.animTime() / dur * 2.0f);
        if (halfIndex != prevRunPhase_) {
            // The original footstep sets by surface (me_fs + gr grass, sa sand, st stone, wd wood,
            // wa water): water when wading, wood on structures (bridges, walkways), sand on the
            // beach just above the sea, grass elsewhere. Louder than before (device test 19).
            const bool wade = player_.position.y < player_.seaLevel - 0.05f;
            const bool giant = hero_ == Hero::Kabuto;
            const char* set = wade ? "wa" : (player_.onObject() ? (giant ? "st" : "wd") : (player_.position.y < player_.seaLevel + 1.5f ? "sa" : "gr"));
            const std::string n = std::string(giant ? "ka_fs" : "me_fs") + set + ((int(halfIndex) & 1) ? "2" : "1");
            sfx2D(n.c_str(), wade ? 0.9f : (giant ? 1.0f : 0.75f), 0.95f + 0.1f * frand());
            if (giant) {
                // Every step of the giant: a thud in both hands and dust at the foot.
                haptics_.push_back({kLeft, 0.35f, 0.08f});
                haptics_.push_back({kRight, 0.35f, 0.08f});
                const Quat r = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
                const Vec3 foot = player_.position + r.rotate(Vec3{((int(halfIndex) & 1) ? 0.12f : -0.12f) * player_.height, 0, 0});
                if (!wade) dustBurst(foot, 0.08f * player_.height, 4);
            }
            if (wade) {  // a splash at the foot
                const Quat r = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
                const Vec3 foot = player_.position + r.rotate(Vec3{(int(halfIndex) & 1) ? 0.2f : -0.2f, 0, -0.2f});
                waterFx_.push_back({Vec3{foot.x, 0.02f, foot.z}, 0.0f, 0, 0.35f});
            }
        }
        prevRunPhase_ = halfIndex;
    }
    // Swimming: the stroke loop while moving in deep water.
    const bool strokes = player_.swimming() && player_.horizontalSpeed() > 0.3f;
    if (strokes && !swimLoop_) swimLoop_ = audio_->play2D("me_swmlp", 0.6f, 1.0f, true);
    if (!strokes && swimLoop_) { audio_->stop(swimLoop_); swimLoop_ = 0; }
    // Reaper footsteps while running.
    if (slotStep_.size() != enemyPool_.size()) slotStep_.assign(enemyPool_.size(), 0.0f);
    for (const auto& e : enemies_.enemies) {
        if (e.slot < 0 || e.clip != EnemyClip::Run || e.state == EnemyState::Dead) continue;
        float& t = slotStep_[size_t(e.slot)];
        t -= dt;
        if (t <= 0) {
            t = 0.3f;
            if (length(e.pos - player_.position) < 30.0f) sfx("rh_fsld1", e.pos, 0.75f, 0.9f + 0.2f * frand());
        }
    }
}

void GameApp::creatureSound(const CreatureEvent& e) {
    // Sound sets by Ripper size: rs_ small, rm_ medium, rl_ large (meanings inferred from names).
    const int k = int(e.size);
    const char* pre = k == 0 ? "rs_" : (k == 1 ? "rm_" : "rl_");
    const float pitch = 0.92f + 0.16f * frand();
    auto play = [&](const char* a, const char* b, float vol) {
        const std::string n = std::string(pre) + ((b && frand() < 0.5f) ? b : a);
        sfx(n.c_str(), e.pos, vol, pitch);
    };
    switch (e.kind) {
        case CreatureEvent::Alert: play("chitt", nullptr, 0.8f); break;
        case CreatureEvent::Swing:
        case CreatureEvent::Lunge:
            if (k == 0) play("attk1", "attk2", 0.9f); else play("attk1", nullptr, 0.9f);
            break;
        case CreatureEvent::SpitLaunch: play("atfr1", "atfr2", 1.0f); break;
        case CreatureEvent::SpitBurst:
            if (k == 2) play("frexp1", "frexp2", 1.0f); else sfx("rm_frexp", e.pos, 1.0f, pitch);
            break;
        case CreatureEvent::Strike: break;  // the player's hurt sound covers it
        case CreatureEvent::Hurt:
            if (k == 1) play("inj01", "inj02", 0.9f); else play("injr1", "injr2", 0.9f);
            break;
        case CreatureEvent::Die:
            ++recentKills_;
            play("deat1", "deat2", 1.0f);
            if (e.burst) sfx(frand() < 0.5f ? "ex_bdyl1" : "ex_bdyl2", e.pos, 0.9f, 0.95f + 0.1f * frand());
            break;
        default: break;  // burrowing and latching: burrowEvent
    }
}

// ---------------------------------------------------------------------------------------------
// Blood, corpses, Cleaners

namespace {
// Lists *.wav / *.raw files under `dir` (one level of subdirectories), lower-case name -> path.
void indexFiles(const std::string& dir, const char* ext, std::vector<std::pair<std::string, std::string>>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        const std::string path = dir + "/" + n;
        std::string l = toLower(n);
        if (l.size() > 4 && l.compare(l.size() - 4, 4, ext) == 0) {
            out.push_back({l.substr(0, l.size() - 4), path});
        } else if (l.find('.') == std::string::npos) {
            indexFiles(path, ext, out);
        }
    }
    closedir(d);
}
bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    const bool ok = n > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}
}  // namespace

void GameApp::initMusic() {
    if (!audio_ || !assetWorld_) return;
    std::vector<uint8_t> bytes;
    musicSet_ = std::make_unique<MusicSet>();
    std::string err;
    if (!assetWorld_->fs().read("MusicSet.bin", bytes) || !musicSet_->parse(bytes.data(), bytes.size(), &err)) {
        LOGW("Music: MusicSet.bin unavailable %s", err.c_str());
        musicSet_.reset();
        return;
    }
    // The game's Music folder sits next to Bin (copy it to the headset alongside Bin).
    for (const char* rel : {"/../Music", "/Music"}) indexFiles(dataDir_ + rel, ".raw", musicFiles_);
    LOGI("Music: %zu songs, %zu transitions, %zu segment files", musicSet_->songCount(), musicSet_->transitionCount(),
         musicFiles_.size());
    if (musicFiles_.empty()) { musicSet_.reset(); return; }
    music_ = std::make_unique<MusicDirector>(*musicSet_);
    // Ambient mood by story: Mecc a1 (the game also has a2, and Kabuto/Reaper sets).
    ambientSet_ = ambientFor(worldName_);
    music_->request(ambientSet_);
    audio_->setMusicVolume(0.8f * float(settings_.music) / 8.0f);
}

std::string GameApp::findMusicFile(const std::string& segment) const {
    for (const auto& f : musicFiles_)
        if (f.first == segment) return f.second;
    return {};
}

void GameApp::updateMusic(float dt) {
    if (!music_ || !audio_) return;
    streamLeft_ = std::max(0.0f, streamLeft_ - dt);
    stingerLeft_ = std::max(0.0f, stingerLeft_ - dt);
    // Mood from the fight around the player (our mapping; the original's is native code):
    // small battle (msb), large battle with 4+ attackers (mlb), victory stinger, ambient.
    size_t engaged = 0;
    for (const auto& c : creatures_.creatures)
        if (c.state != CreatureState::Dead && c.state != CreatureState::Idle && c.state != CreatureState::Return &&
            length(c.pos - player_.position) < 45.0f)
            ++engaged;
    for (const auto& e : enemies_.enemies)
        if (e.state != EnemyState::Dead && e.state != EnemyState::Idle && length(e.pos - player_.position) < 60.0f) ++engaged;
    const char* race = ambientSet_.c_str();  // "ma1" -> prefix 'm'
    const std::string pre(1, race[0]);
    std::string want = ambientSet_;
    if (engaged > 0) combatHold_ = 6.0f;
    else combatHold_ = std::max(0.0f, combatHold_ - dt);
    if (combatHold_ > 0) want = pre + (engaged >= 4 ? "lb" : (pre == "k" ? "ff" : "sb"));  // Kabuto's sets have no sb
    if (combatHold_ > 0 && assetWorld_) {  // the world's own battle sets (op 109 / 111), if it names them
        const std::string& own = assetWorld_->world().music[engaged >= 4 ? 3 : 1];
        if (!own.empty()) want = toLower(own);
    }
    // (2026-10-28) A battle that ends closes on its own ending phrase (msbend, rlbend...: one segment) before the
    // ambient set, instead of cutting across; the player's death plays the death song (mdh / rdh / kdh).
    if (combatHold_ > 0) battleSet_ = want;
    else if (!battleSet_.empty()) {
        const std::string end = battleSet_ + "end";
        if (!musicSet_ || musicSet_->variants(end).empty()) battleSet_.clear();  // (no ending phrase: straight back)
        else if (musicSet_ && !musicSet_->variants(end).empty() && music_->current() != end && !battleEndHeard_) want = end;
        else if (music_->current() == end) { battleEndHeard_ = true; }
        if (battleEndHeard_) { battleSet_.clear(); battleEndHeard_ = false; }
    }
    if (dying_ && musicSet_ && !musicSet_->variants(pre + "dh").empty()) want = pre + "dh";
    if (stingerLeft_ > 0) { want = pre + "vy"; battleSet_.clear(); }  // (the victory closes the battle: no ending phrase after it)
    if (streamLeft_ > 0) want.clear();  // a cutscene soundtrack is playing
    music_->request(want);
    static std::string lastMood = "?";
    if (want != lastMood) {
        LOGI("Music mood: %s", want.empty() ? "(soundtrack)" : want.c_str());
        lastMood = want;
    }
    // Keep one segment queued behind the playing one (mood changes land on segment ends).
    for (int guard = 0; guard < 2 && audio_->musicQueued() < 2; ++guard) {
        const std::string seg = music_->next();
        if (seg.empty()) break;
        const std::string path = findMusicFile(seg);
        std::vector<uint8_t> bytes;
        if (path.empty() || !readFile(path, bytes)) {
            static std::vector<std::string> warned;
            if (std::find(warned.begin(), warned.end(), seg) == warned.end()) {
                warned.push_back(seg);
                LOGW("Music segment %s missing", seg.c_str());
            }
            continue;
        }
        audio_->queueMusic(decodeMusicRaw(bytes.data(), bytes.size()), 22050);
    }
    if (want.empty() && audio_->musicQueued() > 0 && streamLeft_ > 0) audio_->clearMusic();
}

bool GameApp::playFlickSoundtrack(const std::string& flick) {
    if (!audio_) return false;
    std::vector<uint8_t> bytes;
    if (!assetWorld_->fs().read("flk_" + flick + ".bin", bytes)) return false;
    // The flick names its stream as a NUL-terminated "f_..." string.
    std::string stream;
    for (size_t i = 0; i + 3 < bytes.size() && stream.empty(); ++i) {
        if ((i == 0 || bytes[i - 1] == 0) && (bytes[i] == 'f' || bytes[i] == 'F') && bytes[i + 1] == '_') {
            size_t e = i;
            while (e < bytes.size() && bytes[e] >= 0x20 && bytes[e] < 0x7f) ++e;
            if (e < bytes.size() && bytes[e] == 0 && e - i > 2) stream = toLower(std::string(bytes.begin() + long(i), bytes.begin() + long(e)));
        }
    }
    if (stream.empty()) return false;
    return playStream(stream);
}

bool GameApp::playStream(const std::string& streamIn, bool cue) {
    if (!audio_) return false;
    const std::string stream = toLower(streamIn);
    if (!streamsIndexed_) {
        // Soundtracks and effects in Stream/<World>/, voice clips in StreamEnglish/<World>/.
        // Voice clips live in StreamEnglish/Newstreams (baz006, tim001, ...), effects and soundtracks
        // in Stream/<World>/ (indexFiles also walks one level of subfolders).
        for (const char* rel : {"/../Stream/", "/Stream/"}) indexFiles(dataDir_ + rel + worldName_, ".wav", streamFiles_);
        for (const char* rel : {"/../StreamEnglish", "/StreamEnglish"}) indexFiles(dataDir_ + rel, ".wav", streamFiles_);
        streamsIndexed_ = true;
    }
    for (const auto& f : streamFiles_) {
        if (f.first != stream) continue;
        const std::string key = "stream_" + stream;
        if (cue) {
            if (!audio_->hasSound(key)) {
                std::vector<uint8_t> wav;
                WavData w;
                std::string err;
                if (!readFile(f.second, wav) || !parseWav(wav.data(), wav.size(), w, &err)) break;
                audio_->addSound(key, std::move(w));
            }
            cueVoices_.push_back(audio_->play2D(key.c_str(), 1.0f));
            LOGI("Cutscene cue %s", stream.c_str());
            return true;
        }
        std::vector<uint8_t> wav;
        WavData w;
        std::string err;
        if (!readFile(f.second, wav) || !parseWav(wav.data(), wav.size(), w, &err)) break;
        const float seconds = float(w.mono.size()) / float(w.rate);
        audio_->addSound("stream_" + stream, std::move(w));
        streamVoice_ = audio_->play2D(("stream_" + stream).c_str(), 1.0f);
        streamLeft_ = seconds;
        audio_->clearMusic();
        LOGI("Cutscene soundtrack %s (%.0f s)", stream.c_str(), seconds);
        return true;
    }
    LOGW("Cutscene %s %s not found (copy the game's Stream and StreamEnglish folders next to Bin)", cue ? "sound" : "soundtrack",
         stream.c_str());
    return false;
}

}  // namespace gvr
