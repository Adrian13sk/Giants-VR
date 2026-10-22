// SKN (rigid skin) and ANM (keyframed node hierarchy) parsers.
//
// Both are serialized memory images whose pointers are byte offsets from the start of the
// file. Verified on sm_L0.skn + sm_stand.anm: reconstructing vertices as
// BoneWorld(bone) * local reproduces the GBS rest positions EXACTLY (error 0.000) for every
// bone whose stand-animation pose equals the export pose (root chain, right leg, tail), which
// confirms the cluster layout, hierarchy composition (W = Wparent * L) and the quaternion
// convention (stored x,y,z are negated). See docs/formats/FORMATS.md.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math.h"

namespace gvr {

// ---------------------------------------------------------------------------------- SKN ---
struct SknCluster {
    uint32_t bone = 0;  // index into SknModel::boneNames
    struct V { float x, y, z; uint32_t index; };
    std::vector<V> verts;  // index is local to the owning object
};

struct SknObject {
    std::string name;
    uint32_t vertexCount = 0;
    std::vector<SknCluster> clusters;
};

struct SknModel {
    std::vector<SknObject> objects;                       // in file (NAM) order
    std::unordered_map<uint32_t, std::string> boneNames;  // bone index -> name
};

bool parseSkn(const uint8_t* data, size_t size, SknModel& out, std::string* error = nullptr);

// ---------------------------------------------------------------------------------- ANM ---
struct AnmKeyVec3 { int32_t tick; Vec3 value; };
struct AnmKeyQuat { int32_t tick; Quat value; };

struct AnmNode {
    std::string name;
    int parent = -1;
    Vec3 pos;           // default local transform (game units, Z-up frame)
    Quat rot;           // standard rotation (stored xyz already negated back)
    Vec3 scale{1, 1, 1};
    bool mirrored = false;  // flag bit 0x08 at +0x94
    int table = 0;          // node table (cutscene director images hold one per actor tree)
    uint8_t keyType[3] = {0, 0, 0};  // pos/rot/scale track key types (diagnostics)
    std::vector<float> rawKeys;      // diagnostics: first two position keys, 16 floats each
    std::vector<AnmKeyVec3> posKeys;
    std::vector<AnmKeyQuat> rotKeys;
    std::vector<AnmKeyVec3> scaleKeys;
};

struct AnmAnimation {
    int32_t startTick = 0, endTick = 0;
    uint32_t ticksPerFrame = 160, fps = 30;
    std::vector<AnmNode> nodes;
    std::unordered_map<std::string, int> byName;  // lower-case name -> node index
    uint32_t unsupportedTracks = 0;

    float durationSeconds() const {
        const float tps = float(ticksPerFrame) * float(fps);
        return tps > 0 ? float(endTick - startTick) / tps : 0.0f;
    }
    // Evaluates every node's world matrix (game units) at `seconds` (looped).
    void evaluate(float seconds, std::vector<Mat4>& world) const;
    // The nodes of one node table as a stand-alone animation (parents outside the table are
    // dropped; *rootParent receives the first such outside parent, e.g. an actor's mover node).
    AnmAnimation subset(int table, int* rootParent = nullptr) const;
    int tableCount() const { int t = 0; for (const auto& n : nodes) t = n.table > t ? n.table : t; return nodes.empty() ? 0 : t + 1; }
};

bool parseAnm(const uint8_t* data, size_t size, AnmAnimation& out, std::string* error = nullptr);

}  // namespace gvr
