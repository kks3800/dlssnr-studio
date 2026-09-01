#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dlssnr {

// RGBA half-float image (DXGI_FORMAT_R16G16B16A16_FLOAT), linear light.
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint16_t> texels;  // width*height*4 halves
    bool Valid() const { return width && height && texels.size() == size_t(width) * height * 4; }
};

// Single channel float image (DXGI_FORMAT_R32_FLOAT).
struct DepthImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> texels;
};

uint16_t FloatToHalf(float f);
float    HalfToFloat(uint16_t h);

// WIC-backed, and the result is always LINEAR light, which is what the NR
// runtime expects.
//
// Note we do no sRGB decode of our own: WIC's 128bppRGBAFloat is scRGB, which
// is linear by definition, so the format converter has already applied the
// transfer function. Decoding a second time costs roughly a 2.2 power and
// produces an image about fourteen times too dark.
bool LoadImageFile(const std::wstring& path, Image* out, std::string* error);

// bits_per_channel is 8 or 16. PNG integer formats are sRGB-encoded, so
// `encode_srgb` should normally be true; pass false only to dump raw linear
// values for inspection.
bool SaveImage(const std::wstring& path, const Image& img, int bits_per_channel,
               bool encode_srgb, std::string* error);

// Loads any WIC-readable image and takes its red channel as depth.
bool LoadDepth(const std::wstring& path, DepthImage* out, std::string* error);

}  // namespace dlssnr
