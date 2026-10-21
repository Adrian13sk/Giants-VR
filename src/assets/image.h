// Image decoding for original textures (TGA; also BMP/PNG via stb_image).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gvr {

struct Image {
    int width = 0, height = 0;
    bool hadAlpha = false;        // source had an alpha channel
    std::vector<uint8_t> rgba;    // rows top-to-bottom
};

bool decodeImage(const uint8_t* data, size_t size, Image& out, std::string* error = nullptr);
bool writePng(const std::string& path, int w, int h, const uint8_t* rgba, bool flipVertically);

}  // namespace gvr
