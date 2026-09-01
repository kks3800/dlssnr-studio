#include "image_io.h"

#include "exr_io.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace dlssnr {
namespace {

struct ComInit {
    ComInit() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
    HRESULT hr = E_FAIL;
};

bool MakeFactory(ComPtr<IWICImagingFactory>* factory, std::string* error) {
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory->GetAddressOf()));
    if (FAILED(hr)) {
        if (error) *error = "CoCreateInstance(WICImagingFactory) failed";
        return false;
    }
    return true;
}

float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float c) {
    if (c <= 0.0f) return 0.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

// IEEE 754 binary32 -> binary16, round-to-nearest-even, with denormal and
// overflow handling. Kept branch-explicit rather than clever; it runs once per
// texel on load, never in the hot path.
uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t raw_exp = (x >> 23) & 0xFFu;
    int32_t exp = int32_t(raw_exp) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;

    if (raw_exp == 0xFFu) {  // Inf / NaN
        return uint16_t(sign | 0x7C00u | (mant ? 0x200u : 0u));
    }
    if (exp >= 0x1F) return uint16_t(sign | 0x7C00u);  // overflow -> Inf
    if (exp <= 0) {
        if (exp < -10) return uint16_t(sign);  // underflow -> zero
        mant |= 0x800000u;
        const uint32_t shift = uint32_t(14 - exp);
        const uint32_t half_mant = mant >> shift;
        const uint32_t remainder = mant & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        uint32_t rounded = half_mant;
        if (remainder > halfway || (remainder == halfway && (half_mant & 1u))) ++rounded;
        return uint16_t(sign | rounded);
    }
    const uint32_t half_mant = mant >> 13;
    const uint32_t remainder = mant & 0x1FFFu;
    uint32_t bits = (uint32_t(exp) << 10) | half_mant;
    // A carry here rolls into the exponent field, which is the correct result.
    if (remainder > 0x1000u || (remainder == 0x1000u && (half_mant & 1u))) ++bits;
    return uint16_t(sign | bits);
}

float HalfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t out;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            exp = 1;
            while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            out = sign | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        out = sign | 0x7F800000u | (mant << 13);
    } else {
        out = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &out, sizeof(f));
    return f;
}

bool LoadImageFile(const std::wstring& path, Image* out, std::string* error) {
    // EXR goes through tinyexr. Its values are already linear and may exceed
    // 1.0 or go negative, both of which are meaningful for AOVs, so nothing is
    // clamped or transformed on the way in.
    if (IsExrPath(path)) {
        ExrImage exr;
        if (!LoadExr(path, &exr, error)) return false;
        out->width = exr.width;
        out->height = exr.height;
        out->texels.resize(exr.rgba.size());
        for (size_t i = 0; i < exr.rgba.size(); ++i) out->texels[i] = FloatToHalf(exr.rgba[i]);
        return true;
    }

    ComInit com;
    ComPtr<IWICImagingFactory> factory;
    if (!MakeFactory(&factory, error)) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, &decoder))) {
        if (error) *error = "could not open image (unsupported format or missing file)";
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        if (error) *error = "could not read frame 0";
        return false;
    }
    // 128bppRGBAFloat is scRGB -- linear. The converter applies the sRGB
    // transfer function for us, so the buffer below is already linear light and
    // must not be decoded again.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat128bppRGBAFloat,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        if (error) *error = "could not convert source to 128bpp float RGBA";
        return false;
    }
    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (!w || !h) {
        if (error) *error = "image has zero extent";
        return false;
    }
    std::vector<float> scratch(size_t(w) * h * 4);
    const UINT stride = w * 4 * sizeof(float);
    if (FAILED(converter->CopyPixels(nullptr, stride, UINT(scratch.size() * sizeof(float)),
                                     reinterpret_cast<BYTE*>(scratch.data())))) {
        if (error) *error = "CopyPixels failed";
        return false;
    }

    out->width = w;
    out->height = h;
    out->texels.resize(scratch.size());
    for (size_t i = 0; i < scratch.size(); ++i) {
        out->texels[i] = FloatToHalf(scratch[i]);
    }
    return true;
}

bool SaveImage(const std::wstring& path, const Image& img, int bits_per_channel,
               bool encode_srgb, std::string* error) {
    if (!img.Valid()) {
        if (error) *error = "refusing to save an invalid image";
        return false;
    }
    ComInit com;
    ComPtr<IWICImagingFactory> factory;
    if (!MakeFactory(&factory, error)) return false;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, &props)) ||
        FAILED(frame->Initialize(props.Get()))) {
        if (error) *error = "could not create PNG encoder";
        return false;
    }

    const bool sixteen = bits_per_channel == 16;
    // The PNG encoder's native 8-bit layout is BGR, not RGB; asking for RGB
    // gets silently substituted and then fails the equality check below.
    const WICPixelFormatGUID requested =
        sixteen ? GUID_WICPixelFormat48bppRGB : GUID_WICPixelFormat24bppBGR;
    WICPixelFormatGUID fmt = requested;
    if (FAILED(frame->SetSize(img.width, img.height)) ||
        FAILED(frame->SetPixelFormat(&fmt)) ||
        std::memcmp(&fmt, &requested, sizeof(fmt)) != 0) {
        if (error) *error = "encoder rejected the requested pixel format";
        return false;
    }
    const int channel_order[3] = {sixteen ? 0 : 2, 1, sixteen ? 2 : 0};

    const size_t channels = size_t(img.width) * 3;
    std::vector<uint8_t> row8;
    std::vector<uint16_t> row16;
    if (sixteen) row16.resize(channels); else row8.resize(channels);

    for (uint32_t y = 0; y < img.height; ++y) {
        const uint16_t* src = img.texels.data() + size_t(y) * img.width * 4;
        for (uint32_t x = 0; x < img.width; ++x) {
            for (int c = 0; c < 3; ++c) {
                float v = HalfToFloat(src[size_t(x) * 4 + c]);
                if (encode_srgb) v = LinearToSrgb(v);
                v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                const size_t slot = size_t(x) * 3 + channel_order[c];
                if (sixteen) row16[slot] = uint16_t(v * 65535.0f + 0.5f);
                else         row8[slot]  = uint8_t(v * 255.0f + 0.5f);
            }
        }
        const UINT stride = UINT(channels * (sixteen ? 2 : 1));
        BYTE* data = sixteen ? reinterpret_cast<BYTE*>(row16.data())
                             : reinterpret_cast<BYTE*>(row8.data());
        if (FAILED(frame->WritePixels(1, stride, stride, data))) {
            if (error) *error = "WritePixels failed";
            return false;
        }
    }
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        if (error) *error = "commit failed";
        return false;
    }
    return true;
}

bool LoadDepth(const std::wstring& path, DepthImage* out, std::string* error) {
    Image rgba;
    if (!LoadImageFile(path, &rgba, error)) return false;
    out->width = rgba.width;
    out->height = rgba.height;
    out->texels.resize(size_t(rgba.width) * rgba.height);
    for (size_t i = 0; i < out->texels.size(); ++i) {
        out->texels[i] = HalfToFloat(rgba.texels[i * 4]);
    }
    return true;
}

}  // namespace dlssnr
