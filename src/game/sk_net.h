// Skirmish over the local network (Wi-Fi): one headset hosts - it runs the rules and the computer
// players (game/skirmish.h) - and the others join. Our own protocol over UDP (the original used
// DirectPlay; nothing of it is reused):
//  - the host beacons its game on the LAN once a second (broadcast); a joiner lists what it hears
//    (or is given the host's address);
//  - each joiner sends its own player's state ~30 times a second: position, facing, animation,
//    alive, aim, the hits its weapons landed and the shots it fired;
//  - the host sends everyone a snapshot ~20 times a second: every player, the flags, the scores,
//    the shots in flight, and the events (kills, flag lines, damage to that joiner's player).
// Hits, deaths and events are resent until acknowledged. Each headset is the authority for its own
// player's movement and health; the host for everything else.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/skirmish.h"
#include "net/udp.h"

namespace gvr {

constexpr uint16_t kSkNetPort = 27350;   // ours
constexpr uint32_t kSkNetMagic = 0x4E525647;  // "GVRN"
constexpr uint16_t kSkNetVersion = 3;  // 2: snapshots numbered, players that left marked; 3: spells (2026-10-29)

// One message's bytes (little-endian; floats as IEEE bits).
class NetWriter {
public:
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
    void f32(float v);
    void vec(const Vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
    void str(const std::string& s);
};
class NetReader {
public:
    NetReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool ok() const { return ok_; }
    size_t left() const { return ok_ ? n_ - p_ : 0; }
    uint8_t u8();
    uint16_t u16() { const uint16_t a = u8(); return uint16_t(a | (u8() << 8)); }
    uint32_t u32() { const uint32_t a = u16(); return a | (uint32_t(u16()) << 16); }
    float f32();
    Vec3 vec() { Vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
    std::string str();

private:
    const uint8_t* d_;
    size_t n_, p_ = 0;
    bool ok_ = true;
};

// What the local player's game reports each frame (joiner -> host, and the host's own).
struct SkNetSelf {
    Vec3 pos, vel, aim{0, 0, -1};
    float yaw = 0;
    uint8_t anim = 0, prevAnim = 0;
    float animT = 0, prevT = 0, fade = 1;
    float hp = 100;
    bool alive = true;
    int lastAttacker = -1;
};

struct SkNetGame {                       // a game heard on the LAN
    NetAddr host;
    std::string map, hostName;
    int players = 0, teams = 0;
    float age = 0;                       // seconds since its last beacon
};

class SkNetHost {
public:
    // Hosting `map` with the game's skirmish; false if the port can't be opened.
    bool start(const std::string& map, Skirmish* sk, uint16_t port = kSkNetPort);
    void stop();
    bool running() const { return sock_.isOpen(); }
    // A joiner asks for a player on `team` (-1: the side with fewest): the game adds one (its model
    // and movement) and returns its id (-1: refused).
    std::function<int(const std::string& name, int team)> addRemote;
    std::function<void(int id)> removeRemote;
    // Each frame, after the skirmish's update and before the game drains its events.
    void update(float dt);
    // A visual-only shot (the host's own player's), passed on to the joiners.
    void localShot(const SkShot& s);
    size_t clients() const { return clients_.size(); }
    std::vector<NetAddr> addresses() const;  // where joiners can reach this host
    uint16_t port() const { return sock_.localPort(); }

private:
    struct Client {
        NetAddr addr;
        int id = -1;
        float quiet = 0;                 // seconds since its last packet
        uint32_t hitSeq = 0;             // its hits applied up to this
        uint32_t spellSeq = 0;           // its spells taken in up to this
        uint32_t stateSeq = 0;           // its newest state taken in (older ones arriving late are dropped)
        bool started = false;            // a state came: its player is in the game (not while it loads the map)
        uint32_t eventAcked = 0;         // events it has
        bool wasAlive = true;
    };
    struct LoggedEvent { uint32_t seq; SkEvent e; int forPlayer; };  // forPlayer -1: everyone
    UdpSocket sock_;
    Skirmish* sk_ = nullptr;
    std::string map_;
    std::vector<Client> clients_;
    std::vector<LoggedEvent> log_;
    uint32_t eventSeq_ = 0, snapSeq_ = 0;
    float beaconT_ = 0, snapT_ = 0;
    void onPacket(const std::vector<uint8_t>& d, const NetAddr& from);
    void sendSnapshot(Client& c);
    Client* client(const NetAddr& a);
};

class SkNetClient {
public:
    enum class State { Idle, Searching, Joining, Joined, Lost };
    State state = State::Idle;
    std::vector<SkNetGame> games;        // heard while searching
    int myId = -1, myTeam = -1;
    std::string map;                     // the host's map (Welcome)
    // Start listening for beacons (and probing the LAN).
    bool search(uint16_t port = kSkNetPort);
    // Ask `host` for a place on `team` (-1: any).
    bool join(const NetAddr& host, const std::string& name, int team);
    void leave();
    // Each frame: send our state, take in the host's snapshots into `sk` (players, flags, scores,
    // shots) and its events into `events`.
    void update(float dt, const SkNetSelf& self, Skirmish& sk);
    std::vector<SkEvent> events;         // new since the game last drained them
    void hit(int target, float damage, const Vec3& at);  // a hit our weapons landed (resent until applied)
    void shot(const SkShot& s);          // a shot we fired (for the others to see)
    void spell(int kind, float yaw, const Vec3& at);  // a spell we cast (for the others to see; resent until taken)
    const NetAddr& host() const { return host_; }
    float sinceHost() const { return quiet_; }

private:
    UdpSocket sock_;
    NetAddr host_;
    std::string name_;
    int wantTeam_ = -1;
    float sendT_ = 0, quiet_ = 0, probeT_ = 0;
    uint32_t stateSeq_ = 0, hitSeq_ = 0, hitAcked_ = 0, eventSeen_ = 0, snapSeen_ = 0;
    struct Hit { uint32_t seq; int target; float damage; Vec3 at; };
    std::vector<Hit> hits_;
    std::vector<SkShot> shotsOut_;
    struct SpellOut { uint32_t seq; int kind; float yaw; Vec3 at; };
    std::vector<SpellOut> spells_;
    uint32_t spellSeq_ = 0, spellAcked_ = 0;
    void onPacket(const std::vector<uint8_t>& d, const NetAddr& from, Skirmish& sk);
};

}  // namespace gvr
