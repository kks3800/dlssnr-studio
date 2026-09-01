#pragma once

#include <string>

namespace dlssnr {

// EU AI Act Art. 50 visible disclosure badge: a translucent ring with short
// text in it, composited over the finished video by ffmpeg.
//
// The visible mark is only half of the obligation. Art. 50(2) asks for a
// *machine-readable* marking, which a burned-in graphic is not, so the encode
// also writes the declaration into the container metadata. Neither is a
// substitute for the other.
struct BadgeOptions {
    int diameter = 96;            // pixels
    std::wstring text = L"AI";
    float opacity = 0.35f;        // 0..1, applied to the whole badge
};

// Renders the badge to a 32-bit PNG with alpha. Returns false and sets
// `error` if the file could not be written.
bool BuildBadgePng(const BadgeOptions& options, const std::wstring& out_path,
                   std::string* error);

// Badge diameter for a given output height, as a percentage of it -- so one
// setting looks the same at 1080p and at 4K.
int BadgeDiameterFor(int video_height, float size_pct);

}  // namespace dlssnr
