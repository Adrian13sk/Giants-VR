// Skirmish: the original's multiplayer maps (w_M_*) played against computer players ("bots"), and the
// game state online play replicates. Team games as the original's multiplayer text describes them
// (GTextEnglish: teams 1 Blue / Mecc, 2 Green / Reaper, 3 Red / Kabuto; kills and deaths; flag captures,
// the flag being a Smartie "flag holder" at each base; a team score limit). The rules' numbers and the
// bots' behaviour are ours - the original's game logic is native code (BLOCKERS B3).
//
// Platform- and renderer-independent: the game feeds in world queries and the local player's state,
// draws the players and shots, and applies the events (damage to the local player, messages).
// Positions are runtime metres (Y up).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/math.h"
#include "game/player.h"

namespace gvr {

enum class SkHero : int { Mecc = 0, Reaper = 1, Kabuto = 2 };

// A coarse walking map of the terrain for the bots' routes: A* over a grid of ground heights, with
// what each hero can cross (the Meccaryn jets up cliffs, the Sea Reaper swims, Kabuto steps over most).
class SkNav {
public:
    enum Profile { Walker = 0, Swimmer = 1, Giant = 2, Jetter = 3 };
    void build(float x0, float z0, float x1, float z1, float cell, const std::function<float(float, float)>& ground, float seaLevel);
    bool ready() const { return w_ > 0; }
    // Waypoints from `from` to `to` (the last is `to`); false when there's no way (then go straight).
    // The search is bounded: past `maxExpand` cells it returns the way to the cell nearest the goal.
    bool path(const Vec3& from, const Vec3& to, Profile prof, std::vector<Vec3>& out, int maxExpand = 4000) const;
    float cell() const { return cell_; }
    // A building's footprint (world box): no route through it.
    void block(float x0, float z0, float x1, float z1);
    // The same for a turned building: the cells whose centres lie in its footprint (local box
    // mn..mx under `xf`, grown by `margin` metres). Its axis-aligned bounds blocked far too much
    // round a turned base (the flag's own cell, inside the ring of buildings).
    void blockBox(const Mat4& xf, const Vec3& mn, const Vec3& mx, float margin);
    bool blocked(const Vec3& p) const;
    // A bot couldn't get through here (a rock, a ledge the samples missed): costlier for its kind of
    // mover, impassable after a few such reports (the map's buildings and rocks aren't in the grid).
    void penalize(const Vec3& p, Profile prof);

private:
    int w_ = 0, h_ = 0;
    float x0_ = 0, z0_ = 0, cell_ = 6, sea_ = 0;
    std::vector<float> hgt_;
    std::vector<uint8_t> blocked_;
    std::vector<uint8_t> pen_;    // per cell and profile (4): times bots got stuck going into it
    std::vector<float> up_;       // per cell and direction (8): the steepest rise along the step, sampled every half metre
    // Search scratch, kept between searches (a stamp per search instead of clearing).
    mutable std::vector<float> g_;
    mutable std::vector<int> came_;
    mutable std::vector<uint32_t> seen_, closed_;
    mutable uint32_t stamp_ = 0;
    bool cellOf(const Vec3& p, int& cx, int& cz) const;
    float stepCost(int a, int b, int dir, float dist, Profile prof) const;
};

struct SkTeam {
    SkHero hero = SkHero::Mecc;
    std::string name;            // "Blue", "Green", "Red" (the original's team names)
    Vec3 base;                   // the base template (or Kabuto's eat pad)
    std::vector<Vec3> spawns;    // markers round the base
    int captures = 0, kills = 0, deaths = 0;
};

struct SkFlag {                  // a team's flag holder (a Smartie at its base)
    bool used = true;            // Kabuto's team has none (he has no Smarties; he eats the others' on his pad)
    Vec3 home, pos;
    int carrier = -1;            // player id carrying it
    bool atHome = true;
    float droppedT = 0;          // seconds lying where its carrier fell
};

struct SkPlayer {
    int id = 0;
    std::string name;
    int team = 0;
    bool bot = true, local = false;
    bool remote = false;         // a player on another headset (online play): moved by the network
    // Animation as the network gives it (remote players; and everyone on a joiner's mirror).
    uint8_t netAnim = 0, netPrev = 0;
    float netT = 0, netPrevT = 0, netFade = 1;
    Vec3 netTarget;              // where the host last had it (a joiner's mirror eases toward it)
    PlayerController body;       // bots: driven by the skirmish; the local player: mirrored from the game
    float hp = 100, maxHp = 100;
    bool alive = true;
    float deadT = 0;             // seconds since death (respawn after rules.respawnSeconds)
    int kills = 0, deaths = 0, captures = 0;
    int carrying = -1;           // the team whose flag this player has
    // (2026-10-28 balance) Kabuto fumbles a flag after this much damage since he took it, and can't take one again
    // for noGrabT s: with no flag of his own to guard, he won every 3-way round in a bot survey (ours).
    float carryHurt = 0, noGrabT = 0;
    int lastAttacker = -1;
    float lastHurtAt = -1e9f;    // game time of its last damage (a fall long after a hit is nobody's kill)
    bool gone = false;           // left the game (a joiner): its slot can be taken by the next to join
    // Bot brain.
    enum Role { Attack, Defend } role = Attack;
    int target = -1;             // enemy player in sight
    Vec3 moveTo;
    float think = 0, fireCool = 0, stuckT = 0, detourT = 0, strafe = 0, jetT = 0, blockedT = 0;
    Vec3 lastPos;
    Vec3 aim{0, 0, -1};          // where it points (for drawing)
    std::vector<Vec3> route;     // waypoints to moveTo (SkNav)
    size_t routeAt = 0;
    Vec3 routeGoal;
    float routeAge = 0;
    float giveUpT = 0;
    Vec3 progGoal;               // the waypoint being closed on, the nearest it got, for how long not nearer
    float bestDist = 1e9f, progT = 0;           // no way to its goal at all: does something else for this long
    bool firing = false;         // fired this update (for the shot animation / sound)
};

struct SkShot {                  // a bot's bolt or arrow
    int shooter = -1, team = 0;
    Vec3 pos, vel;
    float life = 0, damage = 10, gravity = 0;
    SkHero hero = SkHero::Mecc;
    bool visual = false;         // only drawn (another player's own shot: its hits come separately)
    bool hostOwn = false;        // the host's own player's (its game draws the real one): for the joiners only
};

enum class SkSpell { FireWall, Tornado, FireCircle, Hail };

struct SkEvent {
    enum Kind { Kill, FlagTaken, FlagDropped, FlagReturned, FlagCaptured, Respawn, Fire, Stomp, HurtLocal, Win, Spell } kind = Kill;
    // Spell (2026-10-29, online): caster a, which one in team (SkSpell), its facing (yaw, rad) in amount, where in at.
    int a = -1, b = -1;          // players (Kill: killer a (-1: none), victim b; Hurt: attacker a)
    int team = -1;               // flags: whose flag; Win: the winner
    float amount = 0;
    Vec3 at;
};

struct SkRules {
    int captureLimit = 3;        // a team with this many captures wins
    int killLimit = 20;          // ...or with this many kills (0: off; 30 left most bot rounds unfinished after 15 min)
    float respawnSeconds = 5.0f;
    bool friendlyFire = false;   // the original's "Damage Teammates?" option
    float flagReturnSeconds = 30.0f;
    float captureDelay = 0.0f;   // the original's "time until flag captures are allowed"
};

class Skirmish {
public:
    std::vector<SkTeam> teams;
    std::vector<SkFlag> flags;   // one per team
    std::vector<SkPlayer> players;
    std::vector<SkShot> shots;
    std::vector<SkEvent> events; // since the game last drained them
    SkRules rules;
    int winner = -1;
    float time = 0;

    // World queries (all optional): ground height, whether a straight line is clear (sight, shots),
    // and the collision world bots walk against.
    std::function<float(float, float)> groundAt;
    std::function<bool(const Vec3&, const Vec3&)> lineClear;
    const CollisionWorld* collision = nullptr;
    SkNav nav;                   // built by setup when groundAt is set
    float seaLevel = 0.0f;
    int routesThisUpdate = 0;    // route searches are spread out: one per update

    // Teams from the map: one hero per team, its base, and the map's markers (assigned to the nearest
    // base within reach as spawn points; a ring round the base where a team gets none).
    void setup(const std::vector<SkHero>& heroes, const std::vector<Vec3>& bases, const std::vector<Vec3>& markers);
    // A player of `team` whose movement follows `proto` (the hero's parameters); returns its id.
    // After the map's buildings are blocked in the nav: which sides can walk (swim, jet) to which flags
    // and back home. A flag no route reaches is left alone (bots stood at the foot of a cliff for the
    // whole match). Without it every flag counts as reachable.
    void prepareRoutes();
    bool canReach(int team, int flag) const {
        const size_t i = size_t(team) * flags.size() + size_t(flag);
        return reach_.empty() || i >= reach_.size() || reach_[i] != 0;
    }
    int addPlayer(const std::string& name, int team, bool bot, bool local, const PlayerController& proto, float maxHp = 100);
    SkPlayer* player(int id);
    const SkPlayer* player(int id) const;
    Vec3 spawnPoint(int id) const;
    void spawn(int id);

    void update(float dt);
    // Damage from a shot, a blast or the local player's weapons. attacker -1: the world.
    void damage(int victim, float amount, int attacker, const Vec3& from);
    // The local player, mirrored each frame (its health is the game's own).
    void setLocal(int id, const Vec3& pos, float yaw, float hp, bool alive);
    void localDied(int id);      // the game's death of the local player (counted as a kill for its attacker)
    void remoteDied(int id, int killer);  // a remote player's own report of its death
    void removePlayer(int id);   // gone (a joiner left): out of the game, its flag dropped, no death counted
    // A joiner's copy: nothing is simulated here (the host's snapshots fill it); shots fly on.
    bool mirror = false;
    // Adds a player slot with a given id (a joiner's mirror of the host's players).
    SkPlayer& mirrorPlayer(int id);
    // Nearest living player of another team to `p` (-1: none) within `range`.
    int nearestEnemy(int team, const Vec3& p, float range, bool needSight) const;
    float heightOf(const SkPlayer& p) const { return p.body.height; }
    bool enemies(int a, int b) const;

private:
    void think(SkPlayer& p);
    void moveBot(SkPlayer& p, float dt);
    void fire(SkPlayer& p, const Vec3& at);
    void updateFlags(float dt);
    void updateShots(float dt);
    void kill(SkPlayer& victim, int killer);
    void dropFlag(SkPlayer& p);
    float ground(const Vec3& p) const { return groundAt ? groundAt(p.x, p.z) : 0.0f; }
    bool clear(const Vec3& a, const Vec3& b) const { return !lineClear || lineClear(a, b); }
    float touchRadius(const SkPlayer& p) const;
    uint32_t rng_ = 12345;
    float rnd();
    std::vector<uint8_t> reach_;   // [team * flags + flag]
    SkNav::Profile profileOf(int team) const;
};

}  // namespace gvr
