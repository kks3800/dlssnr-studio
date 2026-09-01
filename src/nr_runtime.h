#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "image_io.h"
#include "motion_estimator.h"

namespace dlssnr {

// Sentinel for "leave this key alone". It must sit outside every legal range:
// skin structure legitimately accepts -1, so -1 cannot mean "unset" the way it
// does for the others.
constexpr float kUnset = -1000.0f;

// Every tunable the recovered DLSSNR parameter block exposes. Unset values are
// left alone rather than forced, because the runtime's own defaults are what
// the in-game addon shows and overriding them blindly diverges from it.
//
// Ranges, confirmed against the addon's own sliders and by measurement:
//   intensity, local structure, local tone   0 .. 2, default 1
//   skin structure                          -1 .. 2, default 1
struct Settings {
    float intensity = kUnset;          // DLSSNR.Intensity  0..2
    // Style 2 and auto-mask on are the measured best combination: style 1 is
    // the over-cooked look, style 0 (the runtime default) warms and darkens,
    // and styles 2..6 are identical to each other and cleanest.
    int   style = 2;                   // DLSSNR.Style
    int   use_auto_mask = 1;           // DLSSNR.UseAutoMask
    int   ui_correction = -1;          // DLSSNR.UICorrection
    // The reference harness creates the feature with performance=3, preset=1;
    // these defaults mirror a known-good run rather than guessing.
    int   render_preset = 1;           // DLSSNR.Hint.Render.Preset (0..3)
    int   perf_quality = 3;            // PerfQualityValue at creation
    float skin_structure = kUnset;     // DLSSNR.SkinStructureStrength -1..2
    float local_structure = kUnset;    // DLSSNR.LocalStructureStrength 0..2
    float local_tone = kUnset;         // DLSSNR.LocalToneStrength      0..2
    float global_tone = kUnset;        // not a runtime key; addon-side only
    // DLSSNR.Backbuffer: the snippet reads this key but we never filled it.
    // In a renderer the backbuffer holds the previously presented image, so
    // this feeds the last output back in. 0 = leave unbound (the old
    // behaviour), 1 = previous output, 2 = previous input colour.
    int   use_backbuffer = 0;
    // DLSSNR.BidirectionalDistortionField: the last key the snippet reads and
    // we never filled. A per-pixel displacement field, same shape as the
    // motion vectors. 0 = unbound, 1 = zero field, 2 = a strong synthetic
    // swirl, which is what tells a field that is read apart from one that is
    // not -- an ignored parameter and a zero field look identical.
    int   use_distortion = 0;
    bool  upscaling = false;           // DLSSNR.Upscaling
    bool  depth_inverted = false;      // DLSSNR.DepthInverted

    // A still image has no motion, so the temporal network is fed the same
    // frame repeatedly and allowed to converge. Reference run showed
    // evaluation 1 cold-starts and 2..8 settle, hence the default of 8.
    int   iterations = 8;

    // Whether the first evaluation of this call clears NR's temporal history.
    // True is right for a one-off still. For a sequence, set it only on the
    // first frame: the accumulated history across frames is the entire point
    // of a temporal network, and resetting per frame throws it away.
    bool  reset_history = true;

    // 0 keeps the source dimensions.
    uint32_t output_width = 0;
    uint32_t output_height = 0;

    // Used when the caller supplies no depth map.
    float constant_depth = 0.5f;
};

struct EvaluationStat {
    int   index = 0;
    bool  finite = false;
    bool  non_black = false;
    bool  spatially_varying = false;
    float mean_absolute_rgb = 0.0f;
    float mean_absolute_difference_from_input = 0.0f;
    double gpu_wait_ms = 0.0;
};

struct Report {
    int feature_id = -1;                 // which NVSDK_NGX_Feature slot answered
    std::wstring adapter;
    std::vector<EvaluationStat> evaluations;
};

// Owns the D3D12 device and the NGX feature. Construct once, call Process as
// often as you like -- the feature is rebuilt only when the dimensions change,
// which is what makes interactive parameter tweaking affordable despite the
// ~950 ms per evaluation on Ampere.
// A GPU the runtime could run on, in DXGI high-performance order. The index
// into this list is what Runtime::Initialize accepts.
struct AdapterInfo {
    std::wstring name;
    uint64_t dedicated_vram = 0;  // bytes
    bool software = false;        // WARP and friends: listed, never chosen by default
};

std::vector<AdapterInfo> ListAdapters();

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // snippet_dll is the path to nvngx_dlssnr.dll. core_dll may be empty, in
    // which case the newest nvngx.dll in the driver store is used.
    // adapter_index selects from ListAdapters(); -1 keeps DXGI's own
    // high-performance pick, which is right on nearly every machine.
    bool Initialize(const std::wstring& snippet_dll, const std::wstring& core_dll,
                    std::string* error, int adapter_index = -1);

    // depth may be null; a constant plane is uploaded in that case.
    // control_mask may be null. It is DLSSNR.ControlMask -- the per-pixel
    // authoring input a game supplies to say where the effect should apply.
    // Without it the runtime falls back to its own automatic mask.
    bool Process(const Image& color, const DepthImage* depth, const DepthImage* control_mask,
                 const MotionField* motion, const Settings& settings, Image* out,
                 Report* report, std::string* error);

    const std::wstring& AdapterName() const { return adapter_name_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::wstring adapter_name_;
    unsigned ngx_api_version_ = 0;
};

// Locates the newest nvngx.dll under %WINDIR%\System32\DriverStore\FileRepository.
// Returns an empty string when nothing is found.
std::wstring FindNgxCore();

}  // namespace dlssnr
