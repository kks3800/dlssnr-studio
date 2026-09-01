#pragma once

#include <memory>
#include <string>
#include <vector>

#include "image_io.h"

namespace dlssnr {

// Per-pixel screen-space motion, in pixels of the source resolution.
// Convention matches what NR expects from a game: adding the vector to a pixel
// position gives where that pixel was in the PREVIOUS frame.
struct MotionField {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> xy;  // 2 floats per texel

    bool Valid() const {
        return width && height && xy.size() == size_t(width) * height * 2;
    }
};

struct MotionOptions {
    // Longest side fed to the network; rounded to a multiple of 8. Flow is
    // estimated here and scaled up, because RAFT at native 4K is impractical.
    int resolution = 512;
    // Extra multiplier applied to the result, for dialling the strength.
    float scale = 1.0f;
};

// Optical flow (RAFT) via ONNX Runtime, for footage rendered without a
// velocity pass. A real engine-exported velocity buffer is always better:
// it is exact, includes motion this cannot see (transparency, shadows), and
// costs nothing at runtime. Use LoadMotionFile for that case.
class MotionEstimator {
public:
    MotionEstimator();
    ~MotionEstimator();
    MotionEstimator(const MotionEstimator&) = delete;
    MotionEstimator& operator=(const MotionEstimator&) = delete;

    bool Load(const std::wstring& model_path, std::string* error);
    bool Loaded() const;

    // "DirectML" or "CPU" -- which one you got matters enough to surface.
    std::string Provider() const;

    // Flow from `current` back to `previous`.
    bool Estimate(const Image& current, const Image& previous, const MotionOptions& options,
                  MotionField* out, std::string* error);

    // Largest and mean vector magnitude, for sanity-checking the result.
    static void Statistics(const MotionField& field, float* max_magnitude,
                           float* mean_magnitude);

    // Direction as hue, magnitude as value -- the usual flow visualisation.
    static Image Visualise(const MotionField& field);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reads an engine-exported velocity pass. R and G are taken as the motion
// vector. `scale` converts the file's units into pixels: pass the render
// width/height if the pass is in normalised screen space, or 1.0 if it is
// already in pixels. `invert` flips the sign for engines that store
// previous->current instead of current->previous.
bool LoadMotionFile(const std::wstring& path, float scale_x, float scale_y, bool invert,
                    MotionField* out, std::string* error);

}  // namespace dlssnr
