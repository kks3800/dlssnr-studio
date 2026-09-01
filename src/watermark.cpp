#include "watermark.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dlssnr {
namespace {

// Drawn at this multiple and box-filtered down. The ring is thin enough at
// small diameters that unsupersampled edges crawl visibly once the badge sits
// over moving footage.
constexpr int kSupersample = 4;

struct Rgba {
    float r = 0, g = 0, b = 0, a = 0;  // premultiplied
};

// Standard source-over. Premultiplied throughout, which is also what makes the
// box filter below correct -- averaging straight alpha bleeds the dark disc
// into the white ring and leaves a grey halo.
void Over(Rgba* dst, float r, float g, float b, float a) {
    dst->r = r * a + dst->r * (1.0f - a);
    dst->g = g * a + dst->g * (1.0f - a);
    dst->b = b * a + dst->b * (1.0f - a);
    dst->a = a + dst->a * (1.0f - a);
}

// Rasterises the glyphs once via GDI and hands back an 8-bit coverage mask.
// Doing our own glyph rasterisation would be a lot of code for no gain.
std::vector<uint8_t> RenderTextMask(int size, const std::wstring& text, int box) {
    std::vector<uint8_t> mask(size_t(size) * size, 0);
    if (text.empty() || box <= 0) return mask;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dc || !dib || !bits) {
        if (dib) DeleteObject(dib);
        if (dc) DeleteDC(dc);
        return mask;
    }
    HGDIOBJ old_bitmap = SelectObject(dc, dib);
    std::memset(bits, 0, size_t(size) * size * 4);

    // Shrink until the string fits the inner box on both axes.
    int height = box;
    HFONT font = nullptr;
    SIZE extent{};
    for (int guard = 0; guard < 40 && height > 4; ++guard) {
        if (font) DeleteObject(font);
        font = CreateFontW(-height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
                           L"Segoe UI");
        HGDIOBJ old_font = SelectObject(dc, font);
        GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &extent);
        SelectObject(dc, old_font);
        if (extent.cx <= box && extent.cy <= box) break;
        height = int(height * 0.92f);
    }

    if (font) {
        HGDIOBJ old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, (size - extent.cx) / 2, (size - extent.cy) / 2, text.c_str(),
                 int(text.size()));
        SelectObject(dc, old_font);
        DeleteObject(font);
    }

    GdiFlush();
    const auto* pixels = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = pixels[i * 4];  // white on black: any channel is the coverage
    }

    SelectObject(dc, old_bitmap);
    DeleteObject(dib);
    DeleteDC(dc);
    return mask;
}

// WIC needs COM, and the badge is reachable from the CLI, which has not
// necessarily initialised it yet. RPC_E_CHANGED_MODE fails SUCCEEDED(), so an
// apartment that is already up a different way is left alone rather than being
// torn down underneath its owner.
struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

bool WritePng(const std::wstring& path, int width, int height,
              const std::vector<uint8_t>& bgra, std::string* error) {
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        if (error) *error = "could not create the WIC factory";
        return false;
    }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        if (error) *error = "could not open the badge file for writing";
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr))) {
        if (error) *error = "could not initialise the PNG encoder";
        return false;
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(width, height)) ||
        FAILED(frame->SetPixelFormat(&format))) {
        if (error) *error = "encoder rejected the badge format";
        return false;
    }

    const UINT stride = UINT(width) * 4;
    if (FAILED(frame->WritePixels(height, stride, UINT(bgra.size()),
                                  const_cast<BYTE*>(bgra.data()))) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        if (error) *error = "could not write the badge PNG";
        return false;
    }
    return true;
}

}  // namespace

int BadgeDiameterFor(int video_height, float size_pct) {
    const int diameter = int(video_height * size_pct / 100.0f);
    return std::max(24, diameter);
}

bool BuildBadgePng(const BadgeOptions& options, const std::wstring& out_path,
                   std::string* error) {
    const int diameter = std::max(16, options.diameter);
    const int size = diameter * kSupersample;
    const float radius = size * 0.5f;
    const float ring = std::max(float(kSupersample) * 2.0f, size * 0.075f);
    const float ring_radius = radius - ring * 0.5f;
    const float centre = radius - 0.5f;

    // Text is fitted to the square inside the ring, not the whole circle.
    const int inner_box = int((size - 2.0f * ring) * 0.72f);
    const std::vector<uint8_t> text_mask =
        RenderTextMask(size, options.text, inner_box);

    std::vector<Rgba> canvas(size_t(size) * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = x - centre, dy = y - centre;
            const float d = std::sqrt(dx * dx + dy * dy);
            Rgba& px = canvas[size_t(y) * size + x];

            // A dark disc under the ring is what keeps the mark readable over
            // bright footage; without it the badge disappears on a sky shot.
            if (d <= radius) Over(&px, 0.0f, 0.0f, 0.0f, 0.353f);
            if (std::fabs(d - ring_radius) <= ring * 0.5f) {
                Over(&px, 1.0f, 1.0f, 1.0f, 1.0f);
            }
            const float coverage = text_mask[size_t(y) * size + x] / 255.0f;
            if (coverage > 0.0f) Over(&px, 1.0f, 1.0f, 1.0f, coverage);
        }
    }

    const float opacity = (options.opacity > 0.0f && options.opacity <= 1.0f)
                              ? options.opacity
                              : 0.35f;
    const float inv = 1.0f / (kSupersample * kSupersample);
    std::vector<uint8_t> bgra(size_t(diameter) * diameter * 4, 0);

    for (int y = 0; y < diameter; ++y) {
        for (int x = 0; x < diameter; ++x) {
            Rgba sum;
            for (int sy = 0; sy < kSupersample; ++sy) {
                for (int sx = 0; sx < kSupersample; ++sx) {
                    const Rgba& s =
                        canvas[size_t(y * kSupersample + sy) * size + x * kSupersample + sx];
                    sum.r += s.r;
                    sum.g += s.g;
                    sum.b += s.b;
                    sum.a += s.a;
                }
            }
            sum.r *= inv; sum.g *= inv; sum.b *= inv; sum.a *= inv;

            // WIC wants straight alpha, so undo the premultiply before the
            // global opacity is folded in.
            const float a = std::clamp(sum.a, 0.0f, 1.0f);
            const float scale = a > 1e-4f ? 1.0f / a : 0.0f;
            uint8_t* out = &bgra[(size_t(y) * diameter + x) * 4];
            out[0] = uint8_t(std::clamp(sum.b * scale, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[1] = uint8_t(std::clamp(sum.g * scale, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[2] = uint8_t(std::clamp(sum.r * scale, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[3] = uint8_t(a * opacity * 255.0f + 0.5f);
        }
    }

    return WritePng(out_path, diameter, diameter, bgra, error);
}

}  // namespace dlssnr
