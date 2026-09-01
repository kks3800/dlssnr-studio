#include "motion_estimator.h"

#include "exr_io.h"

#include <onnxruntime_cxx_api.h>
#ifdef DLSSNR_HAVE_DIRECTML
#include <dml_provider_factory.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <thread>
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


float LinearToSrgb(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Nearest-neighbour resample of a colour image into planar NCHW, in 0..255.
// RAFT applies its own normalisation internally, so the network wants raw
// display-referred values rather than linear light.
void ToPlanar(const Image& img, int nw, int nh, float* dst) {
    const float sx = float(img.width) / float(nw);
    const float sy = float(img.height) / float(nh);
    for (int y = 0; y < nh; ++y) {
        const int y0 = std::min(int(img.height) - 1, int((y + 0.5f) * sy));
        for (int x = 0; x < nw; ++x) {
            const int x0 = std::min(int(img.width) - 1, int((x + 0.5f) * sx));
            const uint16_t* t = &img.texels[(size_t(y0) * img.width + x0) * 4];
            for (int c = 0; c < 3; ++c) {
                dst[size_t(c) * nh * nw + size_t(y) * nw + x] =
                    LinearToSrgb(HalfToFloat(t[c])) * 255.0f;
            }
        }
    }
}

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

struct MotionEstimator::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "dlssnr-flow"};
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;
    std::vector<std::string> inputs;
    std::string output;
    // Many exported flow models are frozen at one resolution. When the shape is
    // fixed we must match it exactly; -1 means the axis is dynamic.
    int fixed_w = -1, fixed_h = -1;
    std::string provider = "CPU";
};

MotionEstimator::MotionEstimator() : impl_(std::make_unique<Impl>()) {}
MotionEstimator::~MotionEstimator() = default;
bool MotionEstimator::Loaded() const { return impl_ && impl_->session != nullptr; }
std::string MotionEstimator::Provider() const {
    return impl_ ? impl_->provider : std::string("none");
}

bool MotionEstimator::Load(const std::wstring& model_path, std::string* error) {
    try {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(0);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        TryEnableDirectML(options, &impl_->provider);
        impl_->session = std::make_unique<Ort::Session>(impl_->env, model_path.c_str(), options);

        const size_t n = impl_->session->GetInputCount();
        if (n < 2) {
            if (error) {
                *error = "flow model expects " + std::to_string(n) +
                         " input(s); a two-frame optical flow model is required";
            }
            impl_->session.reset();
            return false;
        }
        impl_->inputs.clear();
        for (size_t i = 0; i < 2; ++i) {
            auto name = impl_->session->GetInputNameAllocated(i, impl_->allocator);
            impl_->inputs.emplace_back(name.get());
        }
        auto out = impl_->session->GetOutputNameAllocated(0, impl_->allocator);
        impl_->output = out.get();

        const auto shape =
            impl_->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() == 4) {
            if (shape[2] > 0) impl_->fixed_h = int(shape[2]);
            if (shape[3] > 0) impl_->fixed_w = int(shape[3]);
        }
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("could not load flow model: ") + e.what();
        impl_->session.reset();
        return false;
    }
}

bool MotionEstimator::Estimate(const Image& current, const Image& previous,
                               const MotionOptions& options, MotionField* out,
                               std::string* error) {
    if (!Loaded()) {
        if (error) *error = "flow model is not loaded";
        return false;
    }
    if (!current.Valid() || !previous.Valid() || current.width != previous.width ||
        current.height != previous.height) {
        if (error) *error = "flow needs two valid frames of identical size";
        return false;
    }

    // A frozen model dictates its own size; otherwise fit the longest side to
    // the requested resolution. RAFT downsamples by 8, so dimensions must be
    // multiples of 8. Aspect distortion from a fixed size is harmless here
    // because the x and y components are scaled back independently below.
    int nw, nh;
    if (impl_->fixed_w > 0 && impl_->fixed_h > 0) {
        nw = impl_->fixed_w;
        nh = impl_->fixed_h;
    } else {
        const int longest = std::max(8, (options.resolution / 8) * 8);
        const float s = float(longest) / float(std::max(current.width, current.height));
        auto round8 = [](float v) { return std::max(8, int(std::lround(v / 8.0f)) * 8); };
        nw = round8(float(current.width) * s);
        nh = round8(float(current.height) * s);
    }

    std::vector<float> a(size_t(3) * nw * nh), b(size_t(3) * nw * nh);
    ToPlanar(current, nw, nh, a.data());
    ToPlanar(previous, nw, nh, b.data());

    std::vector<float> flow;
    int ow = 0, oh = 0;
    try {
        const std::array<int64_t, 4> shape{1, 3, nh, nw};
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensors[2] = {
            Ort::Value::CreateTensor<float>(mem, a.data(), a.size(), shape.data(), shape.size()),
            Ort::Value::CreateTensor<float>(mem, b.data(), b.size(), shape.data(), shape.size()),
        };
        const char* in_names[2] = {impl_->inputs[0].c_str(), impl_->inputs[1].c_str()};
        const char* out_names[1] = {impl_->output.c_str()};
        auto results =
            impl_->session->Run(Ort::RunOptions{nullptr}, in_names, tensors, 2, out_names, 1);

        const auto dims = results[0].GetTensorTypeAndShapeInfo().GetShape();
        if (dims.size() < 3) {
            if (error) *error = "flow model returned an unexpected output rank";
            return false;
        }
        oh = int(dims[dims.size() - 2]);
        ow = int(dims[dims.size() - 1]);
        const float* data = results[0].GetTensorData<float>();
        flow.assign(data, data + size_t(2) * ow * oh);  // planar: x plane then y plane
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("flow inference failed: ") + e.what();
        return false;
    }

    // Scale vectors from network resolution up to source resolution.
    const float kx = float(current.width) / float(ow) * options.scale;
    const float ky = float(current.height) / float(oh) * options.scale;
    const float gx = float(ow) / float(current.width);
    const float gy = float(oh) / float(current.height);
    const float* plane_x = flow.data();
    const float* plane_y = flow.data() + size_t(ow) * oh;

    out->width = current.width;
    out->height = current.height;
    out->xy.resize(size_t(current.width) * current.height * 2);
    // Two bilinear samples for every pixel of the *source*, which at 4K is
    // 16.6 million of them -- far more work than the inference that produced
    // the flow, and it was the single most expensive step in a render. The rows
    // are independent, so this splits across cores.
    const uint32_t height = current.height;
    const uint32_t width = current.width;
    float* dst = out->xy.data();

    auto expand_rows = [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            const float fy = (y + 0.5f) * gy - 0.5f;
            float* row = dst + size_t(y) * width * 2;
            for (uint32_t x = 0; x < width; ++x) {
                const float fx = (x + 0.5f) * gx - 0.5f;
                row[x * 2 + 0] = SamplePlane(plane_x, ow, oh, fx, fy) * kx;
                row[x * 2 + 1] = SamplePlane(plane_y, ow, oh, fx, fy) * ky;
            }
        }
    };

    unsigned workers = std::thread::hardware_concurrency();
    if (workers == 0) workers = 4;
    workers = (std::min)(workers, height);
    if (workers <= 1) {
        expand_rows(0, height);
    } else {
        std::vector<std::future<void>> jobs;
        jobs.reserve(workers - 1);
        const uint32_t band = (height + workers - 1) / workers;
        for (unsigned w = 1; w < workers; ++w) {
            const uint32_t y0 = (std::min)(height, band * w);
            const uint32_t y1 = (std::min)(height, y0 + band);
            if (y0 < y1) {
                jobs.push_back(std::async(std::launch::async, expand_rows, y0, y1));
            }
        }
        expand_rows(0, (std::min)(height, band));  // this thread takes the first band
        for (auto& job : jobs) job.get();
    }
    return true;
}

void MotionEstimator::Statistics(const MotionField& field, float* max_magnitude,
                                 float* mean_magnitude) {
    double sum = 0.0;
    float peak = 0.0f;
    const size_t n = field.xy.size() / 2;
    for (size_t i = 0; i < n; ++i) {
        const float m = std::hypot(field.xy[i * 2], field.xy[i * 2 + 1]);
        sum += m;
        peak = std::max(peak, m);
    }
    if (max_magnitude) *max_magnitude = peak;
    if (mean_magnitude) *mean_magnitude = n ? float(sum / double(n)) : 0.0f;
}

Image MotionEstimator::Visualise(const MotionField& field) {
    Image out;
    if (!field.Valid()) return out;
    float peak = 0.0f;
    Statistics(field, &peak, nullptr);
    if (peak < 1e-6f) peak = 1.0f;

    out.width = field.width;
    out.height = field.height;
    out.texels.resize(size_t(field.width) * field.height * 4);
    for (size_t i = 0; i < size_t(field.width) * field.height; ++i) {
        const float vx = field.xy[i * 2], vy = field.xy[i * 2 + 1];
        const float mag = std::min(1.0f, std::hypot(vx, vy) / peak);
        float hue = std::atan2(vy, vx) / (2.0f * 3.14159265f);
        if (hue < 0.0f) hue += 1.0f;
        // HSV -> RGB with S = 1, V = magnitude.
        const float h6 = hue * 6.0f;
        const int sector = int(h6) % 6;
        const float f = h6 - std::floor(h6);
        const float p = 0.0f, q = mag * (1.0f - f), t = mag * f;
        float r = 0, g = 0, b = 0;
        switch (sector) {
            case 0: r = mag; g = t;   b = p;   break;
            case 1: r = q;   g = mag; b = p;   break;
            case 2: r = p;   g = mag; b = t;   break;
            case 3: r = p;   g = q;   b = mag; break;
            case 4: r = t;   g = p;   b = mag; break;
            default: r = mag; g = p;  b = q;   break;
        }
        // Stored linear; the viewer applies the sRGB encode.
        auto to_linear = [](float v) {
            return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        };
        out.texels[i * 4 + 0] = FloatToHalf(to_linear(r));
        out.texels[i * 4 + 1] = FloatToHalf(to_linear(g));
        out.texels[i * 4 + 2] = FloatToHalf(to_linear(b));
        out.texels[i * 4 + 3] = FloatToHalf(1.0f);
    }
    return out;
}

bool LoadMotionFile(const std::wstring& path, float scale_x, float scale_y, bool invert,
                    MotionField* out, std::string* error) {
    const float sign0 = invert ? -1.0f : 1.0f;
    // Velocity is signed and often outside 0..1, so an EXR is read as raw
    // float rather than being funnelled through the half-float image path.
    if (IsExrPath(path)) {
        ExrImage exr;
        if (!LoadExr(path, &exr, error)) return false;
        out->width = exr.width;
        out->height = exr.height;
        out->xy.resize(size_t(exr.width) * exr.height * 2);
        for (size_t i = 0; i < size_t(exr.width) * exr.height; ++i) {
            out->xy[i * 2 + 0] = exr.rgba[i * 4 + 0] * scale_x * sign0;
            out->xy[i * 2 + 1] = exr.rgba[i * 4 + 1] * scale_y * sign0;
        }
        return true;
    }

    Image rgba;
    if (!LoadImageFile(path, &rgba, error)) return false;
    out->width = rgba.width;
    out->height = rgba.height;
    out->xy.resize(size_t(rgba.width) * rgba.height * 2);
    const float sign = invert ? -1.0f : 1.0f;
    for (size_t i = 0; i < size_t(rgba.width) * rgba.height; ++i) {
        out->xy[i * 2 + 0] = HalfToFloat(rgba.texels[i * 4 + 0]) * scale_x * sign;
        out->xy[i * 2 + 1] = HalfToFloat(rgba.texels[i * 4 + 1]) * scale_y * sign;
    }
    return true;
}

}  // namespace dlssnr
