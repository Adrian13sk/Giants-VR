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
    // The baked lightmap's colour modulation at runtime (x, z) (as the ground's vertex colour, 0..1).
    Vec3 lightAt(float rx, float rz) const;
    // The ground texture's coordinate at runtime (x, z) (as the mesh's), and that texture.
    Vec2 uvAt(float rx, float rz) const;
    TextureId groundTexture() const { return ground_; }
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
    TextureId ground_ = 0;
};

std::vector<uint8_t> makeTerrainNoise(int size, uint32_t seed);

}  // namespace gvr
