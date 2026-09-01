#include "depth_estimator.h"

#include <onnxruntime_cxx_api.h>
#ifdef DLSSNR_HAVE_DIRECTML
#include <dml_provider_factory.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace dlssnr {
namespace {
// Puts the session on the GPU through DirectML when it is available, and says
// so, because the difference is not subtle: RAFT was ~900 ms a frame on the CPU
// provider, which was 96% of a render.
//
// DirectML rather than CUDA deliberately -- this application is already a D3D12
// program, so there is no CUDA or cuDNN runtime to ship or to match against the
// installed driver, and it works on any DX12 GPU rather than only NVIDIA ones.
//
// Falls back to CPU rather than failing: a machine without a usable DX12 device
// should still run, only slowly.
bool TryEnableDirectML(Ort::SessionOptions& options, std::string* provider) {
#ifdef DLSSNR_HAVE_DIRECTML
    // DirectML requires both of these: it allocates its own way and does not
    // support ORT's parallel execution.
    options.DisableMemPattern();
    options.SetExecutionMode(ORT_SEQUENTIAL);
    const OrtStatus* status =
        OrtSessionOptionsAppendExecutionProvider_DML(options, 0);
    if (status == nullptr) {
        if (provider) *provider = "DirectML";
        return true;
    }
    Ort::GetApi().ReleaseStatus(const_cast<OrtStatus*>(status));
    // Undo the DML-specific settings so the CPU path runs at its own best.
    options.EnableMemPattern();
    options.SetExecutionMode(ORT_PARALLEL);
#endif
    if (provider) *provider = "CPU";
    return false;
}


// ImageNet normalisation, which is what the Depth Anything preprocessing uses.
constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};

float LinearToSrgb(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Bilinear sample of a single channel plane.
float SamplePlane(const float* plane, int w, int h, float x, float y) {
    x = std::clamp(x, 0.0f, float(w - 1));
    y = std::clamp(y, 0.0f, float(h - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    const float a = plane[size_t(y0) * w + x0], b = plane[size_t(y0) * w + x1];
    const float c = plane[size_t(y1) * w + x0], d = plane[size_t(y1) * w + x1];
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

}  // namespace

struct DepthEstimator::Impl {
    std::string provider = "CPU";
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "dlssnr-depth"};
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;
    std::string input_name, output_name;
};

DepthEstimator::DepthEstimator() : impl_(std::make_unique<Impl>()) {}
DepthEstimator::~DepthEstimator() = default;

bool DepthEstimator::Loaded() const { return impl_ && impl_->session != nullptr; }

bool DepthEstimator::Load(const std::wstring& model_path, std::string* error) {
    try {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(0);  // let ORT pick when it lands on the CPU
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        TryEnableDirectML(options, &impl_->provider);
        impl_->session =
            std::make_unique<Ort::Session>(impl_->env, model_path.c_str(), options);

        // Query the names rather than hard-coding them -- different exports of
        // this model disagree ("pixel_values"/"image", "predicted_depth"/"depth").
        auto in = impl_->session->GetInputNameAllocated(0, impl_->allocator);
        auto out = impl_->session->GetOutputNameAllocated(0, impl_->allocator);
        impl_->input_name = in.get();
        impl_->output_name = out.get();
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("could not load depth model: ") + e.what();
        impl_->session.reset();
        return false;
    }
}

bool DepthEstimator::Estimate(const Image& color, const DepthOptions& options, DepthImage* out,
                              std::string* error) {
    if (!Loaded()) {
        if (error) *error = "depth model is not loaded";
        return false;
    }
    if (!color.Valid()) {
        if (error) *error = "input image is invalid";
        return false;
    }

    // ViT patch size is 14, so both network dimensions must be multiples of it.
    const int longest = std::max(14, (options.resolution / 14) * 14);
    const float scale = float(longest) / float(std::max(color.width, color.height));
    auto round14 = [](float v) {
        return std::max(14, int(std::lround(v / 14.0f)) * 14);
    };
    const int nw = round14(float(color.width) * scale);
    const int nh = round14(float(color.height) * scale);

    // Resample into NCHW, converting to sRGB first: the network was trained on
    // ordinary display-referred images, not linear light.
    std::vector<float> input(size_t(3) * nw * nh);
    const float sx = float(color.width) / float(nw);
    const float sy = float(color.height) / float(nh);
    for (int y = 0; y < nh; ++y) {
        const float srcy = std::min(float(color.height - 1), (y + 0.5f) * sy - 0.5f);
        const int y0 = int(std::clamp(srcy, 0.0f, float(color.height - 1)));
        for (int x = 0; x < nw; ++x) {
            const float srcx = std::min(float(color.width - 1), (x + 0.5f) * sx - 0.5f);
            const int x0 = int(std::clamp(srcx, 0.0f, float(color.width - 1)));
            const uint16_t* texel = &color.texels[(size_t(y0) * color.width + x0) * 4];
            for (int c = 0; c < 3; ++c) {
                const float v = LinearToSrgb(HalfToFloat(texel[c]));
                input[size_t(c) * nh * nw + size_t(y) * nw + x] = (v - kMean[c]) / kStd[c];
            }
        }
    }

    std::vector<float> raw;
    int ow = 0, oh = 0;
    try {
        const std::array<int64_t, 4> shape{1, 3, nh, nw};
        auto meminfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(meminfo, input.data(), input.size(),
                                                           shape.data(), shape.size());
        const char* in_names[] = {impl_->input_name.c_str()};
        const char* out_names[] = {impl_->output_name.c_str()};
        auto results = impl_->session->Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1,
                                           out_names, 1);
        auto info = results[0].GetTensorTypeAndShapeInfo();
        const auto dims = info.GetShape();
        // Accept (1,H,W) or (1,1,H,W).
        if (dims.size() >= 2) {
            oh = int(dims[dims.size() - 2]);
            ow = int(dims[dims.size() - 1]);
        }
        if (ow <= 0 || oh <= 0) {
            if (error) *error = "depth model returned an unexpected output shape";
            return false;
        }
        const float* data = results[0].GetTensorData<float>();
        raw.assign(data, data + size_t(ow) * oh);
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("depth inference failed: ") + e.what();
        return false;
    }

    // The prediction is relative inverse depth on an arbitrary scale, so
    // normalise per image before doing anything else.
    float lo = raw[0], hi = raw[0];
    for (float v : raw) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const float range = (hi - lo) > 1e-8f ? (hi - lo) : 1.0f;

    out->width = color.width;
    out->height = color.height;
    out->texels.resize(size_t(color.width) * color.height);
    const float gx = float(ow) / float(color.width);
    const float gy = float(oh) / float(color.height);
    const float span = options.far_value - options.near_value;

    for (uint32_t y = 0; y < color.height; ++y) {
        for (uint32_t x = 0; x < color.width; ++x) {
            float v = SamplePlane(raw.data(), ow, oh, (x + 0.5f) * gx - 0.5f,
                                  (y + 0.5f) * gy - 0.5f);
            v = (v - lo) / range;                       // 0..1, near = 1
            if (options.invert) v = 1.0f - v;           // near = 0, like a depth buffer
            if (options.gamma != 1.0f) v = std::pow(std::max(v, 0.0f), options.gamma);
            out->texels[size_t(y) * color.width + x] = options.near_value + v * span;
        }
    }
    return true;
}

Image DepthEstimator::Visualise(const DepthImage& depth) {
    Image out;
    if (depth.texels.empty()) return out;
    out.width = depth.width;
    out.height = depth.height;
    out.texels.resize(size_t(depth.width) * depth.height * 4);
    float lo = depth.texels[0], hi = depth.texels[0];
    for (float v : depth.texels) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const float range = (hi - lo) > 1e-8f ? (hi - lo) : 1.0f;
    for (size_t i = 0; i < depth.texels.size(); ++i) {
        const float n = (depth.texels[i] - lo) / range;
        // Displayed through the same sRGB encode as everything else, so store
        // the linearised value here.
        const float linear = n <= 0.04045f ? n / 12.92f
                                           : std::pow((n + 0.055f) / 1.055f, 2.4f);
        const uint16_t h = FloatToHalf(linear);
        out.texels[i * 4 + 0] = h;
        out.texels[i * 4 + 1] = h;
        out.texels[i * 4 + 2] = h;
        out.texels[i * 4 + 3] = FloatToHalf(1.0f);
    }
    return out;
}

}  // namespace dlssnr
