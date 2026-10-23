// Reaperski (jetski) riding and racing - Delphi's water missions (rstory2_jetski_*, the Reaperski
// races rstory3_race_1..3 and rstory5_race_1). The data gives the course: buoy_spline objects are
// the gates in order, marker_spline objects grouped by id are the rivals' racing lines,
// r3_start_finish the start / finish line, rpman_l0 with id 11 the rival riders, p_jetski_* the
// pickups. The handling, rival speeds and rules beyond the briefings ("if you miss 3 GATES, you
// lose the race") are ours. Renderer-independent; the game layer draws and steers.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/math.h"

namespace gvr {

struct JetskiParams {
    float maxSpeed = 21.0f;     // m/s full throttle
    float turboSpeed = 31.0f;   // m/s with turbo
    float reverseSpeed = 5.0f;
    float accel = 9.0f;         // m/s^2
    float brake = 16.0f;
    float drag = 0.5f;          // 1/s coasting
    float turnRate = 1.6f;      // rad/s at speed
    float maxSlope = 2.5f;      // rise/run it can drive up at speed (ramps, sandbars, heightfield steps); steeper faces bump it back
    float landDrag = 0.7f;      // 1/s on land
    float gravity = 16.0f;      // m/s^2 in the air
};

struct JetskiState {
    Vec3 pos;           // on the water surface
    float yaw = 0;      // runtime yaw (0 = facing -Z)
    float speed = 0;    // m/s forward (negative: reversing)
    float bank = 0;     // visual lean into turns (rad)
    float bob = 0;      // time for the swell
    float vy = 0;       // m/s vertical (jumps)
    bool air = false;   // off a ramp, flying
};

// One physics step. throttle and steer in -1..1; ground(x, z) = terrain height. Returns true if
// it ran aground (bumped back).
bool stepJetski(JetskiState& s, const JetskiParams& p, float throttle, float steer, bool turbo, float dt,
                const std::function<float(float, float)>& ground, float seaLevel);

struct Racer {
    std::vector<Vec3> path;     // racing line (runtime, on the water)
    float along = 0;            // distance along the path (m)
    float length = 0;
    float base = 20.0f;         // m/s cruising
    float speed = 0;
    float spin = 0;             // s spun out (hit)
    Vec3 pos;
    float yaw = 0, bank = 0;
    bool finished = false;
    float finishTime = 0;
};

struct RaceParams {
    float gateRadius = 16.0f;   // m: passing this close to a buoy counts it
    int missLimit = 3;          // "if you miss 3 GATES, you lose the race"
    float countdown = 3.0f;     // s before the start
    float finishRadius = 22.0f; // m round the start / finish line
};

// A route over water from a to b (A* on a grid of `cell` metres over a box round the two points,
// water = ground below `maxGround`), simplified to the corners; just {a, b} if the straight line is
// clear, or if no route is found.
std::vector<Vec3> waterRoute(const Vec3& a, const Vec3& b, const std::function<float(float, float)>& ground, float maxGround = -0.4f,
                             float cell = 4.0f, float margin = 250.0f);

// The gates of a course in driving order: a closed tour from `start` through all gates that is
// cheapest by `cost` (nearest neighbour, then 2-opt), run in the direction that takes the data's
// first gate first. The data lists most gates in order but some were appended out of place.
std::vector<Vec3> orderGates(const std::vector<Vec3>& gates, const Vec3& start, const std::function<float(const Vec3&, const Vec3&)>& cost);

class RaceDirector {
public:
    RaceParams params;
    std::vector<Vec3> gates;
    Vec3 finishLine;
    bool hasFinishLine = false;
    std::vector<Racer> racers;
    // State.
    float time = -1;            // < 0: counting down
    int nextGate = 0, missed = 0;
    bool playerFinished = false, lost = false;
    int playerPlace = 0;        // 1 = won (valid once playerFinished)
    std::vector<int> gateEvents;  // this frame: +1 gate passed, -1 gate missed

    void setup(const std::vector<Vec3>& gates, const std::vector<std::vector<Vec3>>& lines, int rivals, const Vec3* finish);
    // The player's position; playerSpeed for the rivals' rubber band.
    void update(float dt, const Vec3& player);
    bool started() const { return time >= 0; }
    // Place now (1 = leading): finished rivals first, then by progress along the course.
    int place(const Vec3& player) const;
    float playerProgress(const Vec3& player) const;  // 0..1 along the gates

private:
    static Vec3 along(const std::vector<Vec3>& path, float s, float* yaw);
};

}  // namespace gvr
