#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <future>
#include <thread>
#include <cmath>
#include <filesystem>
#include <vector>
#include <string>

#include "depth_estimator.h"
#include "sequence.h"
#include "watermark.h"
#include "video_stream.h"
#include "motion_estimator.h"
#include "image_io.h"
#include "nr_runtime.h"

namespace {

void Usage() {
    std::printf(
        "dlssnr-image -- DLSS 5 Neural Rendering over stills, sequences and video\n"
        "\n"
        "  dlssnr-image <input> -o <output.png> [options]        one still image\n"
        "  dlssnr-image --sequence <dir|frame> [options]         rendered frames\n"
        "  dlssnr-image --video-in <clip> -o <out.mp4> [options] a video file\n"
        "  dlssnr-image --list-gpus\n"
        "\n"
        "Runtime:\n"
        "  --snippet <path>       nvngx_dlssnr.dll (default: beside this exe)\n"
        "  --core <path>          nvngx.dll (default: newest in the driver store)\n"
        "  --gpu <n>              adapter index from --list-gpus (default: DXGI's pick)\n"
        "\n"
        "Still image:\n"
        "  --depth <file>         depth map, red channel, must match input size\n"
        "  --depth-constant <f>   flat depth when no map is given (default 0.5)\n"
        "  --depth-inverted       reversed-Z depth\n"
        "  --estimate-depth       monocular depth via Depth Anything V2\n"
        "  --depth-out <file>     also write the estimated depth as a PNG\n"
        "  --depth-res <n>        estimator input size, longest side (default 518)\n"
        "  --depth-gamma <f>      gamma applied to the estimated depth\n"
        "  --depth-no-invert      keep the estimator's near=high convention\n"
        "  --mask <file>          DLSSNR.ControlMask; white applies, black suppresses\n"
        "  --linear               write raw linear values; skip the sRGB encode\n"
        "  --bits <8|16>          output PNG bit depth (default 16)\n"
        "  --width <n> --height <n>  output size; enables upscaling when larger\n"
        "\n"
        "Sequence (--sequence): depth and velocity passes are found beside the\n"
        "frames automatically, EXR included.\n"
        "  --out-dir <dir>        where frames go (default <sequence>/nr_out)\n"
        "  --video <file>         encode the frames with ffmpeg afterwards\n"
        "  --fps <n>              frame rate for --video (default 24)\n"
        "  --mvec <dir>           velocity pass to use instead of the detected one\n"
        "  --mvec-scale <f>       multiply the velocity pass by this\n"
        "  --mvec-ndc             velocity is normalised screen space, not pixels\n"
        "  --mvec-invert          velocity stores previous->current\n"
        "  --depth-seq <dir>      depth pass to use instead of the detected one\n"
        "  --no-auto-passes       do not look for passes beside the frames\n"
        "\n"
        "Video (--video-in): frames stream decoder -> NR -> encoder; needs ffmpeg.\n"
        "  --from <n>             first frame to render (default 0)\n"
        "  --frames <n>           how many frames (default: to the end)\n"
        "  --mv-out <file>        write the first frame's optical flow as a PNG\n"
        "\n"
        "Motion (sequence and video):\n"
        "  --estimate-motion      optical flow via RAFT, for footage with no pass\n"
        "  --motion-res <n>       flow input size, longest side (default 512)\n"
        "  --motion-scale <f>     multiply the estimated flow by this\n"
        "\n"
        "Neural rendering (unset values stay at the runtime's own defaults):\n"
        "  --iterations <n>       temporal passes per image (default 8)\n"
        "  --intensity <f>        DLSSNR.Intensity              0..1, clamps at 1\n"
        "  --style <n>            DLSSNR.Style                  default 2\n"
        "  --skin <f>             DLSSNR.SkinStructureStrength -1..2\n"
        "  --local-structure <f>  DLSSNR.LocalStructureStrength 0..2\n"
        "  --local-tone <f>       DLSSNR.LocalToneStrength      0..2\n"
        "  --auto-mask <0|1>      DLSSNR.UseAutoMask            default 1\n"
        "  --ui-correction <0|1>  DLSSNR.UICorrection\n"
        "  --preset <n>           DLSSNR.Hint.Render.Preset     measured inert\n"
        "  --global-tone <f>      addon-side control, not a runtime key\n"
        "\n"
        "Experimental inputs (the snippet reads these keys; effect unverified):\n"
        "  --backbuffer <0|1|2>   DLSSNR.Backbuffer: 0 unbound, 1 previous output,\n"
        "                         2 the input colour itself\n"
        "  --distortion <0|1|2>   DLSSNR.BidirectionalDistortionField: 0 unbound,\n"
        "                         1 zero field, 2 a synthetic test swirl\n"
        "\n"
        "Sweeps and tools:\n"
        "  --sweep <param> --sweep-values a,b,c\n"
        "                         contact sheet of 1:1 crops, one per value\n"
        "  --crop x,y,w,h         crop used by --sweep (default 512x512)\n"
        "  --badge <file>         write the AI disclosure badge PNG and exit\n"
        "  --badge-size <px> --badge-text <s> --badge-opacity <f>\n");
}

// Applies one sweep value to the settings block. Returns false for an unknown
// parameter name so the caller can report it rather than silently doing nothing.
bool ApplySweepValue(dlssnr::Settings* s, const std::string& name, float v) {
    if (name == "intensity")             s->intensity = v;
    else if (name == "style")            s->style = int(v);
    else if (name == "preset")           s->render_preset = int(v);
    else if (name == "skin")             s->skin_structure = v;
    else if (name == "local-structure")  s->local_structure = v;
    else if (name == "local-tone")       s->local_tone = v;
    else if (name == "global-tone")      s->global_tone = v;
    else if (name == "auto-mask")        s->use_auto_mask = int(v);
    else if (name == "ui-correction")    s->ui_correction = int(v);
    else if (name == "perf")             s->perf_quality = int(v);
    else return false;
    return true;
}

std::vector<float> ParseValueList(const std::string& spec) {
    std::vector<float> out;
    size_t start = 0;
    while (start <= spec.size()) {
        const size_t comma = spec.find(',', start);
        const std::string piece = spec.substr(start, comma == std::string::npos
                                                         ? std::string::npos
                                                         : comma - start);
        if (!piece.empty()) out.push_back(std::stof(piece));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

// Copies a crop out of `src` into `sheet` at (dx, dy).
void BlitCrop(const dlssnr::Image& src, int cx, int cy, int cw, int ch,
              dlssnr::Image* sheet, int dx, int dy) {
    for (int y = 0; y < ch; ++y) {
        const int sy = cy + y;
        if (sy < 0 || sy >= int(src.height)) continue;
        for (int x = 0; x < cw; ++x) {
            const int sx = cx + x;
            if (sx < 0 || sx >= int(src.width)) continue;
            const size_t s = (size_t(sy) * src.width + sx) * 4;
            const size_t d = (size_t(dy + y) * sheet->width + (dx + x)) * 4;
            if (d + 3 >= sheet->texels.size()) continue;
            for (int c = 0; c < 4; ++c) sheet->texels[d + c] = src.texels[s + c];
        }
    }
}

std::wstring Widen(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(n ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        Usage();
        return 2;
    }

    std::wstring input, output, snippet, core, depth_path, depth_preview, mask_path;
    dlssnr::Settings settings;
    dlssnr::DepthOptions depth_opts;
    bool srgb = true;
    bool estimate_depth = false;
    int bits = 16;
    std::wstring sequence_path, out_dir, video_path, mvec_dir;
    int fps = 24;
    std::wstring depth_seq_dir;
    bool estimate_motion = false, mvec_invert = false;
    bool mvec_ndc = false, auto_passes = true;
    float mvec_scale = 1.0f;
    dlssnr::MotionOptions motion_opts;
    bool list_gpus = false;
    int gpu = -1;  // adapter index; -1 keeps DXGI's high-performance pick
    std::wstring video_in;
    int video_from = 0, video_frames = 0;
    std::wstring mv_out;
    std::wstring badge_out;
    dlssnr::BadgeOptions badge;
    std::string sweep_name, sweep_values;
    int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;

    auto need = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "error: %s needs a value\n", flag);
            std::exit(2);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { Usage(); return 0; }
        else if (a == "-o" || a == "--output") output = Widen(need(i, "-o"));
        else if (a == "--snippet") snippet = Widen(need(i, "--snippet"));
        else if (a == "--core") core = Widen(need(i, "--core"));
        else if (a == "--depth") depth_path = Widen(need(i, "--depth"));
        else if (a == "--estimate-depth") estimate_depth = true;
        else if (a == "--mask") mask_path = Widen(need(i, "--mask"));
        else if (a == "--depth-out") depth_preview = Widen(need(i, "--depth-out"));
        else if (a == "--depth-res") depth_opts.resolution = std::atoi(need(i, "--depth-res"));
        else if (a == "--depth-gamma") depth_opts.gamma = std::stof(need(i, "--depth-gamma"));
        else if (a == "--depth-no-invert") depth_opts.invert = false;
        else if (a == "--video-in") video_in = Widen(need(i, "--video-in"));
        else if (a == "--mv-out") mv_out = Widen(need(i, "--mv-out"));
        else if (a == "--from") video_from = std::atoi(need(i, "--from"));
        else if (a == "--frames") video_frames = std::atoi(need(i, "--frames"));
        else if (a == "--list-gpus") list_gpus = true;
        else if (a == "--gpu") gpu = std::atoi(need(i, "--gpu"));
        else if (a == "--badge") badge_out = Widen(need(i, "--badge"));
        else if (a == "--badge-size") badge.diameter = std::atoi(need(i, "--badge-size"));
        else if (a == "--badge-text") badge.text = Widen(need(i, "--badge-text"));
        else if (a == "--badge-opacity") badge.opacity = std::stof(need(i, "--badge-opacity"));
        else if (a == "--sequence") sequence_path = Widen(need(i, "--sequence"));
        else if (a == "--out-dir") out_dir = Widen(need(i, "--out-dir"));
        else if (a == "--video") video_path = Widen(need(i, "--video"));
        else if (a == "--fps") fps = std::atoi(need(i, "--fps"));
        else if (a == "--estimate-motion") estimate_motion = true;
        else if (a == "--motion-res") motion_opts.resolution = std::atoi(need(i, "--motion-res"));
        else if (a == "--motion-scale") motion_opts.scale = std::stof(need(i, "--motion-scale"));
        else if (a == "--mvec") mvec_dir = Widen(need(i, "--mvec"));
        else if (a == "--mvec-scale") mvec_scale = std::stof(need(i, "--mvec-scale"));
        else if (a == "--mvec-invert") mvec_invert = true;
        else if (a == "--mvec-ndc") mvec_ndc = true;
        else if (a == "--depth-seq") depth_seq_dir = Widen(need(i, "--depth-seq"));
        else if (a == "--no-auto-passes") auto_passes = false;
        else if (a == "--sweep") sweep_name = need(i, "--sweep");
        else if (a == "--sweep-values") sweep_values = need(i, "--sweep-values");
        else if (a == "--crop") {
            const auto v = ParseValueList(need(i, "--crop"));
            if (v.size() == 4) { crop_x=int(v[0]); crop_y=int(v[1]); crop_w=int(v[2]); crop_h=int(v[3]); }
        }
        else if (a == "--depth-constant") settings.constant_depth = std::stof(need(i, "--depth-constant"));
        else if (a == "--depth-inverted") settings.depth_inverted = true;
        else if (a == "--linear") srgb = false;
        else if (a == "--bits") bits = std::atoi(need(i, "--bits"));
        else if (a == "--width") settings.output_width = unsigned(std::atoi(need(i, "--width")));
        else if (a == "--height") settings.output_height = unsigned(std::atoi(need(i, "--height")));
        else if (a == "--iterations") settings.iterations = std::atoi(need(i, "--iterations"));
        else if (a == "--intensity") settings.intensity = std::stof(need(i, "--intensity"));
        else if (a == "--style") settings.style = std::atoi(need(i, "--style"));
        else if (a == "--preset") settings.render_preset = std::atoi(need(i, "--preset"));
        else if (a == "--skin") settings.skin_structure = std::stof(need(i, "--skin"));
        else if (a == "--local-structure") settings.local_structure = std::stof(need(i, "--local-structure"));
        else if (a == "--local-tone") settings.local_tone = std::stof(need(i, "--local-tone"));
        else if (a == "--global-tone") settings.global_tone = std::stof(need(i, "--global-tone"));
        else if (a == "--backbuffer") settings.use_backbuffer = std::atoi(need(i, "--backbuffer"));
        else if (a == "--distortion") settings.use_distortion = std::atoi(need(i, "--distortion"));
        else if (a == "--auto-mask") settings.use_auto_mask = std::atoi(need(i, "--auto-mask"));
        else if (a == "--ui-correction") settings.ui_correction = std::atoi(need(i, "--ui-correction"));
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", a.c_str());
            return 2;
        } else if (input.empty()) {
            input = Widen(argv[i]);
        } else {
            std::fprintf(stderr, "error: unexpected argument %s\n", a.c_str());
            return 2;
        }
    }

    if (list_gpus) {
        const auto adapters = dlssnr::ListAdapters();
        std::wprintf(L"%zu adapter(s), DXGI high-performance order:\n", adapters.size());
        for (size_t i = 0; i < adapters.size(); ++i) {
            std::wprintf(L"  [%zu] %s  %lluGB%s\n", i, adapters[i].name.c_str(),
                         (unsigned long long)(adapters[i].dedicated_vram / (1024ull*1024*1024)),
                         adapters[i].software ? L"  (software)" : L"");
        }
        return 0;
    }

    // Renders the AI disclosure badge on its own, for previewing it or for
    // handing the PNG to an external encode.
    if (!badge_out.empty()) {
        std::string badge_error;
        if (!dlssnr::BuildBadgePng(badge, badge_out, &badge_error)) {
            std::fprintf(stderr, "error: %s\n", badge_error.c_str());
            return 1;
        }
        std::wprintf(L"badge %dpx -> %s\n", badge.diameter, badge_out.c_str());
        return 0;
    }

    if (sequence_path.empty() && video_in.empty() &&
        (input.empty() || output.empty())) {
        std::fprintf(stderr, "error: an input image and -o <output.png> are required\n");
        return 2;
    }
    if (bits != 8 && bits != 16) {
        std::fprintf(stderr, "error: --bits must be 8 or 16\n");
        return 2;
    }
    if (settings.output_width || settings.output_height) settings.upscaling = true;

    if (snippet.empty()) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        snippet = (std::filesystem::path(exe).parent_path() / L"nvngx_dlssnr.dll").wstring();
    }

    std::string err;

    // Shared by the streaming and the sequence paths below.
    using clock = std::chrono::steady_clock;

    // ---- streaming video mode ---------------------------------------------
    // Frames go decoder -> NR -> encoder through pipes. The sequence path below
    // writes every frame out as a PNG and reads it back, which at 4K cost more
    // than the neural pass and the optical flow together, and ~3 MB of disk a
    // frame on top.
    if (!video_in.empty()) {
        if (output.empty()) {
            std::fprintf(stderr, "error: --video-in needs -o <output.mp4>\n");
            return 2;
        }
        dlssnr::VideoInfo vinfo;
        if (!dlssnr::ProbeVideo(video_in, &vinfo, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::printf("video      %dx%d  %.3f fps  %d frames%s\n", vinfo.width,
                    vinfo.height, vinfo.fps, vinfo.frames,
                    vinfo.has_audio ? "  audio" : "");

        dlssnr::Runtime runtime;
        if (!runtime.Initialize(snippet, core, &err, gpu)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }

        dlssnr::MotionEstimator flow;
        if (estimate_motion) {
            wchar_t exe_path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            const auto model =
                std::filesystem::path(exe_path).parent_path() / L"raft.onnx";
            if (!flow.Load(model.wstring(), &err)) {
                std::fprintf(stderr, "warning: flow model: %s\n", err.c_str());
            }
            else {
                std::printf("flow       RAFT on %s\n", flow.Provider().c_str());
            }
        }

        dlssnr::EncodeOptions enc;
        enc.fps = int(vinfo.fps + 0.5);
        enc.crf = 16;
        std::wstring audio_tmp;
        if (vinfo.has_audio) {
            audio_tmp = (std::filesystem::temp_directory_path() /
                         L"dlssnr_stream_audio.m4a").wstring();
            if (dlssnr::ExtractAudio(video_in, audio_tmp, &err)) enc.audio = audio_tmp;
        }

        dlssnr::VideoReader reader;
        dlssnr::VideoWriter writer;
        const int planned_frames =
            video_frames > 0 ? video_frames : vinfo.frames - video_from;
        const double from_seconds =
            vinfo.fps > 0.0 ? video_from / vinfo.fps : 0.0;
        if (!reader.Open(video_in, vinfo.width, vinfo.height, from_seconds,
                         video_frames, &err) ||
            !writer.Open(output, vinfo.width, vinfo.height, vinfo.fps, enc, planned_frames,
                         &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }

        // Two buffers, alternating. Optical flow needs the previous frame, and
        // copying a 4K image every frame to keep one would give back a good
        // part of what streaming just saved.
        dlssnr::Image buffers[2];
        dlssnr::Image processed;
        dlssnr::MotionField motion;
        if (settings.iterations <= 0) settings.iterations = 1;

        auto ms_since = [](clock::time_point t) {
            return std::chrono::duration<double, std::milli>(clock::now() - t).count();
        };
        const auto stream_start = clock::now();
        double t_flow = 0.0, t_nr = 0.0;
        int index = 0, cur = 0;
        while (reader.Read(&buffers[cur], &err)) {
            const dlssnr::Image& frame = buffers[cur];
            const dlssnr::Image& previous = buffers[1 - cur];
            settings.reset_history = (index == 0);

            bool have_motion = false;
            if (index > 0 && flow.Loaded() && previous.Valid()) {
                const auto t0 = clock::now();
                have_motion =
                    flow.Estimate(frame, previous, motion_opts, &motion, &err);
                t_flow += ms_since(t0);
            }

            // Flow visualisation for the first frame that has motion: hue is
            // direction, brightness is how far the pixel moved.
            if (!mv_out.empty() && have_motion) {
                const auto vis = dlssnr::MotionEstimator::Visualise(motion);
                std::string vis_error;
                if (dlssnr::SaveImage(mv_out, vis, 8, true, &vis_error)) {
                    std::wprintf(L"flow vis   %s\n", mv_out.c_str());
                }
                mv_out.clear();
            }

            const auto t1 = clock::now();
            dlssnr::Report rep;
            if (!runtime.Process(frame, nullptr, nullptr,
                                 have_motion ? &motion : nullptr, settings,
                                 &processed, &rep, &err)) {
                std::fprintf(stderr, "frame %d: %s\n", index, err.c_str());
                break;
            }
            t_nr += ms_since(t1);

            if (!writer.Write(processed, &err)) {
                std::fprintf(stderr, "frame %d: %s\n", index, err.c_str());
                break;
            }
            cur ^= 1;
            ++index;
            if (index % 25 == 0 || index == vinfo.frames) {
                std::printf("  %d/%d  %.0f ms/frame\n", index, vinfo.frames,
                            ms_since(stream_start) / index);
                std::fflush(stdout);
            }
        }

        std::string close_error;
        const bool closed = writer.Close(&close_error);
        reader.Close();
        if (!audio_tmp.empty()) DeleteFileW(audio_tmp.c_str());

        const double wall = ms_since(stream_start);
        const double n = index ? double(index) : 1.0;
        std::printf("\n%d frames, %.1f s wall (%.0f ms/frame)\n", index,
                    wall / 1000.0, wall / n);
        std::printf("  flow   %6.0f ms/frame  %4.1f%%\n", t_flow / n,
                    100.0 * t_flow / wall);
        std::printf("  nr     %6.0f ms/frame  %4.1f%%\n", t_nr / n,
                    100.0 * t_nr / wall);
        std::printf("  io     %6.0f ms/frame  %4.1f%%\n",
                    (wall - t_flow - t_nr) / n,
                    100.0 * (wall - t_flow - t_nr) / wall);
        if (!closed) {
            std::fprintf(stderr, "error: %s\n", close_error.c_str());
            return 1;
        }
        std::wprintf(L"wrote      %s\n", output.c_str());
        return index > 0 ? 0 : 1;
    }

    // ---- sequence mode -----------------------------------------------------
    // This is the configuration NR was built for: consecutive frames, history
    // carried across them, reset only on the first. Every still-image result we
    // have is effectively a degenerate case of this.
    if (!sequence_path.empty()) {
        // Point at a render folder and the passes sort themselves out: MRQ
        // writes each AOV either into its own subfolder or with the pass name
        // in the filename, and both layouts are recognised. A plain folder of
        // colour frames still works -- it just yields beauty and nothing else.
        dlssnr::Sequence seq;
        dlssnr::PassSet passes;
        if (auto_passes && dlssnr::DetectPasses(sequence_path, &passes, &err)) {
            seq = passes.beauty;
            std::printf("passes     %s\n", dlssnr::DescribePasses(passes).c_str());
        } else if (!dlssnr::DetectSequence(sequence_path, &seq, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        std::wprintf(L"sequence   %s%s  frames %d..%d (%zu)\n", seq.prefix.c_str(),
                     seq.extension.c_str(), seq.first, seq.last, seq.Count());

        if (out_dir.empty()) out_dir = (std::filesystem::path(seq.directory) / L"nr_out").wstring();
        std::error_code fec;
        std::filesystem::create_directories(out_dir, fec);
        std::wprintf(L"output     %s\n", out_dir.c_str());

        dlssnr::Runtime runtime;
        if (!runtime.Initialize(snippet, core, &err, gpu)) {
            std::fprintf(stderr, "error: runtime init: %s\n", err.c_str());
            return 1;
        }
        std::wprintf(L"adapter    %s\n\n", runtime.AdapterName().c_str());

        dlssnr::Settings seq_settings = settings;
        if (seq_settings.iterations <= 0) seq_settings.iterations = 1;

        // --estimate-depth was accepted here and quietly dropped: the depth
        // path lived only in the still-image branch below, which a sequence
        // run returns before ever reaching.
        dlssnr::DepthEstimator seq_depth_estimator;
        if (estimate_depth && depth_seq_dir.empty() && !passes.HasDepth()) {
            wchar_t exe_path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            const auto model = std::filesystem::path(exe_path).parent_path() /
                               L"depth-anything-v2-small.onnx";
            if (!seq_depth_estimator.Load(model.wstring(), &err)) {
                std::fprintf(stderr, "warning: depth model: %s (continuing flat)\n",
                             err.c_str());
            }
            std::fprintf(stderr,
                "note: supplying depth produces bit-identical output on this\n"
                "      snippet, and costs about a second a frame to do it.\n");
        }

        dlssnr::MotionEstimator flow;
        if (estimate_motion) {
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            const auto model = std::filesystem::path(exe).parent_path() / L"raft.onnx";
            if (!flow.Load(model.wstring(), &err)) {
                std::fprintf(stderr, "error: %s\n", err.c_str());
                return 1;
            }
            std::printf("motion     optical flow (RAFT) at %d px\n", motion_opts.resolution);
        }
        // The remaining cases are reported below, once auto-detection has had
        // its say -- announcing "none" here would contradict it.

        // An explicit --mvec / --depth-seq always wins; otherwise fall back to
        // whatever the pass auto-detection found next to the beauty frames.
        dlssnr::Sequence mvec_seq;
        if (!mvec_dir.empty()) {
            if (!dlssnr::DetectSequence(mvec_dir, &mvec_seq, &err)) {
                std::fprintf(stderr, "error: velocity pass: %s\n", err.c_str());
                return 1;
            }
        } else if (!estimate_motion && passes.HasVelocity()) {
            mvec_seq = passes.velocity;
        }

        dlssnr::Sequence depth_seq;
        if (!depth_seq_dir.empty()) {
            if (!dlssnr::DetectSequence(depth_seq_dir, &depth_seq, &err)) {
                std::fprintf(stderr, "error: depth pass: %s\n", err.c_str());
                return 1;
            }
        } else if (passes.HasDepth()) {
            depth_seq = passes.depth;
            std::printf("depth      pass (auto-detected)\n");
        }

        if (!estimate_motion) {
            std::printf("motion     %s\n",
                        mvec_seq.frames.empty() ? "none (zero vectors)"
                        : mvec_dir.empty()      ? "velocity pass (auto-detected)"
                                                : "velocity pass from disk");
        }
        if (depth_seq.frames.empty()) std::printf("depth      flat plane\n");
        std::printf("\n");

        dlssnr::Image previous;
        dlssnr::MotionField motion;
        double total_ms = 0.0;
        int written = 0;
        auto ms_since = [](clock::time_point t) {
            return std::chrono::duration<double, std::milli>(clock::now() - t).count();
        };
        double t_load = 0, t_gpu = 0, t_save = 0;
        const auto wall_start = clock::now();

        // Decode the next frame and encode the previous one on other threads so
        // both hide behind the GPU work instead of adding to it. The evaluation
        // is the only genuinely serial part of this loop.
        auto decode = [&](size_t index) {
            auto img = std::make_shared<dlssnr::Image>();
            std::string e;
            if (!dlssnr::LoadImageFile(seq.frames[index], img.get(), &e)) img->texels.clear();
            return img;
        };
        std::future<std::shared_ptr<dlssnr::Image>> next_frame;
        std::vector<std::future<void>> pending_saves;

        for (size_t i = 0; i < seq.frames.size(); ++i) {
            auto t0 = clock::now();
            std::shared_ptr<dlssnr::Image> frame_ptr =
                next_frame.valid() ? next_frame.get() : decode(i);
            if (i + 1 < seq.frames.size()) {
                next_frame = std::async(std::launch::async, decode, i + 1);
            }
            if (!frame_ptr->Valid()) {
                std::fprintf(stderr, "  frame %zu: could not decode\n", i);
                continue;
            }
            dlssnr::Image& frame = *frame_ptr;
            t_load += ms_since(t0);
            // The whole point: clear history once, then accumulate.
            seq_settings.reset_history = (i == 0);

            // Motion for frame i describes how pixels moved since frame i-1, so
            // there is none for the first frame by definition.
            dlssnr::DepthImage frame_depth;
            bool have_frame_depth = false;
            if (!depth_seq.frames.empty() && i < depth_seq.frames.size()) {
                have_frame_depth =
                    dlssnr::LoadDepth(depth_seq.frames[i], &frame_depth, &err);
                if (!have_frame_depth) std::fprintf(stderr, "  depth: %s\n", err.c_str());
            } else if (seq_depth_estimator.Loaded()) {
                have_frame_depth =
                    seq_depth_estimator.Estimate(frame, depth_opts, &frame_depth, &err);
                if (!have_frame_depth) std::fprintf(stderr, "  depth: %s\n", err.c_str());
            }

            // NR wants pixels. A normalised-screen-space pass therefore scales
            // by the render dimensions, one factor per axis.
            const float sx = mvec_ndc ? float(frame.width) * mvec_scale : mvec_scale;
            const float sy = mvec_ndc ? float(frame.height) * mvec_scale : mvec_scale;

            bool have_motion = false;
            if (i > 0) {
                if (estimate_motion && previous.Valid()) {
                    have_motion = flow.Estimate(frame, previous, motion_opts, &motion, &err);
                    if (!have_motion) std::fprintf(stderr, "  flow: %s\n", err.c_str());
                } else if (!mvec_seq.frames.empty() && i < mvec_seq.frames.size()) {
                    have_motion = dlssnr::LoadMotionFile(mvec_seq.frames[i], sx, sy,
                                                         mvec_invert, &motion, &err);
                    if (!have_motion) std::fprintf(stderr, "  velocity: %s\n", err.c_str());
                }
            }

            dlssnr::Image processed;
            dlssnr::Report rep;
            if (!runtime.Process(frame, have_frame_depth ? &frame_depth : nullptr,
                                 nullptr, have_motion ? &motion : nullptr,
                                 seq_settings, &processed, &rep, &err)) {
                std::fprintf(stderr, "  frame %zu: %s\n", i, err.c_str());
                continue;
            }
            double ms = 0.0;
            for (const auto& e : rep.evaluations) ms += e.gpu_wait_ms;
            total_ms += ms;
            t_gpu += ms;
            t0 = clock::now();

            wchar_t name[64];
            swprintf_s(name, L"%s%0*d.png", L"frame.", seq.digits ? seq.digits : 4,
                       seq.first + int(i));
            const auto out_path = (std::filesystem::path(out_dir) / name).wstring();
            // Hand the encode to a worker. Two in flight is enough to cover one
            // GPU evaluation without letting memory grow without bound.
            auto owned = std::make_shared<dlssnr::Image>(std::move(processed));
            while (pending_saves.size() >= 2) {
                pending_saves.front().get();
                pending_saves.erase(pending_saves.begin());
            }
            pending_saves.push_back(std::async(std::launch::async, [out_path, owned, bits] {
                std::string e;
                dlssnr::SaveImage(out_path, *owned, bits, true, &e);
            }));
            t_save += ms_since(t0);
            ++written;
            const auto& last = rep.evaluations.back();
            float peak = 0.0f, mean = 0.0f;
            if (have_motion) dlssnr::MotionEstimator::Statistics(motion, &peak, &mean);
            std::printf("  [%3zu/%3zu] %6.0f ms  diff %.4f  mv max %6.1f mean %5.2f px%s\n",
                        i + 1, seq.frames.size(), ms,
                        last.mean_absolute_difference_from_input, peak, mean,
                        seq_settings.reset_history ? "  (reset)" : "");

            // Keep this frame as the reference for the next frame's flow.
            previous = std::move(frame);
        }
        for (auto& f : pending_saves) f.get();

        const double wall = ms_since(wall_start);
        const double n = written ? double(written) : 1.0;
        std::printf("\n%d/%zu frames, %.1f s wall (%.0f ms/frame)\n", written,
                    seq.frames.size(), wall / 1000.0, wall / n);
        std::printf("  load   %6.0f ms/frame  %4.1f%%\n", t_load / n, 100.0 * t_load / wall);
        std::printf("  gpu    %6.0f ms/frame  %4.1f%%\n", t_gpu / n, 100.0 * t_gpu / wall);
        std::printf("  save   %6.0f ms/frame  %4.1f%%\n", t_save / n, 100.0 * t_save / wall);
        std::printf("  other  %6.0f ms/frame  %4.1f%%\n",
                    (wall - t_load - t_gpu - t_save) / n,
                    100.0 * (wall - t_load - t_gpu - t_save) / wall);

        if (!video_path.empty()) {
            const auto pattern =
                (std::filesystem::path(out_dir) / L"frame.%04d.png").wstring();
            const auto cmd =
                dlssnr::BuildEncodeCommand(pattern, seq.first, video_path, fps, 16);
            if (cmd.empty()) {
                std::fprintf(stderr, "warning: ffmpeg not found on PATH; frames kept\n");
            } else {
                std::wprintf(L"encoding   %s\n", video_path.c_str());
                if (_wsystem(cmd.c_str()) == 0) {
                    std::wprintf(L"wrote      %s\n", video_path.c_str());
                } else {
                    std::fprintf(stderr, "warning: ffmpeg failed; frames kept\n");
                }
            }
        }
        return written > 0 ? 0 : 1;
    }

    dlssnr::Image src;
    if (!dlssnr::LoadImageFile(input, &src, &err)) {
        std::fprintf(stderr, "error: loading input: %s\n", err.c_str());
        return 1;
    }
    std::printf("input      %ux%u  (linear light via scRGB)\n", src.width, src.height);

    dlssnr::DepthImage depth;
    bool have_depth = !depth_path.empty();
    if (have_depth && !dlssnr::LoadDepth(depth_path, &depth, &err)) {
        std::fprintf(stderr, "error: loading depth: %s\n", err.c_str());
        return 1;
    }
    if (!have_depth && estimate_depth) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const auto model =
            std::filesystem::path(exe).parent_path() / L"depth-anything-v2-small.onnx";
        dlssnr::DepthEstimator estimator;
        if (!estimator.Load(model.wstring(), &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        if (!estimator.Estimate(src, depth_opts, &depth, &err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        have_depth = true;
        if (!depth_preview.empty()) {
            const auto vis = dlssnr::DepthEstimator::Visualise(depth);
            if (!dlssnr::SaveImage(depth_preview, vis, 16, true, &err)) {
                std::fprintf(stderr, "warning: could not write depth preview: %s\n", err.c_str());
            } else {
                std::wprintf(L"depth vis  %s\n", depth_preview.c_str());
            }
        }
    }
    std::printf("depth      %s\n",
                have_depth ? (estimate_depth && depth_path.empty() ? "estimated (Depth Anything V2)"
                                                                  : "from file")
                           : "flat plane (no depth map supplied)");

    dlssnr::DepthImage control_mask;
    bool have_mask = false;
    if (!mask_path.empty()) {
        if (!dlssnr::LoadDepth(mask_path, &control_mask, &err)) {
            std::fprintf(stderr, "error: loading control mask: %s\n", err.c_str());
            return 1;
        }
        have_mask = true;
        std::printf("mask       DLSSNR.ControlMask from file\n");
    }

    dlssnr::Runtime runtime;
    if (!runtime.Initialize(snippet, core, &err, gpu)) {
        std::fprintf(stderr, "error: runtime init: %s\n", err.c_str());
        return 1;
    }
    std::wprintf(L"adapter    %s\n", runtime.AdapterName().c_str());

    // ---- sweep mode: render one parameter across a list of values and tile
    // 1:1 crops into a contact sheet. Downscaled full frames would hide exactly
    // the micro-detail we are trying to judge.
    if (!sweep_name.empty()) {
        const auto values = ParseValueList(sweep_values);
        if (values.empty()) {
            std::fprintf(stderr, "error: --sweep needs --sweep-values\n");
            return 2;
        }
        int cw = crop_w > 0 ? crop_w : 512;
        int ch = crop_h > 0 ? crop_h : 512;
        cw = (std::min)(cw, int(src.width));
        ch = (std::min)(ch, int(src.height));
        const int cx = crop_w > 0 ? crop_x : int(src.width) / 2 - cw / 2;
        const int cy = crop_h > 0 ? crop_y : int(src.height) / 3 - ch / 2;

        const int cells = int(values.size()) + 1;  // +1 for the untouched original
        const int cols = int(std::ceil(std::sqrt(double(cells))));
        const int rows = (cells + cols - 1) / cols;
        const int gap = 8;

        dlssnr::Image sheet;
        sheet.width = uint32_t(cols * cw + (cols + 1) * gap);
        sheet.height = uint32_t(rows * ch + (rows + 1) * gap);
        sheet.texels.assign(size_t(sheet.width) * sheet.height * 4, dlssnr::FloatToHalf(0.02f));

        std::printf("\nsweep '%s' over %zu values, %dx%d crop at (%d,%d)\n",
                    sweep_name.c_str(), values.size(), cw, ch, cx, cy);
        std::printf("cell 0 = original, then values in order\n\n");

        BlitCrop(src, cx, cy, cw, ch, &sheet, gap, gap);

        for (size_t i = 0; i < values.size(); ++i) {
            dlssnr::Settings s = settings;
            if (!ApplySweepValue(&s, sweep_name, values[i])) {
                std::fprintf(stderr, "error: unknown sweep parameter '%s'\n", sweep_name.c_str());
                return 2;
            }
            dlssnr::Image cell;
            dlssnr::Report cell_report;
            if (!runtime.Process(src, have_depth ? &depth : nullptr,
                                 have_mask ? &control_mask : nullptr, nullptr, s, &cell, &cell_report, &err)) {
                std::fprintf(stderr, "  %s=%g -> FAILED: %s\n", sweep_name.c_str(), values[i],
                             err.c_str());
                continue;
            }
            const auto& last = cell_report.evaluations.back();
            std::printf("  %s=%-6g  mean|rgb| %.4f  diff %.4f\n", sweep_name.c_str(), values[i],
                        last.mean_absolute_rgb, last.mean_absolute_difference_from_input);
            const int index = int(i) + 1;
            const int col = index % cols, row = index / cols;
            BlitCrop(cell, cx, cy, cw, ch, &sheet, gap + col * (cw + gap),
                     gap + row * (ch + gap));
        }

        if (!dlssnr::SaveImage(output, sheet, bits, true, &err)) {
            std::fprintf(stderr, "error: saving sheet: %s\n", err.c_str());
            return 1;
        }
        std::wprintf(L"\nwrote      %s (%ux%u contact sheet)\n", output.c_str(), sheet.width,
                     sheet.height);
        return 0;
    }

    dlssnr::Image result;
    dlssnr::Report report;
    if (!runtime.Process(src, have_depth ? &depth : nullptr,
                        have_mask ? &control_mask : nullptr, nullptr, settings, &result, &report, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    std::printf("feature    NVSDK_NGX_Feature_Reserved%d\n", report.feature_id);
    std::printf("\n  pass   gpu wait   mean|rgb|   diff vs input   finite  non-black  varying\n");
    for (const auto& s : report.evaluations) {
        std::printf("  %4d  %7.0f ms   %9.4f   %13.4f   %6s  %9s  %7s\n", s.index, s.gpu_wait_ms,
                    s.mean_absolute_rgb, s.mean_absolute_difference_from_input,
                    s.finite ? "yes" : "NO", s.non_black ? "yes" : "NO",
                    s.spatially_varying ? "yes" : "NO");
    }

    const auto& last = report.evaluations.back();
    if (!last.finite || !last.non_black || !last.spatially_varying) {
        std::fprintf(stderr,
                     "\nerror: the final evaluation failed content validation; not saving.\n");
        return 1;
    }

    if (!dlssnr::SaveImage(output, result, bits, srgb, &err)) {
        std::fprintf(stderr, "error: saving output: %s\n", err.c_str());
        return 1;
    }
    std::wprintf(L"\nwrote      %s (%ux%u, %d-bit PNG)\n", output.c_str(), result.width,
                 result.height, bits);
    return 0;
}
