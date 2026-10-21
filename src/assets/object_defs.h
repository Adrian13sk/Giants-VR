// Object type registry and placement records.
//
// Object definitions live in the loose Bin/*.bin files (xx_*.bin etc.). Two record shapes,
// found by reverse engineering against the GOG data (see docs/formats/FORMATS.md):
//   mesh/group:  u32 meshId, char name[] NUL (+pad), u32 groupId, 0x51 u32 meshId f32 dist f32 ...
//   object type: [0xFF end of previous record] u32 typeId, 0x01, u32 groupId, property ops..., 0xFF
// Placements: world op 42 and mission (wm_*.bin) op 0x03 records share the 21-byte layout
//   u8 op, u32 typeId, f32 x, f32 y, f32 z (game units, Z-up), f32 yaw (DEGREES).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gvr {

struct ObjectPlacement {
    uint32_t type = 0;
    float x = 0, y = 0, z = 0;  // game units
    float yawDegrees = 0;
};

class ObjectRegistry {
public:
    // Scans every *.bin file in `binDir`. Returns number of object types found.
    size_t build(const std::string& binDir);
    // Feeds one definition file (used by build and by tests).
    void addDefinitionFile(const uint8_t* d, size_t n);
    void finalize();

    // Model (mesh) name for an object type, or empty if unknown.
    std::string modelName(uint32_t type) const;
    // Animation (.anm) listed with the type's LOD0 mesh (0x52 entry), or empty.
    std::string modelAnim(uint32_t type) const;
    bool knows(uint32_t type) const { return typeToGroup_.count(type) != 0; }
    size_t typeCount() const { return typeToGroup_.size(); }
    size_t meshCount() const { return meshNames_.size(); }

private:
    std::unordered_map<uint32_t, std::string> meshNames_;   // meshId -> name
    std::unordered_map<uint32_t, std::string> meshAnims_;   // meshId -> .anm name
    std::unordered_map<uint32_t, uint32_t> groupMesh_;      // groupId -> LOD0 meshId
    std::unordered_map<uint32_t, uint32_t> typeToGroup_;    // typeId -> groupId
    std::vector<std::vector<uint8_t>> pending_;             // files kept until finalize()
};

// Heuristic placement scan of a mission file: op-3 records whose type is known to the registry
// and whose position lies inside `bounds` (minX, maxX, minY, maxY in game units). The mission
// opcode stream is not fully decoded yet; registry membership plus bounds keeps false positives
// out in practice (validated on the Story1 missions).
std::vector<ObjectPlacement> scanMissionPlacements(const uint8_t* d, size_t n, const ObjectRegistry& reg,
                                                   const float bounds[4]);

// True for engine helper objects that have no visible geometry in the shipped game
// (markers, collision volumes, reference nodes, ambient sound emitters, lights).
bool isInvisibleHelper(const std::string& modelName);

}  // namespace gvr
