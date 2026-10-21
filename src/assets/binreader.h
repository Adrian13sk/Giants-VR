// Bounds-checked little-endian reader. Reads past the end return zero and latch ok() == false.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace gvr {

class BinReader {
public:
    BinReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool ok() const { return ok_; }
    size_t pos() const { return p_; }
    size_t size() const { return n_; }
    size_t remaining() const { return p_ <= n_ ? n_ - p_ : 0; }
    void seek(size_t p) { if (p > n_) { ok_ = false; p_ = n_; } else p_ = p; }
    void skip(size_t k) { seek(p_ + k); }
    const uint8_t* ptr() const { return d_ + p_; }

    uint8_t u8() { uint8_t v = 0; get(&v, 1); return v; }
    uint16_t u16() { uint8_t b[2] = {}; get(b, 2); return uint16_t(b[0] | (b[1] << 8)); }
    uint32_t u32() {
        uint8_t b[4] = {};
        get(b, 4);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    float f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
    std::string fixedString(size_t len) {
        std::string s;
        if (p_ + len > n_) { ok_ = false; p_ = n_; return s; }
        const char* c = reinterpret_cast<const char*>(d_ + p_);
        s.assign(c, strnlen(c, len));
        p_ += len;
        return s;
    }

private:
    void get(void* out, size_t k) {
        if (p_ + k > n_) { ok_ = false; p_ = n_; std::memset(out, 0, k); return; }
        std::memcpy(out, d_ + p_, k);
        p_ += k;
    }
    const uint8_t* d_;
    size_t n_;
    size_t p_ = 0;
    bool ok_ = true;
};

}  // namespace gvr
