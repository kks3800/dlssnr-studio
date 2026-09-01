// EXR reading, via tinyexr.
//
// Needed because Unreal writes its AOVs as EXR and WIC cannot read them --
// and for good reason: velocity is signed (things move left and up too) and
// depth needs float range. Neither survives an 8- or 16-bit integer PNG.

// miniz is compiled as its own translation unit (see CMakeLists); including
// its .c here as well is what produced a wall of redefinition errors.
#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 1
#define TINYEXR_USE_THREAD 0
#include "tinyexr.h"

#include <cstring>
#include <string>
#include <vector>

#include "exr_io.h"

namespace dlssnr {
namespace {

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s;
    s.reserve(w.size());
    // EXR paths are ASCII in practice; anything exotic falls back to '?' and
    // tinyexr will report a clean open failure rather than misbehaving.
    for (wchar_t c : w) s.push_back(c < 128 ? char(c) : '?');
    return s;
}

}  // namespace

bool IsExrPath(const std::wstring& path) {
    if (path.size() < 4) return false;
    std::wstring tail = path.substr(path.size() - 4);
    for (auto& c : tail) c = wchar_t(::towlower(c));
    return tail == L".exr";
}

bool LoadExr(const std::wstring& path, ExrImage* out, std::string* error) {
    const std::string narrow = Narrow(path);
    const char* err = nullptr;

    auto fail = [&](const char* what) {
        if (error) *error = std::string("EXR load failed: ") + (err ? err : what);
        if (err) FreeEXRErrorMessage(err);
        return false;
    };

    // The low-level path rather than LoadEXR(), which insists on R, G and B
    // all being present. AOVs routinely are not RGB: a depth pass is often a
    // single channel and a velocity pass has only two, so channels are mapped
    // by name and anything absent is left at zero.
    EXRVersion version;
    if (ParseEXRVersionFromFile(&version, narrow.c_str()) != TINYEXR_SUCCESS) {
        return fail("not an EXR file");
    }
    EXRHeader header;
    InitEXRHeader(&header);
    if (ParseEXRHeaderFromFile(&header, &version, narrow.c_str(), &err) != TINYEXR_SUCCESS) {
        return fail("bad header");
    }
    // Ask for float regardless of whether the file stores half or float.
    for (int i = 0; i < header.num_channels; ++i) {
        if (header.pixel_types[i] == TINYEXR_PIXELTYPE_HALF) {
            header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
        }
    }
    EXRImage image;
    InitEXRImage(&image);
    if (LoadEXRImageFromFile(&image, &header, narrow.c_str(), &err) != TINYEXR_SUCCESS) {
        FreeEXRHeader(&header);
        return fail("could not read pixels");
    }

    const size_t n = size_t(image.width) * image.height;
    out->width = uint32_t(image.width);
    out->height = uint32_t(image.height);
    out->rgba.assign(n * 4, 0.0f);

    // Map by channel name. Depth passes turn up variously as R, Y or Z; all
    // three land in the red slot, which is where every consumer here looks.
    for (int c = 0; c < header.num_channels; ++c) {
        const std::string name = header.channels[c].name;
        int slot = -1;
        if (name == "R" || name == "Y" || name == "Z") slot = 0;
        else if (name == "G") slot = 1;
        else if (name == "B") slot = 2;
        else if (name == "A") slot = 3;
        if (slot < 0) continue;

        if (header.pixel_types[c] == TINYEXR_PIXELTYPE_UINT) continue;
        const float* src = reinterpret_cast<const float*>(image.images[c]);
        for (size_t i = 0; i < n; ++i) out->rgba[i * 4 + slot] = src[i];
    }
    // A single-channel image is more useful broadcast than left green-less.
    if (header.num_channels == 1) {
        for (size_t i = 0; i < n; ++i) {
            out->rgba[i * 4 + 1] = out->rgba[i * 4 + 0];
            out->rgba[i * 4 + 2] = out->rgba[i * 4 + 0];
        }
    }

    FreeEXRImage(&image);
    FreeEXRHeader(&header);
    return true;
}

}  // namespace dlssnr
