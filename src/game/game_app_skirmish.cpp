// Skirmish: the original's multiplayer maps (w_M_*: 3-way, Meccaryn and Reaper maps) played against
// computer players. The teams come from the map's base templates (mecc_base_template_1,
// rp_base_template_1) and Kabuto's eat pad; the markers round each base are its spawn points; each
// team's flag holder (the Smartie smborjoyzee with sm_flag1..3) stands at its base. Rules and bots:
// game/skirmish.h (ours; the original's multiplayer logic is native code).
//
// Started by loading a multiplayer world (Options / GVR_SKIRMISH="<team>,<players per team>").
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "assets/gtext.h"
#include "core/log.h"
#include "core/prof.h"
#include "game/arm_ik.h"
#include "game/cutscene.h"
#include "game/hero_clips.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
const char* kTeamUpper[3] = {"BLUE", "GREEN", "RED"};
}  // namespace

std::string GameApp::skText(const char* key, const char* fallback) const {
    if (gameText_) {
        const std::string s = gameText_->get(key);
        if (!s.empty()) return s;
    }
    return fallback;
}

std::string GameApp::skName(int id) const {
    if (id == skLocal_) return "YOU";
    const SkPlayer* p = sk_.player(id);
    return p ? p->name : std::string("SOMEONE");
}

bool GameApp::skAddBody(int id) {
    const SkPlayer* p = sk_.player(id);
    if (!p || p->local || !assetWorld_ || size_t(p->team) >= sk_.teams.size()) return false;
    if (size_t(id) >= skBodies_.size()) skBodies_.resize(size_t(id) + 1);
    SkBody& b = skBodies_[size_t(id)];
    if (b.model) return true;
    const SkHero sh = sk_.teams[size_t(p->team)].hero;
    const Hero h = sh == SkHero::Reaper ? Hero::Delphi : (sh == SkHero::Kabuto ? Hero::Kabuto : Hero::Mecc);
    AssetWorld* w = assetWorld_.get();
    b.hero = h;
    b.model = std::make_unique<SkinnedModel>();
    const char* base = h == Hero::Delphi ? "rp_l0" : (h == Hero::Kabuto ? "kb_l0" : "mc_l0");
    const char* const* clips = h == Hero::Delphi ? kDelphiClips : (h == Hero::Kabuto ? kKabutoClips : kMeccClips);
    if (!b.model->load(w->fs(), *w->models(), renderer_, base, clips[0])) {
        LOGW("Skirmish: %s did not load", base);
        b.model.reset();
        return false;
    }
    for (int i = 1; i < int(PlayerAnim::Count); ++i) b.clip[i] = std::max(0, b.model->addAnimation(w->fs(), clips[i]));
    b.die = b.model->addAnimation(w->fs(), h == Hero::Delphi ? "rp_die.anm" : (h == Hero::Kabuto ? "kb_die.anm" : "mc_die.anm"));
    if (h == Hero::Kabuto) { loadKabuto(); b.lift = kabutoFeet_; }
    return true;
}

// A joiner's player on this host: on the side it asked for (else the one with fewest people),
// moved by its own headset.
int GameApp::skAddRemote(const std::string& name, int team) {
    if (sk_.players.size() >= 16) return -1;
    if (team < 0 || size_t(team) >= sk_.teams.size()) {
        team = 0;
        size_t fewest = 1000;
        for (size_t t = 0; t < sk_.teams.size(); ++t) {
            size_t n = 0;
            for (const auto& p : sk_.players) n += p.team == int(t) && !p.bot;
            if (n < fewest) { fewest = n; team = int(t); }
        }
    }
    const SkHero sh = sk_.teams[size_t(team)].hero;
    const Hero h = sh == SkHero::Reaper ? Hero::Delphi : (sh == SkHero::Kabuto ? Hero::Kabuto : Hero::Mecc);
    if (h == Hero::Kabuto) loadKabuto();
    PlayerController pc;
    float scale = 1, lift = 0;
    heroBody(h, pc, scale, lift);
    pc.groundHeight = player_.groundHeight;
    pc.seaLevel = player_.seaLevel;
    const int id = sk_.addPlayer(name, team, false, false, pc, 100.0f);
    sk_.players[size_t(id)].remote = true;
    sk_.players[size_t(id)].name = name + " " + std::to_string(id);  // (every headset says QUEST)
    skAddBody(id);
    skBanner_ = sk_.players[size_t(id)].name + " JOINED TEAM " + kTeamUpper[team % 3];
    skBannerT_ = 3.0f;
    return id;
}

// The local player's new shots (its gun's bolts, her arrows) - drawn for the others.
void GameApp::skOwnShots() {
    const size_t fired = weapon_.shotsFired();
    if (fired <= skShotsSeen_) { skShotsSeen_ = fired; return; }
    const size_t n = std::min<size_t>(fired - skShotsSeen_, 4);
    skShotsSeen_ = fired;
    const auto& pr = weapon_.projectiles();
    const SkPlayer* me = sk_.player(skLocal_);
    for (size_t k = pr.size() >= n ? pr.size() - n : 0; k < pr.size(); ++k) {
        SkShot s;
        s.shooter = skLocal_;
        s.team = me ? me->team : 0;
        s.pos = pr[k].pos;
        s.vel = pr[k].vel;
        s.hero = hero_ == Hero::Delphi ? SkHero::Reaper : SkHero::Mecc;
        if (skNet_ == SkNetMode::Host) skHost_.localShot(s);
        else if (skNet_ == SkNetMode::Join) skClient_.shot(s);
    }
}

void GameApp::initSkirmish() {
    if (skirmish_ && toLower(worldName_).rfind("m_", 0) != 0) {  // left the multiplayer maps
        skHost_.stop();
        skClient_.leave();
        skNet_ = SkNetMode::Off;
    }
    skirmish_ = false;
    skBodies_.clear();
    sk_ = Skirmish{};
    skLocal_ = -1;
    if (!assetWorld_ || title_) return;
    if (toLower(worldName_).rfind("m_", 0) != 0) return;
    // The teams: each base template (and Kabuto's eat pad) in the map, in the original's team order.
    const ObjectRegistry& reg = assetWorld_->registry();
    struct BaseAt { SkHero hero; Vec3 pos; };
    std::vector<BaseAt> bases;
    std::vector<Vec3> markers;
    for (const auto& o : assetWorld_->world().objects) {
        const std::string n = toLower(reg.modelName(o.type));
        const Vec3 p = giantsToRuntime(o.x, o.y, o.z);
        if (n == "mecc_base_template_1") bases.push_back({SkHero::Mecc, p});
        else if (n == "rp_base_template_1") bases.push_back({SkHero::Reaper, p});
        else if (n == "kabutopad") bases.push_back({SkHero::Kabuto, p});
        else if (n == "marker") markers.push_back(p);
    }
    std::stable_sort(bases.begin(), bases.end(), [](const BaseAt& a, const BaseAt& b) { return int(a.hero) < int(b.hero); });
    if (bases.size() < 2) {
        LOGW("Skirmish: %s has %zu bases - not a multiplayer map", worldName_.c_str(), bases.size());
        return;
    }
    if (bases.size() > 3) bases.resize(3);
    AssetWorld* w = assetWorld_.get();
    // The base templates are layout gizmos (base building reads their .anm), not scenery.
    for (auto& wo : w->worldObjects()) {
        const std::string n = toLower(wo.name);
        if (n != "mecc_base_template_1" && n != "rp_base_template_1") continue;
        wo.hidden = true;
        w->collisionMutable().setEnabled(wo.collision, false);  // (it had collision: bots walked into it)
    }
    sk_.groundAt = [w](float x, float z) {
        float y;
        return w->terrain().heightAt(x, z, y) ? y : -20.0f;
    };
    sk_.collision = &w->collision();
    sk_.seaLevel = player_.seaLevel;
    std::vector<SkHero> heroes;
    std::vector<Vec3> basePos;
    for (const auto& b : bases) { heroes.push_back(b.hero); basePos.push_back(b.pos); }
    sk_.setup(heroes, basePos, markers);
    // Which team the player is on, how many a side: GVR_SKIRMISH="<team>,<per team>" (default 0, 3).
    int myTeam = 0, perTeam = skPerTeam_;
    if (const char* c = std::getenv("GVR_SKIRMISH")) std::sscanf(c, "%d,%d", &myTeam, &perTeam);
    if (skWantTeam_ >= 0) myTeam = skWantTeam_;  // chosen in the main menu
    // Online: GVR_NET=host, or join:<a.b.c.d[:port]> (test hook); else what the menu chose.
    if (const char* n = std::getenv("GVR_NET")) {
        const std::string net = n;
        if (net == "host") skNet_ = SkNetMode::Host;
        else if (net.rfind("join:", 0) == 0) {
            NetAddr a;
            if (NetAddr::parse(net.substr(5), kSkNetPort, a) && skClient_.state != SkNetClient::State::Joined) {
                skClient_.join(a, "QUEST", myTeam);
                // (in the harness: wait for the welcome here, as the menu does before loading)
                SkNetSelf none;
                Skirmish scratch;
                for (int k = 0; k < 300 && skClient_.state == SkNetClient::State::Joining; ++k) {
                    skClient_.update(0.01f, none, scratch);
                    struct timespec ts{0, 10000000};
                    nanosleep(&ts, nullptr);
                }
            }
            skNet_ = SkNetMode::Join;
        }
    } else {
        skNet_ = skNetWant_ == 1 ? SkNetMode::Host : (skNetWant_ == 2 ? SkNetMode::Join : SkNetMode::Off);
    }
    if (skNet_ == SkNetMode::Join) {
        if (skClient_.state != SkNetClient::State::Joined) {
            LOGW("Skirmish: not joined (no answer from the host) - playing against computer players");
            skNet_ = SkNetMode::Off;
        } else {
            myTeam = std::max(0, std::min(int(heroes.size()) - 1, skClient_.myTeam));
            if (toLower(skClient_.map) != toLower(worldName_)) LOGW("Skirmish: the host plays %s, this is %s", skClient_.map.c_str(), worldName_.c_str());
        }
    }
    myTeam = std::max(0, std::min(int(heroes.size()) - 1, myTeam));
    perTeam = std::max(1, std::min(6, perTeam));
    auto heroOf = [](SkHero h) { return h == SkHero::Reaper ? Hero::Delphi : (h == SkHero::Kabuto ? Hero::Kabuto : Hero::Mecc); };
    setHero(heroOf(heroes[size_t(myTeam)]));
    player_.jetpackAvailable = hero_ == Hero::Mecc;
    if (heroes.end() != std::find(heroes.begin(), heroes.end(), SkHero::Kabuto)) loadKabuto();  // his size for his bot
    if (skNet_ == SkNetMode::Join) {
        // A joiner: the host's id and team (from its welcome); the rest is a mirror of the host's game.
        sk_.mirror = true;
        skLocal_ = skClient_.myId;
        SkPlayer& me = sk_.mirrorPlayer(skLocal_);
        me.local = true;
        me.alive = true;
        me.team = myTeam;
        me.name = "YOU";
        me.body = player_;
        me.maxHp = me.hp = 100.0f;
    } else {
        // (the name the others see; this headset shows "YOU")
        skLocal_ = sk_.addPlayer(skNet_ == SkNetMode::Host ? "HOST" : "YOU", myTeam, false, true, player_, 100.0f);
    }
    skBodies_.resize(size_t(skLocal_) + 1);
    // The computer players: a full side each (Kabuto plays alone), the player's side one fewer.
    // A joiner has none: the host's come with its snapshots.
    for (size_t t = 0; t < heroes.size() && skNet_ != SkNetMode::Join; ++t) {
        const int n = heroes[t] == SkHero::Kabuto ? (int(t) == myTeam ? 0 : 1) : (int(t) == myTeam ? perTeam - 1 : perTeam);
        for (int k = 0; k < n; ++k) {
            const Hero h = heroOf(heroes[t]);
            PlayerController pc;
            float scale = 1, lift = 0;
            heroBody(h, pc, scale, lift);
            pc.groundHeight = player_.groundHeight;
            pc.seaLevel = player_.seaLevel;
            char name[32];
            std::snprintf(name, sizeof name, "%s %d", kTeamUpper[t % 3], k + 1 + (int(t) == myTeam ? 1 : 0));
            const int id = sk_.addPlayer(name, int(t), true, false, pc, h == Hero::Kabuto ? 500.0f : 100.0f);
            skAddBody(id);
        }
    }
    // The flag holders: the Smartie with his team's flag (sm_flag1..3) at each base that has one.
    for (size_t t = 0; t < heroes.size(); ++t) {
        skHolder_[t].reset();
        skFlag_[t] = nullptr;
        if (!sk_.flags[t].used) continue;
        auto m = std::make_unique<SkinnedModel>();
        if (m->load(w->fs(), *w->models(), renderer_, "smborjoyzee_l0", "Smborjoyzee_flag.anm")) {
            skHolderClip_[t][0] = 0;
            skHolderClip_[t][1] = std::max(0, m->addAnimation(w->fs(), "Smborjoyzee_flag_fly.anm"));
            skHolderClip_[t][2] = std::max(0, m->addAnimation(w->fs(), "smborjoyzee_flag_fly_rp.anm"));
            skHolder_[t] = std::move(m);
        }
        const std::string flag = "sm_flag" + std::to_string(t + 1) + ".gbs";
        skFlag_[t] = w->models()->get(flag, "sm_flag.anm", /*dynamicLit=*/true);
    }
    skBow_ = w->models()->get("rp_bow.gbs", "", /*dynamicLit=*/true);
    const Vec3 at = sk_.spawnPoint(skLocal_);
    const Vec3 mid = (basePos[0] + basePos[1]) * 0.5f;
    teleportPlayer(at, std::atan2(-(mid.x - at.x), -(mid.z - at.z)));
    sk_.setLocal(skLocal_, player_.position, player_.yaw, playerHealth_, true);
    skirmish_ = true;
    skRoundT_ = 0;
    skShotsSeen_ = weapon_.shotsFired();
    skLastHurtBy_ = -1;
    if (skNet_ == SkNetMode::Host) {
        skHost_.addRemote = [this](const std::string& name, int team) { return skAddRemote(name, team); };
        skHost_.removeRemote = [this](int id) {
            if (SkPlayer* p = sk_.player(id)) {
                skBanner_ = p->name + " LEFT THE GAME";
                skBannerT_ = 3.0f;
                p->name += " (LEFT)";
                sk_.removePlayer(id);
            }
        };
        if (!skHost_.start(worldName_, &sk_)) skNet_ = SkNetMode::Off;
    }
    std::string vs;
    for (size_t t = 0; t < sk_.teams.size(); ++t) vs += (t ? " VS " : "") + std::string(kTeamUpper[t % 3]);
    skBanner_ = "SKIRMISH: " + vs + " - " + std::to_string(sk_.rules.captureLimit) + " CAPTURES OR " + std::to_string(sk_.rules.killLimit) +
                " KILLS WIN. YOU ARE " + kTeamUpper[myTeam % 3];
    skBannerT_ = 6.0f;
    int bots = 0;
    for (const auto& p : sk_.players) bots += p.bot;
    LOGI("Skirmish on %s: %zu teams, %d computer players; you are %s (%s)", worldName_.c_str(), sk_.teams.size(), bots,
         sk_.teams[size_t(myTeam)].name.c_str(), hero_ == Hero::Mecc ? "Meccaryn" : (hero_ == Hero::Delphi ? "Sea Reaper" : "Kabuto"));
    for (size_t t = 0; t < sk_.teams.size(); ++t)
        LOGI("  team %s: base (%.0f %.0f %.0f), %zu spawn points, flag %s", sk_.teams[t].name.c_str(), sk_.teams[t].base.x, sk_.teams[t].base.y,
             sk_.teams[t].base.z, sk_.teams[t].spawns.size(), sk_.flags[t].used ? "yes" : "none (Kabuto)");
}

bool GameApp::skirmishRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& target) const {
    if (!skirmish_) return false;
    bool hit = false;
    for (const auto& p : sk_.players) {
        if (!p.alive || p.local) continue;
        // A capsule: the body's axis, its radius (a little generous: they move).
        const float r = std::max(0.45f, p.body.radius * 1.4f);
        const Vec3 a = p.body.position + Vec3{0, std::min(r, 0.5f * p.body.height), 0};
        const Vec3 b = p.body.position + Vec3{0, std::max(0.5f * p.body.height, p.body.height - r), 0};
        // Closest approach of the ray to the segment ab, sampled along the ray near it.
        const Vec3 c = (a + b) * 0.5f;
        const float tc = dot(c - o, d);
        if (tc < 0 || tc > maxD + r + 0.5f * p.body.height) continue;
        for (int k = -8; k <= 8; ++k) {
            const float tt = tc + float(k) * (0.125f * (r + 0.5f * p.body.height));
            if (tt < 0 || tt > maxD || (hit && tt >= t)) continue;
            const Vec3 q = o + d * tt;
            const Vec3 ab = b - a;
            const float u = std::max(0.0f, std::min(1.0f, dot(q - a, ab) / std::max(1e-4f, dot(ab, ab))));
            if (length(q - (a + ab * u)) < r) {
                t = tt;
                target = kBotTarget + p.id;
                hit = true;
                break;
            }
        }
    }
    return hit;
}

void GameApp::skirmishHit(int id, float amount, const Vec3& at, const Vec3& dir) {
    SkPlayer* p = sk_.player(id);
    if (!p || !p->alive) return;
    const bool wasAlive = p->alive;
    if (skNet_ == SkNetMode::Join) skClient_.hit(id, amount, at - dir);  // the host decides
    else sk_.damage(id, amount, skLocal_, at - dir);
    bleed(at, dir * -1.0f, wasAlive && !p->alive, p->body.height > 5.0f ? 3.0f : 1.0f);
    haptics_.push_back({rig_.turnHand(), 0.4f, 0.04f});
}

void GameApp::skirmishBlast(const Vec3& at, float radius, float damage) {
    if (!skirmish_) return;
    for (auto& p : sk_.players) {
        if (!p.alive || p.local) continue;
        const Vec3 c = p.body.position + Vec3{0, 0.5f * std::min(p.body.height, 2.0f), 0};
        const float d = std::max(0.0f, length(c - at) - p.body.radius);
        if (d >= radius) continue;
        const bool wasAlive = p.alive;
        const float dmg = damage * (1.0f - 0.6f * d / radius);
        if (skNet_ == SkNetMode::Join) skClient_.hit(p.id, dmg, at);
        else sk_.damage(p.id, dmg, skLocal_, at);
        bleed(c, normalize(c - at + Vec3{0, 0.5f, 0}), wasAlive && !p.alive, 1.0f);
    }
}

void GameApp::skHandleEvent(const SkEvent& e) {
    auto flagName = [](int team) { return std::string("THE ") + kTeamUpper[team % 3] + " FLAG"; };
    auto say = [&](const std::string& s, float secs) {
        skBanner_ = s;
        skBannerT_ = secs;
        LOGI("Skirmish: %s", s.c_str());
    };
    auto fill = [](std::string fmt, const std::string& a, const std::string& b) {
        // The original's lines are printf formats with %s.
        for (const std::string* arg : {&a, &b}) {
            const size_t k = fmt.find("%s");
            if (k == std::string::npos) break;
            fmt.replace(k, 2, *arg);
        }
        return fmt;
    };
    switch (e.kind) {
        case SkEvent::HurtLocal:
            if (e.b != skLocal_) break;  // (a joiner's: the host sent it on)
            skLastHurtBy_ = e.a;
            hitFrom_ = e.at;
            hitDirTimer_ = 1.2f;
            damagePlayer(e.amount, DeathCause::Shot);
            break;
        case SkEvent::Kill: {
            const SkPlayer* v = sk_.player(e.b);
            if (v && !v->local && size_t(e.b) < skBodies_.size()) {
                const Hero h = skBodies_[size_t(e.b)].hero;
                sfx(h == Hero::Kabuto ? "ka_deth1" : (h == Hero::Delphi ? "rpman_die1" : "me_shrt3"), e.at, 1.0f, h == Hero::Mecc ? 0.8f : 1.0f);
                blood_.pool(e.at, h == Hero::Kabuto ? 6.0f : 1.4f);
            }
            say(e.a >= 0 ? fill(skText("mpchkilled1", "%s was killed by %s."), skName(e.b), skName(e.a))
                         : fill(skText("mpchsuicide1", "%s died."), skName(e.b), ""),
                3.0f);
            break;
        }
        case SkEvent::FlagTaken:
            say(fill(skText("mpchflagtaken1", "%s has %s!"), skName(e.a), flagName(e.team)), 3.5f);
            sfx2D("pu_hlth", 0.6f, 1.3f);
            break;
        case SkEvent::FlagDropped: say(skName(e.a) + " DROPPED " + flagName(e.team), 3.0f); break;
        case SkEvent::FlagReturned:
            say(e.a >= 0 ? fill(skText("mpchflagret1", "%s has returned %s!"), skName(e.a), flagName(e.team)) : flagName(e.team) + " IS BACK",
                3.0f);
            break;
        case SkEvent::FlagCaptured:
            say(fill(skText("mpchflagcap1", "%s has captured %s!"), skName(e.a), flagName(e.team)), 4.0f);
            sfx2D("pu_hlth", 1.0f, 0.8f);
            break;
        case SkEvent::Fire:
            if (const SkPlayer* p = sk_.player(e.a); p && size_t(e.a) < skBodies_.size())
                sfx(skBodies_[size_t(e.a)].hero == Hero::Delphi ? "sr_rbwsh" : ((e.a & 1) ? "me_peas1" : "me_peas2"), e.at, 0.6f,
                    0.95f + 0.1f * frand());
            break;
        case SkEvent::Stomp:
            dustBurst(e.at, 6.0f, 18);
            sfx("ka_stomp", e.at, 1.0f, 0.9f + 0.15f * frand());
            break;
        case SkEvent::Win:
            say(std::string(kTeamUpper[e.team % 3]) + " TEAM WINS!", 10.0f);
            sfx2D("pu_hlth", 1.0f, 0.6f);
            skRoundT_ = 0;
            break;
        case SkEvent::Respawn: break;
    }
}

void GameApp::updateSkirmish(float dt) {
    if (!skirmish_) return;
    skAnimT_ += dt;
    skBannerT_ = std::max(0.0f, skBannerT_ - dt);
    sk_.setLocal(skLocal_, player_.position, player_.yaw, playerHealth_, !dying_ && playerHealth_ > 0);
    // A won round: the result for a while, then a fresh one (scores cleared, everyone respawned).
    // A joiner's round is the host's: it only goes back to its base when the next one starts.
    if (skNet_ == SkNetMode::Join && skPrevWinner_ >= 0 && sk_.winner < 0) {
        if (!dying_) teleportPlayer(sk_.spawnPoint(skLocal_), player_.yaw);
        playerHealth_ = 100.0f;
        skBanner_ = "NEW ROUND";
        skBannerT_ = 3.0f;
    }
    skPrevWinner_ = sk_.winner;
    if (sk_.winner >= 0 && skNet_ != SkNetMode::Join) {
        skRoundT_ += dt;
        if (skRoundT_ > 10.0f) {
            for (auto& t : sk_.teams) t.captures = t.kills = t.deaths = 0;
            for (auto& f : sk_.flags) { f.carrier = -1; f.atHome = true; f.pos = f.home; }
            for (auto& p : sk_.players) {
                p.kills = p.deaths = p.captures = 0;
                p.carrying = -1;
                if (!p.local) sk_.spawn(p.id);
            }
            sk_.winner = -1;
            skRoundT_ = 0;
            if (!dying_) teleportPlayer(sk_.spawnPoint(skLocal_), player_.yaw);
            playerHealth_ = 100.0f;
            skBanner_ = "NEW ROUND";
            skBannerT_ = 3.0f;
            LOGI("Skirmish: new round");
        }
    }
    sk_.update(dt);
    // Every 15 s: where everyone is (logs for tests).
    static const float traceEvery = std::getenv("GVR_SK_TRACE") ? float(std::atof(std::getenv("GVR_SK_TRACE"))) : 15.0f;  // test hook
    if (std::floor(sk_.time / traceEvery) != std::floor((sk_.time - dt) / traceEvery)) {
        std::string s;
        for (const auto& p : sk_.players) {
            char b[128];
            const Vec3 nx = p.routeAt < p.route.size() ? p.route[p.routeAt] : p.moveTo;
            std::snprintf(b, sizeof b, " %s%s(%.0f %.0f %.0f g%.1f)->%.0fm%s%s t%d r%zu/%zu(%.0f %.0f %.0f)", p.name.c_str(), p.alive ? "" : "+",
                          p.body.position.x, p.body.position.y, p.body.position.z, sk_.groundAt ? sk_.groundAt(p.body.position.x, p.body.position.z) : 0.0f,
                          length(p.moveTo - p.body.position),
                          p.body.swimming() ? " swim" : "", p.body.onGround ? "" : " air", p.target, p.routeAt, p.route.size(), nx.x, nx.y, nx.z);
            s += b;
        }
        LOGI("Skirmish %.1f s:%s", sk_.time, s.c_str());
    }
    // The network: the host passes its game on; a joiner takes the host's in.
    if (skNet_ == SkNetMode::Host) {
        skOwnShots();
        skHost_.update(dt);
    } else if (skNet_ == SkNetMode::Join) {
        skOwnShots();
        SkNetSelf self;
        self.pos = player_.position;
        self.vel = player_.velocity;
        self.yaw = player_.yaw;
        self.anim = uint8_t(player_.anim());
        self.prevAnim = uint8_t(player_.prevAnim());
        self.animT = player_.animTime();
        self.prevT = player_.prevAnimTime();
        self.fade = player_.fadeWeight();
        self.hp = playerHealth_;
        self.alive = !dying_ && playerHealth_ > 0;
        self.lastAttacker = skLastHurtBy_;
        self.aim = gunValid_ ? normalize(Vec3{-gunXf_.m[8], -gunXf_.m[9], -gunXf_.m[10]}) : headWorldRot_.rotate(Vec3{0, 0, -1});
        skClient_.update(dt, self, sk_);
        for (const auto& e : skClient_.events) sk_.events.push_back(e);
        skClient_.events.clear();
        for (size_t i = 0; i < sk_.players.size(); ++i) skAddBody(int(i));  // new arrivals
        if (skClient_.state == SkNetClient::State::Lost) {
            skLostT_ += dt;
            if (skBannerT_ <= 0) { skBanner_ = "CONNECTION TO THE HOST LOST"; skBannerT_ = 3.0f; }
        }
    }
    for (const auto& e : sk_.events) skHandleEvent(e);
    sk_.events.clear();
    // The bodies: posed from their controllers (as the player's), dead ones playing their death clip.
    GVR_PROF("skirmish pose");
    const Vec3 eye = player_.position;
    for (const auto& p : sk_.players) {
        if (p.local || size_t(p.id) >= skBodies_.size()) continue;
        SkBody& b = skBodies_[size_t(p.id)];
        if (!b.model) continue;
        b.xf = Mat4::translation(p.body.position + Vec3{0, b.lift, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, p.body.yaw));
        const float far = b.hero == Hero::Kabuto ? 700.0f : 200.0f;
        if (length(p.body.position - eye) > far) continue;
        if (!p.alive) {
            if (b.die >= 0) {
                const float d = b.model->animDuration(b.die);
                const float tt = std::min(p.deadT, d * 0.999f);
                b.model->poseBlend(renderer_, b.die, tt, b.die, tt, 0.0f);
            }
            b.handValid = false;
            continue;
        }
        if (p.remote) {  // as its own headset (or the host) animates it
            const int n = int(PlayerAnim::Count);
            b.model->poseBlend(renderer_, b.clip[std::min<int>(n - 1, p.netPrev)], p.netPrevT, b.clip[std::min<int>(n - 1, p.netAnim)], p.netT,
                               std::max(0.0f, std::min(1.0f, p.netFade)));
        } else {
            b.model->poseBlend(renderer_, b.clip[int(p.body.prevAnim())], p.body.prevAnimTime(), b.clip[int(p.body.anim())], p.body.animTime(),
                               p.body.fadeWeight());
        }
        Mat4 ref;
        b.handValid = b.hero != Hero::Kabuto && b.model->nodeWorld("ref_rhand", ref);
        if (b.handValid) {
            const Vec4 hp = b.xf * Vec4{-ref.m[12] * gGameUnitsToMeters, ref.m[14] * gGameUnitsToMeters, ref.m[13] * gGameUnitsToMeters, 1.0f};
            b.hand = Vec3{hp.x, hp.y, hp.z};
        }
    }
    // The flag holders' clips.
    for (size_t t = 0; t < sk_.flags.size() && t < 3; ++t) {
        if (!skHolder_[t]) continue;
        const SkFlag& f = sk_.flags[t];
        const SkPlayer* c = sk_.player(f.carrier);
        const int k = c ? (sk_.teams[size_t(c->team)].hero == SkHero::Reaper ? 2 : 1) : 0;
        const int clip = skHolderClip_[t][k];
        const float d = std::max(0.1f, skHolder_[t]->animDuration(clip));
        skHolder_[t]->poseBlend(renderer_, clip, std::fmod(skAnimT_, d), clip, std::fmod(skAnimT_, d), 0.0f);
    }
}

void GameApp::renderSkirmish(bool shadow, const Vec3& eye) {
    if (!skirmish_) return;
    for (const auto& p : sk_.players) {
        if (p.local || size_t(p.id) >= skBodies_.size()) continue;
        const SkBody& b = skBodies_[size_t(p.id)];
        if (!b.model) continue;
        if (!p.alive && p.deadT > 4.0f) continue;  // the body is gone (the respawn comes at 5 s)
        const float far = b.hero == Hero::Kabuto ? 700.0f : 200.0f;
        if (length(p.body.position - eye) > far) continue;
        if (shadow) {
            b.model->renderShadow(renderer_, b.xf);
            continue;
        }
        b.model->render(renderer_, b.xf);
        // The gun / bow in the right hand, pointing where it aims.
        if (b.handValid && p.alive) {
            const RenderModel* held = b.hero == Hero::Delphi ? skBow_ : gunModel_;
            if (!held) continue;
            const Vec3 f = normalize(p.aim);
            const Quat look = Quat::fromAxisAngle({0, 1, 0}, std::atan2(-f.x, -f.z)) *
                              Quat::fromAxisAngle({1, 0, 0}, std::asin(std::max(-1.0f, std::min(1.0f, f.y))));
            const float k = b.hero == Hero::Delphi ? 0.9f : 0.75f;
            const Mat4 xf = Mat4::translation(b.hand) * Mat4::rotation(look) * Mat4::scale({k, k, k}) *
                            Mat4::translation((held->boundsMin + held->boundsMax) * -0.5f);
            drawModel(held, xf, false);
        }
    }
    if (shadow) return;
    // Their shots: the Meccaryn's bolt spark, the Reaper's arrow streak.
    for (const auto& s : sk_.shots) {
        if (length(s.pos - eye) > 200.0f) continue;
        const Vec3 f = normalize(s.vel);
        if (s.hero == SkHero::Reaper && arrowModel_) {
            const Quat look = Quat::fromAxisAngle({0, 1, 0}, std::atan2(-f.x, -f.z)) *
                              Quat::fromAxisAngle({1, 0, 0}, std::asin(std::max(-1.0f, std::min(1.0f, f.y))));
            drawModel(arrowModel_, Mat4::translation(s.pos) * Mat4::rotation(look) * Mat4::scale({0.25f, 0.25f, 0.25f}), true, 0.9f);
        } else if (boltModel_) {
            drawModel(boltModel_, Mat4::translation(s.pos) * Mat4::rotation(rotationBetween({0, 0, -1}, f)), true, 1.0f);
        }
    }
    // The flag holders: standing at the base, or riding the carrier's back.
    for (size_t t = 0; t < sk_.flags.size() && t < 3; ++t) {
        const SkFlag& f = sk_.flags[t];
        if (!f.used || length(f.pos - eye) > 300.0f) continue;
        const SkPlayer* c = sk_.player(f.carrier);
        Mat4 xf;
        if (c) {
            const Quat r = Quat::fromAxisAngle({0, 1, 0}, c->body.yaw);
            const Vec3 back = c->body.position + Vec3{0, 0.55f * c->body.height, 0} + r.rotate(Vec3{0, 0, 0.35f});
            xf = Mat4::translation(back) * Mat4::rotation(r * Quat::fromAxisAngle({0, 1, 0}, 3.14159f));
        } else {
            xf = Mat4::translation(f.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, float(t) * 2.1f));
        }
        if (skHolder_[t]) skHolder_[t]->render(renderer_, xf);
        // The flag upright in his hand (its pole held a third of the way up).
        if (skFlag_[t]) {
            Mat4 h;
            Vec3 at = Vec3{xf.m[12], xf.m[13], xf.m[14]} + Vec3{0, 0.9f, 0};
            if (skHolder_[t] && (skHolder_[t]->nodeWorld("ref_lhand01", h) || skHolder_[t]->nodeWorld("r_hand", h))) {
                const Vec4 p = xf * Vec4{-h.m[12] * gGameUnitsToMeters, h.m[14] * gGameUnitsToMeters, h.m[13] * gGameUnitsToMeters, 1.0f};
                at = Vec3{p.x, p.y, p.z};
            }
            const Quat r = Quat::fromAxisAngle({0, 1, 0}, yawOf(Quat{}) + float(t) * 2.1f);
            drawModel(skFlag_[t], Mat4::translation(at - Vec3{0, 0.35f, 0}) * Mat4::rotation(r), false);
        }
    }
}

// The scores, head-locked at the top left; the latest message in the middle.
void GameApp::renderSkirmishHud() {
    if (!skirmish_ || !text_.ready() || cutsceneActive()) return;
    const Pose head = rig_.toWorld(lastInput_.head);
    const Quat r = Quat::fromAxisAngle({0, 1, 0}, yawOf(head.orientation));
    std::string s;
    for (size_t t = 0; t < sk_.teams.size(); ++t) {
        char line[64];
        std::snprintf(line, sizeof line, "%s%s %d  (%d K)", t ? "   " : "", kTeamUpper[t % 3], sk_.teams[t].captures, sk_.teams[t].kills);
        s += line;
    }
    if (const SkPlayer* me = sk_.player(skLocal_)) {
        char line[64];
        std::snprintf(line, sizeof line, "\nYOU: %d KILLS %d DEATHS", me->kills, me->deaths);
        s += line;
        if (me->carrying >= 0) s += std::string("\nYOU HAVE THE ") + kTeamUpper[me->carrying % 3] + " FLAG - TAKE IT HOME!";
    }
    float w = 0, h = 0;
    if (const MeshId m = text_.mesh(s, 0.035f, 1.4f, &w, &h)) {
        const Mat4 xf = Mat4::translation(head.position + r.rotate(Vec3{-0.35f, 0.42f, -1.4f})) * Mat4::rotation(r * Quat::fromAxisAngle({1, 0, 0}, -0.2f));
        text_.drawPanel(xf, w + 0.06f, h + 0.04f, {0, 0, 0, 0.45f});
        text_.draw(m, xf * Mat4::translation({0, 0, 0.003f}), {1.0f, 1.0f, 0.85f, 1.0f});
    }
    if (skBannerT_ > 0 && !skBanner_.empty()) {
        const float a = std::min(1.0f, skBannerT_ / 0.5f);
        if (const MeshId m = text_.mesh(skBanner_, 0.05f, 1.6f, &w, &h)) {
            const Mat4 xf = Mat4::translation(head.position + r.rotate(Vec3{0, 0.18f, -1.4f})) * Mat4::rotation(r);
            text_.drawPanel(xf, w + 0.08f, h + 0.05f, {0, 0, 0, 0.5f * a});
            text_.draw(m, xf * Mat4::translation({0, 0, 0.003f}), {1.0f, 0.9f, 0.5f, a});
        }
    }
}

}  // namespace gvr
