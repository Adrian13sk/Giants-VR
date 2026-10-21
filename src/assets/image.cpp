#include "assets/image.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_DEPRECATION_WARNING
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace gvr {

bool decodeImage(const uint8_t* data, size_t size, Image& out, std::string* error) {
    int w = 0, h = 0, comp = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp, 4);
    if (!px) {
        if (error) *error = stbi_failure_reason() ? stbi_failure_reason() : "decode failed";
        return false;
    }
    out.width = w;
    out.height = h;
    out.hadAlpha = (comp == 4 || comp == 2);
    out.rgba.assign(px, px + size_t(w) * h * 4);
    stbi_image_free(px);
    return true;
}

bool writePng(const std::string& path, int w, int h, const uint8_t* rgba, bool flip) {
    stbi_flip_vertically_on_write(flip ? 1 : 0);
    return stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4) != 0;
}

}  // namespace gvr
