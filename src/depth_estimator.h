#pragma once

#include <memory>
#include <string>

#include "image_io.h"

namespace dlssnr {

// Monocular depth estimation (Depth Anything V2) via ONNX Runtime.
//
// The network predicts *relative inverse depth* -- larger means nearer, on an
// arbitrary per-image scale. A rendered Z buffer is a different animal
// entirely, so the result is remapped before it reaches NR and the controls
// below exist because the exact convention NR wants is not documented.
// If you have a real depth pass out of Unreal or Blender, use that instead:
// it is strictly better than anything estimated from pixels.
struct DepthOptions {
    // Longest-side resolution fed to the network. Must be a multiple of 14
    // (ViT patch size); the class rounds for you. Larger is sharper and slower.
    int  resolution = 518;

    // The network's near=high convention inverted to near=low, matching a
    // conventional non-reversed depth buffer.
    bool invert = true;

    // Output range after normalisation. Narrowing this keeps the map away from
    // the extremes of the buffer, which is often what a real projection does.
    float near_value = 0.0f;
    float far_value = 1.0f;

    // Contrast on the normalised map. 1.0 is linear; higher pushes detail
    // toward the near field.
    float gamma = 1.0f;
};

class DepthEstimator {
public:
    DepthEstimator();
    ~DepthEstimator();
    DepthEstimator(const DepthEstimator&) = delete;
    DepthEstimator& operator=(const DepthEstimator&) = delete;

    // model_path is the .onnx file. Safe to call once and reuse.
    bool Load(const std::wstring& model_path, std::string* error);
    bool Loaded() const;

    // `color` is linear light (as LoadImage returns). The estimate is produced
    // at the network's working resolution and resampled to the source size.
    bool Estimate(const Image& color, const DepthOptions& options, DepthImage* out,
                  std::string* error);

    // Greyscale visualisation of a depth map, for display in the UI.
    static Image Visualise(const DepthImage& depth);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dlssnr
