// GBS static/skinned-base mesh parser (magic 0xAA0100BE). See docs/formats/GBS.md.
// Original data is Z-up; conversion to the runtime's Y-up frame happens in the model builder.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gvr {

struct GbsMaxObj {
    int32_t vstart = 0, vcount = 0, nstart = 0, ncount = 0, noffset = 0;
};

struct GbsSubObject {
    std::string name;
    std::string texture;   // base texture name without extension (may be empty)
    std::string texture2;  // secondary/bump texture name (usually empty)
    uint32_t maxObjIndex = 0;
    int32_t vstart = 0, vcount = 0;
    float falloff = 0, blend = 0, power = 0;
    uint32_t flags = 0;
    uint32_t emissive = 0, ambient = 0, diffuse = 0, specular = 0;  // COLORREF 0x00BBGGRR
    std::vector<uint32_t> indices;  // triangle list into the display-vertex arrays
};

struct GbsModel {
    static constexpr uint32_t kMagic = 0xAA0100BEu;
    static constexpr uint32_t kFlagNormals = 0x1, kFlagUVs = 0x2, kFlagRGBs = 0x4;
    static constexpr uint32_t kSubAdditive = 0x10, kSubSubtractive = 0x20;
    // Sub-object flag bit 0: glass (the lander canopy, the Meccaryn's visor over his face).
    static constexpr uint32_t kSubGlass = 0x1;

    uint32_t flags = 0;
    std::vector<std::array<float, 3>> positions;   // "animation" vertices (Z-up)
    std::vector<uint16_t> normalDefs;              // packed normal table (unused by renderer)
    std::vector<uint16_t> displayToPosition;       // per display vertex -> positions index
    std::vector<uint16_t> displayNormal;           // per display vertex -> normal def
    std::vector<std::array<float, 2>> uvs;         // per display vertex
    std::vector<std::array<uint8_t, 3>> rgbs;      // per display vertex (baked lighting)
    std::vector<GbsMaxObj> maxObjs;
    std::vector<GbsSubObject> subObjects;

    size_t triangleCount() const;
};

// Parses a GBS file. Validates all indices. Returns false and sets `error` on failure.
bool parseGbs(const uint8_t* data, size_t size, GbsModel& out, std::string* error = nullptr);

}  // namespace gvr
