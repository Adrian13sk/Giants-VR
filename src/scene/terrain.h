// GPU terrain built from an original GTI heightfield, plus ground-height queries for
// locomotion. Geometry is split into chunks with bounds for culling.
#pragma once

#include <vector>

#include "assets/gti.h"
#include "render/renderer.h"

namespace gvr {

class Terrain {
public:
    bool build(const GtiTerrain& t, Renderer& r, TextureId ground, TextureId wall);
    void render(Renderer& r) const;
    void release(Renderer& r);  // frees the GPU meshes (switching worlds)
    // Ground height in runtime metres at runtime (x, z); returns false outside the terrain.
    bool heightAt(float rx, float rz, float& outY) const;
    // Runtime-space bounds of the valid terrain.
    Vec3 boundsMin() const { return bmin_; }
    Vec3 boundsMax() const { return bmax_; }
    // A reasonable spawn location on land (runtime metres).
    Vec3 suggestSpawn() const;
    uint32_t triangleCount() const { return tris_; }

private:
    struct Chunk {
        MeshId mesh = 0;
        TextureId texture = 0;
    };
    std::vector<Chunk> chunks_;
    GtiTerrain t_;
    Vec3 bmin_{0, 0, 0}, bmax_{0, 0, 0};
    uint32_t tris_ = 0;
};

std::vector<uint8_t> makeTerrainNoise(int size, uint32_t seed);

}  // namespace gvr
