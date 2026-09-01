#pragma once

#include <string>
#include <vector>

namespace dlssnr {

// A numbered image sequence on disk, e.g. Seq_Scene_9_Bench_01.0000.png ...
struct Sequence {
    std::wstring directory;
    std::wstring prefix;     // everything before the frame number
    std::wstring extension;  // including the dot
    int digits = 0;          // zero-padding width
    int first = 0;
    int last = 0;
    std::vector<std::wstring> frames;  // full paths, ascending frame order

    bool Valid() const { return frames.size() > 1; }
    size_t Count() const { return frames.size(); }
};

// Accepts either a directory or any single frame within one. When given a
// directory containing several sequences, the largest is chosen. Frames are
// ordered numerically, not lexically, so unpadded numbering still sorts right.
bool DetectSequence(const std::wstring& path, Sequence* out, std::string* error);

// A render's set of passes. Unreal's Movie Render Queue writes each AOV either
// into its own subfolder (Velocity\shot.0000.exr) or with the pass name in the
// filename (shot.Velocity.0000.exr); both layouts are recognised.
struct PassSet {
    Sequence beauty;    // the colour render
    Sequence depth;     // SceneDepth / Depth / Z
    Sequence velocity;  // Velocity / MotionVectors / MVec
    bool HasDepth() const { return depth.Valid(); }
    bool HasVelocity() const { return velocity.Valid(); }
};

// Scans `root` (and one level of subfolders) and sorts what it finds into
// passes by name. The largest unclassified sequence becomes the beauty pass,
// so an ordinary single-sequence folder still works with no naming at all.
bool DetectPasses(const std::wstring& root, PassSet* out, std::string* error);

// Human-readable summary for logs and the UI, e.g.
//   "beauty 60 frames, depth 60, velocity 60".
std::string DescribePasses(const PassSet& passes);

// Builds an ffmpeg command line to encode `pattern` (a printf-style path such
// as out\frame.%04d.png) into `output`. Returns an empty string if ffmpeg is
// not on PATH.
std::wstring BuildEncodeCommand(const std::wstring& pattern, int first_frame,
                                const std::wstring& output, int fps, int crf);

bool FfmpegAvailable();

// Runs a command line with no console window and waits for it. The GUI needs
// this because _wsystem flashes a console, which strobes badly when scrubbing
// spawns ffmpeg on every seek.
bool RunCommandHidden(const std::wstring& command);

// Quotes one argument for a Windows command line, following the rules
// CommandLineToArgvW actually parses. Wrapping a path in bare quotes breaks in
// two cases that turn up in ordinary use: a directory path ending in a
// backslash escapes the closing quote, and a relative name starting with '-'
// is read by ffmpeg as an option rather than a file.
std::wstring QuoteArg(const std::wstring& argument);

// ---- video sources -------------------------------------------------------
// A video is turned into a frame sequence before NR sees it, because NR works
// on frames. Scrubbing does not extract the whole file though: seeking one
// frame is fast, and pulling 5000 frames off a 4K clip just to look at one of
// them would make picking a reference shot unusable.

struct VideoInfo {
    int width = 0, height = 0;
    double fps = 0.0;
    double duration = 0.0;  // seconds
    int frames = 0;
    bool has_audio = false;

    bool Valid() const { return width > 0 && height > 0 && frames > 0; }
};

bool ProbeVideo(const std::wstring& path, VideoInfo* out, std::string* error);

// One frame at `seconds`, written as PNG. This is the scrub path.
bool ExtractFrameAt(const std::wstring& src, double seconds,
                    const std::wstring& out_png, std::string* error);

// Every frame into `dir` as frame.%06d.png. This is the render path, and it
// is the slow one -- count on disk in the tens of GB for a long 4K clip.
bool ExtractAllFrames(const std::wstring& src, const std::wstring& dir,
                      std::string* error);

// Lifts the audio track aside so the neural pass never touches it, then
// BuildVideoEncodeCommand muxes it back over the enhanced frames.
bool ExtractAudio(const std::wstring& src, const std::wstring& out_audio,
                  std::string* error);

// Encode options for the finished master.
struct EncodeOptions {
    int fps = 24;
    int crf = 16;
    std::wstring audio;      // optional; muxed in when present
    std::wstring watermark;  // optional badge PNG, composited bottom-*
    std::wstring wm_position = L"bottom-right";
    double wm_margin_pct = 2.5;
};

std::wstring BuildVideoEncodeCommand(const std::wstring& pattern, int first_frame,
                                     const std::wstring& output,
                                     const EncodeOptions& options);

}  // namespace dlssnr
