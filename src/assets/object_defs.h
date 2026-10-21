// Object type registry, decoded from the object-definition section (section 3) of the
// 0x1A0002E5 .bin files (xx_*.bin etc.). See docs/formats/FORMATS.md "Object definitions".
//
//   u32 size; blob[size]                      offsets are relative to the blob
//   blob: u32 nMesh,  u32 off[nMesh]  -> {u32 meshId, strz name}
//         u32 nX,     u32 off[nX]            (unused; 0 in all shipped files)
//         u32 nGroup, u32 off[nGroup] -> {u32 groupId, group ops..., 0xFF}
//         u32 nType,  u32 off[nType]  -> {u32 typeId, type ops...}
// Every shipped file parses (1570 meshes, 799 groups, 807 types).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace gvr {

struct ObjectGroup {
    std::vector<uint32_t> lodMeshes;     // 0x51 entries in order (LOD0 first)
    std::map<int, std::string> anims;    // animation slot -> .anm file (0x52 entries)
};

struct ObjectType {
    uint32_t group = 0;
    bool hasGroup = false;
    int objectClass = -1;   // type op 0x1E (the engine's "ospec": 23 Ripper, 60 tree, 61 Smartie building...)
    float scale = 1.0f;     // type op 0x17
    float hitPoints = 0;    // type op 0x31 (loader stores it as current and maximum health); 0 = unset
};

class ObjectRegistry {
public:
    // Parses every *.bin file in `binDir`. Returns the number of object types.
    size_t build(const std::string& binDir);
    // Parses one 0x1A0002E5 file's definition section; false if it is malformed.
    bool addDefinitionFile(const uint8_t* d, size_t n, std::string* error = nullptr);

    const ObjectType* type(uint32_t id) const;
    const ObjectGroup* group(uint32_t id) const;
    // LOD0 mesh name for an object type, or empty if unknown.
    std::string modelName(uint32_t type) const;
    // The type's default animation (slot 1, else the lowest slot), or empty.
    std::string modelAnim(uint32_t type) const;
    // Animation in a specific slot, or empty.
    std::string anim(uint32_t type, int slot) const;
    bool knows(uint32_t type) const { return types_.count(type) != 0; }
    size_t typeCount() const { return types_.size(); }
    size_t meshCount() const { return meshNames_.size(); }
    size_t groupCount() const { return groups_.size(); }

private:
    std::unordered_map<uint32_t, std::string> meshNames_;
    std::unordered_map<uint32_t, ObjectGroup> groups_;
    std::unordered_map<uint32_t, ObjectType> types_;
};

// True for engine helper objects that have no visible geometry in the shipped game
// (markers, collision volumes, reference nodes, ambient sound emitters, lights).
bool isInvisibleHelper(const std::string& modelName);

}  // namespace gvr
