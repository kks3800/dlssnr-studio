#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dlssnr {

// Raw float RGBA straight out of an EXR, unclamped and untransformed. Depth
// lands in R; a velocity pass lands in R and G and may be negative.
struct ExrImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgba;
};

bool IsExrPath(const std::wstring& path);
bool LoadExr(const std::wstring& path, ExrImage* out, std::string* error);

}  // namespace dlssnr
