#include "game/sk_net.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"

namespace gvr {

namespace {
enum Msg : uint8_t { kBeacon = 1, kProbe = 2, kJoin = 3, kWelcome = 4, kRefuse = 5, kState = 6, kSnapshot = 7, kBye = 8 };
constexpr float kSnapshotEvery = 1.0f / 20.0f, kStateEvery = 1.0f / 30.0f, kTimeout = 15.0f;  // (a joiner loading the map is silent a while)
constexpr size_t kMaxShots = 16, kMaxHits = 8, kMaxEvents = 12;
constexpr size_t kMaxShotsIn = 4;        // a joiner's shots per state (its game sends at most 4)
constexpr size_t kShotsInFlight = 96;    // everyone's, on the host
constexpr size_t kSnapshotBudget = 1400; // bytes: one Wi-Fi frame, unfragmented (shots fill what's left)
// Sequence numbers wrap: a is newer than b.
bool newer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }

void header(NetWriter& w, Msg m) {
    w.u32(kSkNetMagic);
    w.u16(kSkNetVersion);
    w.u8(m);
}
bool readHeader(NetReader& r, uint8_t& m) {
    if (r.u32() != kSkNetMagic || r.u16() != kSkNetVersion) return false;
    m = r.u8();
    return r.ok();
}
void writeShot(NetWriter& w, const SkShot& s) {
    w.u8(uint8_t(s.shooter + 1));
    w.u8(uint8_t(s.team));
    w.vec(s.pos);
    w.vec(s.vel);
    w.f32(s.gravity);
    w.u8(uint8_t(s.hero));
}
SkShot readShot(NetReader& r) {
    SkShot s;
    s.shooter = int(r.u8()) - 1;
    s.team = r.u8();
    s.pos = r.vec();
    s.vel = r.vec();
    s.gravity = r.f32();
    s.hero = SkHero(std::min<int>(2, r.u8()));
    s.visual = true;
    return s;
}
void writeEvent(NetWriter& w, uint32_t seq, const SkEvent& e) {
    w.u32(seq);
    w.u8(uint8_t(e.kind));
    w.u8(uint8_t(e.a + 1));
    w.u8(uint8_t(e.b + 1));
    w.u8(uint8_t(e.team + 1));
    w.f32(e.amount);
    w.vec(e.at);
}
SkEvent readEvent(NetReader& r, uint32_t& seq) {
    SkEvent e;
    seq = r.u32();
    e.kind = SkEvent::Kind(std::min<int>(SkEvent::Spell, r.u8()));
    e.a = int(r.u8()) - 1;
    e.b = int(r.u8()) - 1;
    e.team = int(r.u8()) - 1;
    e.amount = r.f32();
    e.at = r.vec();
    return e;
}
}  // namespace

void NetWriter::f32(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    u32(u);
}
void NetWriter::str(const std::string& s) {
    const size_t n = std::min<size_t>(s.size(), 63);
    u8(uint8_t(n));
    b.insert(b.end(), s.begin(), s.begin() + long(n));
}
uint8_t NetReader::u8() {
    if (!ok_ || p_ >= n_) { ok_ = false; return 0; }
    return d_[p_++];
}
float NetReader::f32() {
    const uint32_t u = u32();
    float v;
    std::memcpy(&v, &u, 4);
    return std::isfinite(v) ? v : 0.0f;
}
std::string NetReader::str() {
    const size_t n = u8();
    if (!ok_ || p_ + n > n_) { ok_ = false; return {}; }
    std::string s(reinterpret_cast<const char*>(d_ + p_), n);
    p_ += n;
    return s;
}

// ---------------------------------------------------------------------------------------------
// Host

bool SkNetHost::start(const std::string& map, Skirmish* sk, uint16_t port) {
    stop();
    sk_ = sk;
    map_ = map;
    if (!sock_.open(port)) {
        LOGW("Network: port %u unavailable - not hosting", port);
        return false;
    }
    std::string where;
    for (const auto& a : addresses()) where += " " + a.str();
    LOGI("Network: hosting %s on port %u (%s)", map.c_str(), sock_.localPort(), where.empty() ? "no network" : where.c_str());
    return true;
}

void SkNetHost::stop() {
    if (sock_.isOpen()) {
        NetWriter w;
        header(w, kBye);
        for (const auto& c : clients_) sock_.send(c.addr, w.b.data(), w.b.size());
    }
    sock_.close();
    clients_.clear();
    log_.clear();
}

std::vector<NetAddr> SkNetHost::addresses() const {
    auto a = UdpSocket::localAddresses();
    for (auto& x : a) x.port = sock_.localPort();
    return a;
}

SkNetHost::Client* SkNetHost::client(const NetAddr& a) {
    for (auto& c : clients_)
        if (c.addr == a) return &c;
    return nullptr;
}

void SkNetHost::localShot(const SkShot& s) {
    if (!sk_) return;
    SkShot v = s;
    v.visual = true;
    v.hostOwn = true;  // (the host's game draws its own projectile; this copy is for the joiners)
    sk_->shots.push_back(v);
}

void SkNetHost::onPacket(const std::vector<uint8_t>& d, const NetAddr& from) {
    NetReader r(d.data(), d.size());
    uint8_t m;
    if (!readHeader(r, m)) return;
    auto beacon = [&](const NetAddr& to) {
        NetWriter w;
        header(w, kBeacon);
        w.str(map_);
        w.str("GIANTS VR");
        w.u8(uint8_t(sk_->teams.size()));
        w.u8(uint8_t(sk_->players.size()));
        sock_.send(to, w.b.data(), w.b.size());
    };
    if (m == kProbe) { beacon(from); return; }
    if (m == kJoin) {
        const std::string name = r.str();
        const int team = int(int8_t(r.u8()));
        if (!r.ok()) return;
        Client* c = client(from);
        if (!c) {
            const int id = addRemote ? addRemote(name.empty() ? "PLAYER" : name, team) : -1;
            if (id < 0) {
                NetWriter w;
                header(w, kRefuse);
                w.str("GAME FULL");
                sock_.send(from, w.b.data(), w.b.size());
                return;
            }
            Client nc;
            nc.addr = from;
            nc.id = id;
            nc.eventAcked = eventSeq_;  // (what came before it joined isn't news to it)
            clients_.push_back(nc);
            c = &clients_.back();
            LOGI("Network: %s joined from %s as player %d", name.c_str(), from.str().c_str(), id);
        }
        const SkPlayer* p = sk_->player(c->id);
        NetWriter w;
        header(w, kWelcome);
        w.u8(uint8_t(c->id));
        w.u8(uint8_t(p ? p->team : 0));
        w.str(map_);
        w.u8(uint8_t(sk_->rules.captureLimit));
        w.u8(uint8_t(sk_->rules.killLimit));
        sock_.send(from, w.b.data(), w.b.size());
        return;
    }
    Client* c = client(from);
    if (!c) return;
    c->quiet = 0;
    if (m == kBye) {
        if (removeRemote) removeRemote(c->id);
        LOGI("Network: player %d left", c->id);
        clients_.erase(clients_.begin() + (c - clients_.data()));
        return;
    }
    if (m != kState) return;
    const uint32_t seq = r.u32();
    if (c->stateSeq != 0 && !newer(seq, c->stateSeq)) return;  // late (an older state overtaken by a newer)
    c->stateSeq = seq;
    const uint32_t ack = r.u32();
    SkNetSelf s;
    s.pos = r.vec();
    s.vel = r.vec();
    s.yaw = r.f32();
    s.anim = r.u8();
    s.prevAnim = r.u8();
    s.animT = r.f32();
    s.prevT = r.f32();
    s.fade = r.f32();
    s.hp = r.f32();
    s.alive = r.u8() != 0;
    s.lastAttacker = int(r.u8()) - 1;
    s.aim = r.vec();
    if (!r.ok()) return;
    if (newer(ack, c->eventAcked)) c->eventAcked = ack;
    if (SkPlayer* p = sk_->player(c->id)) {
        p->body.position = s.pos;
        p->body.velocity = s.vel;
        p->body.yaw = s.yaw;
        p->netAnim = s.anim;
        p->netPrev = s.prevAnim;
        p->netT = s.animT;
        p->netPrevT = s.prevT;
        p->netFade = s.fade;
        p->hp = s.hp;
        p->aim = s.aim;
        if (!s.alive && p->alive) sk_->remoteDied(p->id, s.lastAttacker);
        if (s.alive && !p->alive) { p->alive = true; p->deadT = 0; }  // it respawned (or arrived)
        c->started = true;
    }
    // Its hits (each once), then its shots (for everyone to see).
    const int nh = r.u8();
    for (int k = 0; k < nh && r.ok(); ++k) {
        const uint32_t seq = r.u32();
        const int target = r.u8() - 1;
        const float dmg = r.f32();
        const Vec3 at = r.vec();
        if (!r.ok() || !newer(seq, c->hitSeq)) continue;
        c->hitSeq = seq;
        sk_->damage(target, std::max(0.0f, std::min(dmg, 600.0f)), c->id, at);
    }
    const int ns = r.u8();
    const SkPlayer* me = sk_->player(c->id);
    for (int k = 0; k < ns && r.ok(); ++k) {
        SkShot sh = readShot(r);
        if (!r.ok() || size_t(k) >= kMaxShotsIn || sk_->shots.size() >= kShotsInFlight || !me) continue;
        sh.shooter = c->id;
        sh.team = me->team;
        sk_->shots.push_back(sh);
    }
    // Its spells (each once): events for everyone (its own game ignores its own).
    const int nsp = r.left() > 0 ? r.u8() : 0;
    for (int k = 0; k < nsp && r.ok(); ++k) {
        const uint32_t seq = r.u32();
        const int kind = r.u8();
        const float yaw = r.f32();
        const Vec3 at = r.vec();
        if (!r.ok() || !newer(seq, c->spellSeq) || kind > int(SkSpell::Hail)) continue;
        c->spellSeq = seq;
        SkEvent e;
        e.kind = SkEvent::Spell;
        e.a = c->id;
        e.team = kind;
        e.amount = yaw;
        e.at = at;
        sk_->events.push_back(e);
    }
}

void SkNetHost::sendSnapshot(Client& c) {
    NetWriter w;
    header(w, kSnapshot);
    w.u32(++snapSeq_);
    w.u32(c.hitSeq);
    w.u32(c.spellSeq);
    w.f32(sk_->time);
    w.u8(uint8_t(sk_->winner + 1));
    w.u8(uint8_t(sk_->teams.size()));
    for (const auto& t : sk_->teams) { w.u8(uint8_t(t.captures)); w.u16(uint16_t(t.kills)); w.u16(uint16_t(t.deaths)); }
    w.u8(uint8_t(sk_->flags.size()));
    for (const auto& f : sk_->flags) { w.u8(f.used); w.vec(f.pos); w.u8(uint8_t(f.carrier + 1)); w.u8(f.atHome); }
    w.u8(uint8_t(sk_->players.size()));
    for (const auto& p : sk_->players) {
        w.u8(uint8_t(p.id));
        w.u8(uint8_t(p.team));
        w.str(p.name);
        w.u8(uint8_t((p.bot ? 1 : 0) | (p.alive ? 2 : 0) | (p.gone ? 4 : 0)));
        w.vec(p.body.position);
        w.vec(p.body.velocity);
        w.f32(p.body.yaw);
        // Animation: the bots' and the host's own from their controllers, the others' as they sent it.
        const bool sim = p.bot || p.local;
        w.u8(sim ? uint8_t(p.body.anim()) : p.netAnim);
        w.u8(sim ? uint8_t(p.body.prevAnim()) : p.netPrev);
        w.f32(sim ? p.body.animTime() : p.netT);
        w.f32(sim ? p.body.prevAnimTime() : p.netPrevT);
        w.f32(sim ? p.body.fadeWeight() : p.netFade);
        w.f32(p.deadT);
        w.u16(uint16_t(std::max(0.0f, p.hp)));
        w.u8(uint8_t(p.carrying + 1));
        w.u16(uint16_t(p.kills));
        w.u16(uint16_t(p.deaths));
        w.vec(p.aim);
    }
    // Events it hasn't acknowledged (its own damage, and everyone's news), the oldest first: what
    // doesn't fit comes in the next ones (it acknowledges the newest it has).
    std::vector<const LoggedEvent*> ev;
    for (const auto& le : log_)
        if (newer(le.seq, c.eventAcked) && (le.forPlayer < 0 || le.forPlayer == c.id) && ev.size() < kMaxEvents) ev.push_back(&le);
    w.u8(uint8_t(ev.size()));
    for (const auto* le : ev) writeEvent(w, le->seq, le->e);
    // Shots in flight (the newest), as many as fit in one frame.
    constexpr size_t kShotBytes = 2 + 12 + 12 + 4 + 1;
    const size_t room = w.b.size() + 1 < kSnapshotBudget ? (kSnapshotBudget - w.b.size() - 1) / kShotBytes : 0;
    const size_t ns = std::min({sk_->shots.size(), kMaxShots, room});
    w.u8(uint8_t(ns));
    for (size_t k = sk_->shots.size() - ns; k < sk_->shots.size(); ++k) writeShot(w, sk_->shots[k]);
    sock_.send(c.addr, w.b.data(), w.b.size());
}

void SkNetHost::update(float dt) {
    if (!sock_.isOpen() || !sk_) return;
    std::vector<uint8_t> d;
    NetAddr from;
    for (int k = 0; k < 256 && sock_.recv(d, from); ++k) onPacket(d, from);
    // The skirmish's events, logged for the joiners (damage only to whom it hurts).
    for (const auto& e : sk_->events) {
        if (e.kind == SkEvent::Fire || e.kind == SkEvent::Respawn) continue;
        int forPlayer = -1;
        if (e.kind == SkEvent::HurtLocal) {
            const SkPlayer* v = sk_->player(e.b);
            if (!v || !v->remote) continue;  // the host's own player: its game applies it
            forPlayer = v->id;
        }
        log_.push_back({++eventSeq_, e, forPlayer});
    }
    // Forget what everyone has.
    uint32_t minAck = eventSeq_;
    for (const auto& c : clients_)
        if (newer(minAck, c.eventAcked)) minAck = c.eventAcked;
    log_.erase(std::remove_if(log_.begin(), log_.end(), [&](const LoggedEvent& le) { return !newer(le.seq, minAck); }), log_.end());
    if (log_.size() > 256) log_.erase(log_.begin(), log_.end() - 256);
    // Joiners gone quiet: their players leave.
    for (size_t i = 0; i < clients_.size();) {
        clients_[i].quiet += std::min(dt, 0.25f);
        if (clients_[i].quiet > kTimeout) {
            LOGI("Network: player %d timed out", clients_[i].id);
            if (removeRemote) removeRemote(clients_[i].id);
            clients_.erase(clients_.begin() + long(i));
        } else {
            ++i;
        }
    }
    beaconT_ -= dt;
    if (beaconT_ <= 0) {
        beaconT_ = 1.0f;
        NetWriter w;
        header(w, kBeacon);
        w.str(map_);
        w.str("GIANTS VR");
        w.u8(uint8_t(sk_->teams.size()));
        w.u8(uint8_t(sk_->players.size()));
        sock_.send(NetAddr::broadcast(kSkNetPort), w.b.data(), w.b.size());
    }
    snapT_ -= dt;
    if (snapT_ <= 0) {
        snapT_ = kSnapshotEvery;
        for (auto& c : clients_) sendSnapshot(c);
    }
}

// ---------------------------------------------------------------------------------------------
// Joiner

bool SkNetClient::search(uint16_t port) {
    // On the game's port the host's once-a-second beacons are heard too (else only answers to our
    // probes); taken (a host on this same machine), any port.
    if (!sock_.isOpen() && !sock_.open(port) && !sock_.open(0)) return false;
    state = State::Searching;
    games.clear();
    probeT_ = 0;
    return true;
}

bool SkNetClient::join(const NetAddr& host, const std::string& name, int team) {
    if (!sock_.isOpen() && !sock_.open(0)) return false;
    host_ = host;
    name_ = name;
    wantTeam_ = team;
    state = State::Joining;
    quiet_ = 0;
    sendT_ = 0;
    myId = myTeam = -1;
    // A fresh session (the host may have restarted: its numbers start again).
    stateSeq_ = hitSeq_ = hitAcked_ = eventSeen_ = snapSeen_ = spellSeq_ = spellAcked_ = 0;
    hits_.clear();
    spells_.clear();
    shotsOut_.clear();
    events.clear();
    LOGI("Network: joining %s", host.str().c_str());
    return true;
}

void SkNetClient::leave() {
    if (sock_.isOpen() && state == State::Joined) {
        NetWriter w;
        header(w, kBye);
        sock_.send(host_, w.b.data(), w.b.size());
    }
    sock_.close();
    state = State::Idle;
}

void SkNetClient::hit(int target, float damage, const Vec3& at) {
    hits_.push_back({++hitSeq_, target, damage, at});
    if (hits_.size() > 32) hits_.erase(hits_.begin());
}

void SkNetClient::spell(int kind, float yaw, const Vec3& at) {
    spells_.push_back({++spellSeq_, kind, yaw, at});
    if (spells_.size() > 16) spells_.erase(spells_.begin());
}

void SkNetClient::shot(const SkShot& s) {
    shotsOut_.push_back(s);
    if (shotsOut_.size() > 4) shotsOut_.erase(shotsOut_.begin());
}

void SkNetClient::onPacket(const std::vector<uint8_t>& d, const NetAddr& from, Skirmish& sk) {
    NetReader r(d.data(), d.size());
    uint8_t m;
    if (!readHeader(r, m)) return;
    if (m == kBeacon) {
        SkNetGame g;
        g.host = from;
        g.map = r.str();
        g.hostName = r.str();
        g.teams = r.u8();
        g.players = r.u8();
        if (!r.ok()) return;
        auto it = std::find_if(games.begin(), games.end(), [&](const SkNetGame& x) { return x.host == from; });
        if (it == games.end()) { games.push_back(g); LOGI("Network: found a game at %s (%s)", from.str().c_str(), g.map.c_str()); }
        else *it = g;
        return;
    }
    if (from != host_) return;
    quiet_ = 0;
    if (m == kWelcome && state == State::Joining) {
        myId = r.u8();
        myTeam = r.u8();
        map = r.str();
        const int caps = r.u8(), kills = r.u8();
        if (!r.ok()) return;
        sk.rules.captureLimit = caps;
        sk.rules.killLimit = kills;
        state = State::Joined;
        LOGI("Network: joined %s as player %d, team %d", map.c_str(), myId, myTeam);
        return;
    }
    if (m == kRefuse) {
        LOGW("Network: refused (%s)", r.str().c_str());
        state = State::Lost;
        return;
    }
    if (m == kBye) {
        LOGI("Network: the host ended the game");
        state = State::Lost;
        return;
    }
    if (m != kSnapshot || state != State::Joined) return;
    const uint32_t snap = r.u32();
    if (snapSeen_ != 0 && !newer(snap, snapSeen_)) return;  // late: a newer one is already in
    snapSeen_ = snap;
    if (const uint32_t ha = r.u32(); newer(ha, hitAcked_)) hitAcked_ = ha;
    hits_.erase(std::remove_if(hits_.begin(), hits_.end(), [&](const Hit& h) { return !newer(h.seq, hitAcked_); }), hits_.end());
    if (const uint32_t sa = r.u32(); newer(sa, spellAcked_)) spellAcked_ = sa;
    spells_.erase(std::remove_if(spells_.begin(), spells_.end(), [&](const SpellOut& s) { return !newer(s.seq, spellAcked_); }), spells_.end());
    sk.time = r.f32();
    sk.winner = int(r.u8()) - 1;
    const int nt = r.u8();
    for (int t = 0; t < nt && r.ok(); ++t) {
        const int caps = r.u8(), kills = r.u16(), deaths = r.u16();
        if (size_t(t) < sk.teams.size()) { sk.teams[size_t(t)].captures = caps; sk.teams[size_t(t)].kills = kills; sk.teams[size_t(t)].deaths = deaths; }
    }
    const int nf = r.u8();
    for (int t = 0; t < nf && r.ok(); ++t) {
        SkFlag f;
        f.used = r.u8() != 0;
        f.pos = r.vec();
        f.carrier = int(r.u8()) - 1;
        f.atHome = r.u8() != 0;
        if (f.carrier >= 64) f.carrier = -1;
        if (size_t(t) < sk.flags.size()) { f.home = sk.flags[size_t(t)].home; sk.flags[size_t(t)] = f; }
    }
    const int np = r.u8();
    for (int k = 0; k < np && r.ok(); ++k) {
        const int id = r.u8();
        const int team = r.u8();
        const std::string name = r.str();
        const int flags = r.u8();
        const Vec3 pos = r.vec(), vel = r.vec();
        const float yaw = r.f32();
        const uint8_t anim = r.u8(), prev = r.u8();
        const float at = r.f32(), pt = r.f32(), fade = r.f32(), deadT = r.f32();
        const float hp = r.u16();
        const int carrying = int(r.u8()) - 1;
        const int kills = r.u16(), deaths = r.u16();
        const Vec3 aim = r.vec();
        if (!r.ok() || id > 63) break;
        if (team >= int(sk.teams.size())) continue;  // (not a side of this map)
        SkPlayer& p = sk.mirrorPlayer(id);
        p.team = team;
        p.name = name;
        p.bot = flags & 1;
        p.gone = (flags & 4) != 0;
        p.carrying = carrying < int(sk.flags.size()) ? carrying : -1;
        p.kills = kills;
        p.deaths = deaths;
        if (id == myId) { p.local = true; continue; }  // our own body is ours
        p.remote = true;
        p.alive = (flags & 2) != 0;
        p.deadT = deadT;
        // Smoothed toward where the host has it (the drawing follows; shots aim at it).
        if (length(pos - p.body.position) > 6.0f || p.hp <= 0) p.body.position = pos;
        p.netTarget = pos;
        p.body.velocity = vel;
        p.body.yaw = yaw;
        p.netAnim = anim;
        p.netPrev = prev;
        p.netT = at;
        p.netPrevT = pt;
        p.netFade = fade;
        p.hp = hp;
        p.aim = aim;
    }
    const int ne = r.u8();
    for (int k = 0; k < ne && r.ok(); ++k) {
        uint32_t seq;
        SkEvent e = readEvent(r, seq);
        if (!r.ok() || (eventSeen_ != 0 && !newer(seq, eventSeen_))) continue;
        eventSeen_ = seq;
        if (e.team >= int(sk.teams.size())) e.team = -1;
        events.push_back(e);
    }
    const int ns = r.u8();
    std::vector<SkShot> shots;
    for (int k = 0; k < ns && r.ok(); ++k) {
        SkShot s = readShot(r);
        if (s.shooter != myId) shots.push_back(s);  // (our own we draw ourselves)
    }
    if (r.ok()) sk.shots = shots;
}

void SkNetClient::update(float dt, const SkNetSelf& self, Skirmish& sk) {
    if (!sock_.isOpen()) return;
    std::vector<uint8_t> d;
    NetAddr from;
    for (int k = 0; k < 256 && sock_.recv(d, from); ++k) onPacket(d, from, sk);
    for (auto& g : games) g.age += dt;
    quiet_ += std::min(dt, 0.25f);  // (a long frame - the map loading - is not silence)
    if (state == State::Searching) {
        probeT_ -= dt;
        if (probeT_ <= 0) {
            probeT_ = 1.0f;
            NetWriter w;
            header(w, kProbe);
            sock_.send(NetAddr::broadcast(kSkNetPort), w.b.data(), w.b.size());
        }
        return;
    }
    if (state == State::Joining) {
        sendT_ -= dt;
        if (sendT_ <= 0) {
            sendT_ = 0.5f;
            NetWriter w;
            header(w, kJoin);
            w.str(name_);
            w.u8(uint8_t(int8_t(wantTeam_)));
            sock_.send(host_, w.b.data(), w.b.size());
        }
        if (quiet_ > 10.0f) state = State::Lost;
        return;
    }
    if (state != State::Joined) return;
    if (quiet_ > kTimeout) {
        LOGW("Network: lost the host");
        state = State::Lost;
        return;
    }
    // Others drift toward where the host last had them.
    for (auto& p : sk.players)
        if (p.remote && p.alive) {
            p.netTarget = p.netTarget + p.body.velocity * dt;
            p.body.position = p.body.position + (p.netTarget - p.body.position) * std::min(1.0f, dt * 12.0f);
            p.netT += dt;
            p.netPrevT += dt;
            p.netFade = std::min(1.0f, p.netFade + dt * 5.0f);
        }
    sendT_ -= dt;
    if (sendT_ > 0) return;
    sendT_ = kStateEvery;
    NetWriter w;
    header(w, kState);
    w.u32(++stateSeq_);
    w.u32(eventSeen_);
    w.vec(self.pos);
    w.vec(self.vel);
    w.f32(self.yaw);
    w.u8(self.anim);
    w.u8(self.prevAnim);
    w.f32(self.animT);
    w.f32(self.prevT);
    w.f32(self.fade);
    w.f32(self.hp);
    w.u8(self.alive ? 1 : 0);
    w.u8(uint8_t(self.lastAttacker + 1));
    w.vec(self.aim);
    const size_t nh = std::min(hits_.size(), kMaxHits);
    w.u8(uint8_t(nh));
    for (size_t k = 0; k < nh; ++k) {
        w.u32(hits_[k].seq);
        w.u8(uint8_t(hits_[k].target + 1));
        w.f32(hits_[k].damage);
        w.vec(hits_[k].at);
    }
    w.u8(uint8_t(shotsOut_.size()));
    for (const auto& s : shotsOut_) writeShot(w, s);
    shotsOut_.clear();
    const size_t nsp = std::min<size_t>(spells_.size(), 8);
    w.u8(uint8_t(nsp));
    for (size_t k = 0; k < nsp; ++k) {
        w.u32(spells_[k].seq);
        w.u8(uint8_t(spells_[k].kind));
        w.f32(spells_[k].yaw);
        w.vec(spells_[k].at);
    }
    sock_.send(host_, w.b.data(), w.b.size());
}

}  // namespace gvr
