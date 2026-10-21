// World definition files w_<World>.bin (magic 0x1A0002E5): container + opcode stream walker.
// Opcode sizes come from the decompilation notes, corrected against real files
// (op 41/88 are 12 bytes, op 42 is the 21-byte object record). See docs/formats/WORLD_BIN.md.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gvr {

struct WorldObjectRecord {
    uint32_t type = 0;
    float x = 0, y = 0, z = 0;   // game units, Z-up
    float angle = 0;
};

struct WorldDefinition {
    std::string header1, header2;      // e.g. "box01", "story1.gti"
    std::string terrainName;           // from header2 (".gti" file)
    std::string groundTexture, wallTexture1, wallTexture2;  // ops 52/53/54
    std::string skyTexture, seaTexture, glowTexture;        // ops 30/37/38
    float fogNear = 0, fogFar = 0;     // op 41 (game units)
    uint8_t fogRGB[3] = {0, 0, 0};
    bool hasFog = false;
    float waterFogNear = 0, waterFogFar = 0;  // op 88
    uint8_t waterFogRGB[3] = {0, 0, 0};
    uint8_t ambientRGB[3] = {255, 255, 255};  // op 40
    std::vector<WorldObjectRecord> objects;  // op 42
    struct Scenario { uint8_t kind; std::string name; };
    std::vector<Scenario> scenarios;         // op 92: mission files wm_<name>.bin (kind 2) etc.
    size_t recordsParsed = 0;
    size_t streamSize = 0, streamConsumed = 0;
    int stopOpcode = -1;                // opcode that ended the walk
    bool complete = false;              // reached 0xFF / end of stream
};

bool parseWorldBin(const uint8_t* data, size_t size, WorldDefinition& out, std::string* error = nullptr);

}  // namespace gvr
