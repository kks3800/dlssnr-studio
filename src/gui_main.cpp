// dlssnr-studio -- interactive front end for the DLSS 5 Neural Rendering runtime.
//
// The NR runtime lives on a worker thread with its own D3D12 device; the UI has
// its own. They never share resources -- results come back as CPU images, which
// costs a few megabytes of copy against a ~900 ms evaluation and keeps the two
// devices completely independent.

// Must precede every include: windows.h drags in <math.h>, and M_PI only
// appears when this is defined first.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

#include "depth_estimator.h"
#include "sequence.h"
#include "image_io.h"
#include "nr_runtime.h"
#include "watermark.h"
#include "video_stream.h"

using Microsoft::WRL::ComPtr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

constexpr int kFramesInFlight = 3;
constexpr int kSrvDescriptorCount = 64;

// --------------------------------------------------------------------------
// Theme. Near-neutral dark greys with a single warm accent, chosen so nothing
// in the chrome competes with the image being judged -- the whole point of the
// window is colour and detail assessment, so saturated UI would be actively
// harmful.
// --------------------------------------------------------------------------
namespace col {
constexpr ImVec4 kBg      {0.086f, 0.090f, 0.098f, 1.00f};
constexpr ImVec4 kPanel   {0.118f, 0.125f, 0.137f, 1.00f};
constexpr ImVec4 kSurface {0.157f, 0.165f, 0.180f, 1.00f};
constexpr ImVec4 kHover   {0.204f, 0.216f, 0.235f, 1.00f};
constexpr ImVec4 kActive  {0.251f, 0.267f, 0.290f, 1.00f};
constexpr ImVec4 kAccent  {0.945f, 0.678f, 0.259f, 1.00f};
constexpr ImVec4 kAccentDim{0.945f, 0.678f, 0.259f, 0.35f};
constexpr ImVec4 kText    {0.878f, 0.890f, 0.902f, 1.00f};
constexpr ImVec4 kTextDim {0.514f, 0.533f, 0.561f, 1.00f};
constexpr ImVec4 kGood    {0.400f, 0.800f, 0.510f, 1.00f};
constexpr ImVec4 kWarn    {0.949f, 0.611f, 0.290f, 1.00f};
constexpr ImVec4 kBad     {0.902f, 0.400f, 0.365f, 1.00f};
}  // namespace col

void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 8.0f;
    s.GrabRounding = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.TabRounding = 6.0f;

    s.WindowPadding = ImVec2(16, 16);
    s.FramePadding = ImVec2(11, 7);
    s.ItemSpacing = ImVec2(10, 9);
    s.ItemInnerSpacing = ImVec2(9, 6);
    s.CellPadding = ImVec2(8, 6);
    s.IndentSpacing = 18.0f;
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 12.0f;

    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding = ImVec2(0, 10);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]            = col::kBg;
    c[ImGuiCol_ChildBg]             = col::kPanel;
    c[ImGuiCol_PopupBg]             = col::kPanel;
    c[ImGuiCol_Border]              = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_Text]                = col::kText;
    c[ImGuiCol_TextDisabled]        = col::kTextDim;
    c[ImGuiCol_FrameBg]             = col::kSurface;
    c[ImGuiCol_FrameBgHovered]      = col::kHover;
    c[ImGuiCol_FrameBgActive]       = col::kActive;
    c[ImGuiCol_TitleBg]             = col::kPanel;
    c[ImGuiCol_TitleBgActive]       = col::kPanel;
    c[ImGuiCol_Header]              = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(1, 1, 1, 0.09f);
    c[ImGuiCol_HeaderActive]        = ImVec4(1, 1, 1, 0.13f);
    c[ImGuiCol_Button]              = col::kSurface;
    c[ImGuiCol_ButtonHovered]       = col::kHover;
    c[ImGuiCol_ButtonActive]        = col::kActive;
    c[ImGuiCol_CheckMark]           = col::kAccent;
    c[ImGuiCol_SliderGrab]          = col::kAccent;
    c[ImGuiCol_SliderGrabActive]    = ImVec4(1.0f, 0.78f, 0.38f, 1.0f);
    c[ImGuiCol_Separator]           = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_SeparatorHovered]    = col::kAccentDim;
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0, 0, 0, 0.16f);
    c[ImGuiCol_ScrollbarGrab]       = col::kSurface;
    c[ImGuiCol_ScrollbarGrabHovered]= col::kHover;
    c[ImGuiCol_ScrollbarGrabActive] = col::kActive;
    c[ImGuiCol_PlotHistogram]       = col::kAccent;
    c[ImGuiCol_PlotHistogramHovered]= ImVec4(1.0f, 0.78f, 0.38f, 1.0f);
    c[ImGuiCol_ResizeGrip]          = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_NavCursor]           = col::kAccentDim;
}

// A section heading with a little breathing room above it.
void Section(const char* label) {
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushStyleColor(ImGuiCol_Text, col::kAccent);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
}

// Right-aligned dim value next to a dim label -- for the stats readout.
// "45s", "1m 07s", "2h 14m" -- an ETA in raw seconds is unreadable once a
// 4K render is measured in hours.
void FormatDuration(double seconds, char* out, size_t size) {
    if (!(seconds > 0.0) || seconds > 359999.0) {
        sprintf_s(out, size, "--");
        return;
    }
    const int total = int(seconds + 0.5);
    const int h = total / 3600, m = (total % 3600) / 60, sec = total % 60;
    if (h > 0) {
        sprintf_s(out, size, "%dh %02dm", h, m);
    } else if (m > 0) {
        sprintf_s(out, size, "%dm %02ds", m, sec);
    } else {
        sprintf_s(out, size, "%ds", sec);
    }
}

void StatRow(const char* label, const char* value) {
    ImGui::TextColored(col::kTextDim, "%s", label);
    ImGui::SameLine();
    const float w = ImGui::GetContentRegionAvail().x;
    const float tw = ImGui::CalcTextSize(value).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w - tw);
    ImGui::TextUnformatted(value);
}

// Files dropped on the window, picked up by the main loop.
std::mutex g_drop_mutex;
std::vector<std::wstring> g_dropped;

// --------------------------------------------------------------------------
// SRV descriptor heap with a free list. ImGui needs one descriptor for its
// font atlas and calls back into us for it; our preview textures take the rest.
// --------------------------------------------------------------------------
struct SrvHeap {
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT increment = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start{};
    std::vector<UINT> free_list;

    bool Create(ID3D12Device* device) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = kSrvDescriptorCount;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) return false;
        increment = device->GetDescriptorHandleIncrementSize(desc.Type);
        cpu_start = heap->GetCPUDescriptorHandleForHeapStart();
        gpu_start = heap->GetGPUDescriptorHandleForHeapStart();
        free_list.reserve(kSrvDescriptorCount);
        for (int i = kSrvDescriptorCount; i-- > 0;) free_list.push_back(UINT(i));
        return true;
    }

    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
        const UINT index = free_list.back();
        free_list.pop_back();
        cpu->ptr = cpu_start.ptr + SIZE_T(index) * increment;
        gpu->ptr = gpu_start.ptr + UINT64(index) * increment;
    }

    void Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
        free_list.push_back(UINT((cpu.ptr - cpu_start.ptr) / increment));
    }
};

SrvHeap g_srv;

// --------------------------------------------------------------------------
// Device / swapchain
// --------------------------------------------------------------------------
struct FrameContext {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fence_value = 0;
};

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12GraphicsCommandList> g_cmd_list;
ComPtr<ID3D12DescriptorHeap> g_rtv_heap;
ComPtr<IDXGISwapChain3> g_swapchain;
ComPtr<ID3D12Fence> g_fence;
FrameContext g_frames[kFramesInFlight];
ComPtr<ID3D12Resource> g_backbuffers[kFramesInFlight];
D3D12_CPU_DESCRIPTOR_HANDLE g_rtv[kFramesInFlight]{};
HANDLE g_fence_event = nullptr;
UINT64 g_fence_last = 0;
UINT g_frame_index = 0;
HANDLE g_swapchain_waitable = nullptr;

float LinearToSrgb(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// --------------------------------------------------------------------------
// A displayable copy of a linear half-float image.
// --------------------------------------------------------------------------
struct PreviewTexture {
    ComPtr<ID3D12Resource> texture;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    uint32_t width = 0, height = 0;
    bool allocated = false;

    void Release() {
        if (allocated) {
            g_srv.Free(cpu, gpu);
            allocated = false;
        }
        texture.Reset();
        width = height = 0;
    }

    // Synchronous upload. Runs only when a new result arrives, so the stall is
    // irrelevant next to the evaluation that produced it.
    bool Upload(const dlssnr::Image& image) {
        if (!image.Valid()) return false;
        if (width != image.width || height != image.height) {
            Release();
            D3D12_HEAP_PROPERTIES props{};
            props.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = image.width;
            desc.Height = image.height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            if (FAILED(g_device->CreateCommittedResource(
                    &props, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&texture)))) {
                return false;
            }
            g_srv.Alloc(&cpu, &gpu);
            allocated = true;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = desc.Format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(texture.Get(), &srv, cpu);
            width = image.width;
            height = image.height;
        }

        const UINT row = width * 4;
        const UINT pitch = (row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                           ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        const UINT64 total = UINT64(pitch) * height;

        ComPtr<ID3D12Resource> upload;
        D3D12_HEAP_PROPERTIES up{};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ,
                                                     nullptr, IID_PPV_ARGS(&upload)))) {
            return false;
        }

        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED(upload->Map(0, &none, &mapped))) return false;
        for (uint32_t y = 0; y < height; ++y) {
            auto* dst = static_cast<uint8_t*>(mapped) + size_t(y) * pitch;
            const uint16_t* src = image.texels.data() + size_t(y) * width * 4;
            for (uint32_t x = 0; x < width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float v = LinearToSrgb(dlssnr::HalfToFloat(src[size_t(x) * 4 + c]));
                    dst[size_t(x) * 4 + c] = uint8_t(v * 255.0f + 0.5f);
                }
                dst[size_t(x) * 4 + 3] = 255;
            }
        }
        upload->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                    IID_PPV_ARGS(&list));
        g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = width;
        src.PlacedFootprint.Footprint.Height = height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        dst.pResource = texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        list->Close();

        ID3D12CommandList* lists[] = {list.Get()};
        g_queue->ExecuteCommandLists(1, lists);
        g_queue->Signal(fence.Get(), 1);
        if (fence->GetCompletedValue() < 1) {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fence->SetEventOnCompletion(1, ev);
            // Bounded, not INFINITE. This runs on the UI thread while the NR
            // runtime is hammering the same GPU from its own device, and a
            // blocked UI thread stops presenting entirely -- which looks like
            // a black window, not like a slow one.
            const DWORD waited = WaitForSingleObject(ev, 2000);
            CloseHandle(ev);
            if (waited != WAIT_OBJECT_0) return false;
        }
        return true;
    }
};

// --------------------------------------------------------------------------
// Worker: owns the NR runtime. The UI posts a desired settings state; the
// worker always renders the most recent one and drops anything superseded
// while it was busy, so dragging a slider does not queue up ten evaluations.
// --------------------------------------------------------------------------
class Engine {
public:
    void Start(std::wstring snippet, std::wstring core, int adapter = -1) {
        snippet_ = std::move(snippet);
        core_ = std::move(core);
        adapter_ = adapter;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = false;
            ready_ = false;
            fresh_ = false;
        }
        thread_ = std::thread([this] { Run(); });
    }

    // Switching GPU means a new D3D12 device, so the runtime is torn down and
    // rebuilt. The caller re-supplies the input afterwards.
    void Restart(int adapter) {
        std::wstring snippet = snippet_, core = core_;
        Stop();
        Start(std::move(snippet), std::move(core), adapter);
    }

    int Adapter() const { return adapter_; }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void SetInput(std::shared_ptr<dlssnr::Image> image,
                  std::shared_ptr<dlssnr::DepthImage> depth,
                  std::shared_ptr<dlssnr::DepthImage> mask) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            input_ = std::move(image);
            depth_ = std::move(depth);
            mask_ = std::move(mask);
            ++requested_;
        }
        cv_.notify_all();
    }

    void Request(const dlssnr::Settings& settings) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            settings_ = settings;
            ++requested_;
        }
        cv_.notify_all();
    }

    // Returns true once per completed evaluation. During a sequence run the
    // frame that produced the result comes back too, so the before/after
    // compare keeps showing the same moment instead of a frozen still.
    bool TakeResult(std::shared_ptr<dlssnr::Image>* out, dlssnr::Report* report,
                    std::shared_ptr<dlssnr::Image>* source = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fresh_) return false;
        fresh_ = false;
        *out = result_;
        *report = report_;
        if (source) *source = source_preview_;
        source_preview_.reset();
        return true;
    }

    // Queues a whole sequence. The worker resets NR's temporal history on the
    // first frame only, so history accumulates across the shot the way it does
    // in a game -- which is the entire reason to run a sequence rather than a
    // pile of independent stills.
    void RunSequence(dlssnr::PassSet passes, const dlssnr::Settings& settings,
                     std::wstring out_dir, std::wstring video,
                     const dlssnr::EncodeOptions& encode, int bits,
                     bool estimate_motion, int motion_res) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_passes_ = std::move(passes);
            pending_sequence_ = pending_passes_.beauty;
            seq_bits_ = bits;
            seq_settings_ = settings;
            seq_out_dir_ = std::move(out_dir);
            seq_video_ = std::move(video);
            seq_encode_ = encode;
            seq_estimate_motion_ = estimate_motion;
            seq_motion_res_ = motion_res;
            sequence_pending_ = true;
            seq_done_ = 0;
            seq_total_ = int(pending_sequence_.Count());
        }
        cv_.notify_all();
    }

    // Streams a video straight through: decoder -> NR -> encoder, no frames on
    // disk. The sequence path stays for renders that are already files.
    void RunVideoStream(std::wstring source, std::wstring output,
                        const dlssnr::VideoInfo& info,
                        const dlssnr::Settings& settings,
                        const dlssnr::EncodeOptions& encode, bool estimate_motion,
                        int start_frame, int frame_count) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stream_source_ = std::move(source);
            stream_output_ = std::move(output);
            stream_info_ = info;
            seq_settings_ = settings;
            seq_encode_ = encode;
            seq_estimate_motion_ = estimate_motion;
            stream_start_frame_ = (std::max)(0, start_frame);
            stream_frame_count_ =
                frame_count > 0 ? frame_count : info.frames - stream_start_frame_;
            stream_pending_ = true;
            seq_done_ = 0;
            seq_total_ = stream_frame_count_;
        }
        cv_.notify_all();
    }

    int SeqDone() const { return seq_done_.load(); }
    int SeqStartFrame() const { return stream_start_frame_; }
    int SeqTotal() const { return seq_total_.load(); }
    bool SeqRunning() const { return seq_running_.load(); }
    bool SeqEncoding() const { return seq_encoding_.load(); }
    void CancelSequence() { seq_cancel_ = true; }

    static double NowSeconds() {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    double SeqElapsed() const {
        const double started = seq_started_.load();
        return started > 0.0 ? NowSeconds() - started : 0.0;
    }

    // Smoothed, because per-frame cost wobbles with scene content and the
    // first frame carries the history reset.
    double SeqFrameMs() const { return seq_frame_ms_.load(); }

    // Straight elapsed-per-done extrapolation. Steadier than an instantaneous
    // rate over a long run, which is when an estimate is actually worth having.
    double SeqEta() const {
        const int done = seq_done_.load(), total = seq_total_.load();
        if (done <= 0 || total <= 0 || done >= total) return 0.0;
        return SeqElapsed() / done * double(total - done);
    }

    std::string Status() {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }
    bool Busy() const { return busy_.load(); }
    bool Ready() const { return ready_.load(); }

private:
    void SetStatus(std::string s) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = std::move(s);
    }

    void ProcessVideoStream(dlssnr::Runtime& runtime, const std::wstring& source,
                            const std::wstring& output,
                            const dlssnr::VideoInfo& info, dlssnr::Settings settings,
                            dlssnr::EncodeOptions encode) {
        seq_running_ = true;
        seq_cancel_ = false;
        seq_encoding_ = false;
        seq_started_ = NowSeconds();
        seq_frame_ms_ = 0.0;
        if (settings.iterations <= 0) settings.iterations = 1;

        dlssnr::MotionEstimator flow;
        if (seq_estimate_motion_) {
            wchar_t exe_path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            const auto model =
                std::filesystem::path(exe_path).parent_path() / L"raft.onnx";
            std::string load_error;
            if (!flow.Load(model.wstring(), &load_error)) {
                SetStatus("flow model: " + load_error + " (continuing without motion)");
            }
        }

        // Audio is lifted out once and muxed back by the encoder, so the neural
        // pass never sees it.
        std::wstring audio_path;
        if (info.has_audio) {
            SetStatus("extracting audio...");
            const auto tmp = (std::filesystem::temp_directory_path() /
                              L"dlssnr_studio_audio.m4a").wstring();
            std::string audio_error;
            if (dlssnr::ExtractAudio(source, tmp, &audio_error)) {
                audio_path = tmp;
                encode.audio = tmp;
            }
        }

        std::string err;
        dlssnr::VideoReader reader;
        dlssnr::VideoWriter writer;
        const double start_seconds =
            info.fps > 0.0 ? stream_start_frame_ / info.fps : 0.0;
        if (!reader.Open(source, info.width, info.height, start_seconds,
                         stream_frame_count_, &err)) {
            SetStatus("decoder: " + err);
            seq_running_ = false;
            return;
        }
        if (!writer.Open(output, info.width, info.height, info.fps, encode,
                         stream_frame_count_, &err)) {
            SetStatus("encoder: " + err);
            seq_running_ = false;
            return;
        }

        // Alternating buffers: flow needs the previous frame, and copying a 4K
        // image every frame would give back much of what streaming saves.
        dlssnr::Image buffers[2];
        dlssnr::MotionField motion;
        int index = 0, cur = 0;

        while (!seq_cancel_ && reader.Read(&buffers[cur], &err)) {
            const double frame_started = NowSeconds();
            const dlssnr::Image& frame = buffers[cur];
            const dlssnr::Image& previous = buffers[1 - cur];
            settings.reset_history = (index == 0);

            bool has_motion = false;
            if (index > 0 && flow.Loaded() && previous.Valid()) {
                dlssnr::MotionOptions mo;
                mo.resolution = seq_motion_res_ > 0 ? seq_motion_res_ : 512;
                has_motion = flow.Estimate(frame, previous, mo, &motion, &err);
            }

            auto processed = std::make_shared<dlssnr::Image>();
            dlssnr::Report rep;
            if (!runtime.Process(frame, nullptr, nullptr,
                                 has_motion ? &motion : nullptr, settings,
                                 processed.get(), &rep, &err)) {
                SetStatus("frame " + std::to_string(index) + ": " + err);
                break;
            }
            if (!writer.Write(*processed, &err)) {
                SetStatus("encoder: " + err);
                break;
            }

            ++index;
            seq_done_ = index;
            const double took = (NowSeconds() - frame_started) * 1000.0;
            const double previous_ms = seq_frame_ms_.load();
            seq_frame_ms_ =
                previous_ms > 0.0 ? previous_ms * 0.8 + took * 0.2 : took;

            {
                std::lock_guard<std::mutex> lock(mutex_);
                result_ = processed;
                source_preview_ = std::make_shared<dlssnr::Image>(frame);
                report_ = rep;
                fresh_ = true;
            }
            cur ^= 1;
        }

        seq_encoding_ = true;
        SetStatus("finishing the encode...");
        std::string close_error;
        const bool closed = writer.Close(&close_error);
        reader.Close();
        seq_encoding_ = false;
        if (!audio_path.empty()) DeleteFileW(audio_path.c_str());

        char summary[160];
        sprintf_s(summary, "video %s: %d/%d frames%s",
                  seq_cancel_ ? "cancelled" : (closed ? "done" : "failed"), index,
                  stream_frame_count_, closed ? "" : " (encoder error)");
        SetStatus(summary);
        seq_running_ = false;
    }

    void ProcessSequence(dlssnr::Runtime& runtime, const dlssnr::PassSet& passes,
                         dlssnr::Settings settings, const std::wstring& out_dir,
                         const std::wstring& video,
                         const dlssnr::EncodeOptions& encode) {
        const dlssnr::Sequence& seq = passes.beauty;
        seq_running_ = true;
        seq_cancel_ = false;
        seq_encoding_ = false;
        seq_started_ = NowSeconds();
        seq_frame_ms_ = 0.0;
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        if (settings.iterations <= 0) settings.iterations = 1;

        // Footage captured from a screen has no velocity pass, so without this
        // NR accumulates history with nothing to reproject it by. Measured on a
        // 48-frame clip, motion changes ~99% of pixels versus running without
        // it -- this is the difference between the GUI and the CLI, not a
        // rounding one.
        //
        // Depth is deliberately not estimated here. Supplying it -- estimated,
        // constant, or inverted -- produces bit-identical output on this
        // snippet, so DepthAnything would cost about a second a frame to change
        // nothing.
        dlssnr::MotionEstimator flow;
        if (seq_estimate_motion_ && !passes.HasVelocity()) {
            wchar_t exe_path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            const auto model =
                std::filesystem::path(exe_path).parent_path() / L"raft.onnx";
            std::string load_error;
            if (!flow.Load(model.wstring(), &load_error)) {
                SetStatus("flow model: " + load_error + " (continuing without motion)");
            }
        }
        dlssnr::Image previous;

        int written = 0;
        std::string err;
        for (size_t i = 0; i < seq.frames.size() && !seq_cancel_; ++i) {
            const double frame_started = NowSeconds();
            dlssnr::Image frame;
            if (!dlssnr::LoadImageFile(seq.frames[i], &frame, &err)) continue;
            settings.reset_history = (i == 0);

            // Engine-supplied guides, when the render provided them.
            dlssnr::DepthImage fdepth;
            bool has_depth = false;
            if (passes.HasDepth() && i < passes.depth.frames.size()) {
                has_depth = dlssnr::LoadDepth(passes.depth.frames[i], &fdepth, &err);
            }
            // Motion for frame i is how pixels moved since i-1, so the first
            // frame has none by definition.
            dlssnr::MotionField fmotion;
            bool has_motion = false;
            if (i > 0) {
                if (passes.HasVelocity() && i < passes.velocity.frames.size()) {
                    has_motion = dlssnr::LoadMotionFile(passes.velocity.frames[i], 1.0f,
                                                        1.0f, false, &fmotion, &err);
                } else if (flow.Loaded() && previous.Valid()) {
                    dlssnr::MotionOptions mo;
                    mo.resolution = seq_motion_res_ > 0 ? seq_motion_res_ : 512;
                    has_motion = flow.Estimate(frame, previous, mo, &fmotion, &err);
                }
            }

            auto processed = std::make_shared<dlssnr::Image>();
            dlssnr::Report rep;
            if (!runtime.Process(frame, has_depth ? &fdepth : nullptr, nullptr,
                                 has_motion ? &fmotion : nullptr, settings, processed.get(),
                                 &rep, &err)) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "sequence error: " + err;
                break;
            }
            wchar_t name[64];
            swprintf_s(name, L"frame.%0*d.png", seq.digits ? seq.digits : 4,
                       seq.first + int(i));
            dlssnr::SaveImage((std::filesystem::path(out_dir) / name).wstring(), *processed,
                              seq_bits_, true, &err);
            ++written;
            seq_done_ = int(i) + 1;

            const double took = (NowSeconds() - frame_started) * 1000.0;
            const double previous_ms = seq_frame_ms_.load();
            seq_frame_ms_ =
                previous_ms > 0.0 ? previous_ms * 0.8 + took * 0.2 : took;

            // Show the newest frame in the viewer as it goes, alongside the
            // source it came from so the split compare stays honest.
            if (flow.Loaded()) previous = frame;

            std::lock_guard<std::mutex> lock(mutex_);
            result_ = processed;
            source_preview_ = std::make_shared<dlssnr::Image>(std::move(frame));
            report_ = rep;
            fresh_ = true;
        }

        if (!video.empty() && written > 0 && !seq_cancel_) {
            seq_encoding_ = true;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "encoding video...";
            }
            const auto pattern = (std::filesystem::path(out_dir) / L"frame.%04d.png").wstring();
            const auto cmd =
                dlssnr::BuildVideoEncodeCommand(pattern, seq.first, video, encode);
            if (!cmd.empty()) dlssnr::RunCommandHidden(cmd);
            seq_encoding_ = false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            char buf[128];
            sprintf_s(buf, "sequence %s: %d/%zu frames", seq_cancel_ ? "cancelled" : "done",
                      written, seq.frames.size());
            status_ = buf;
        }
        seq_running_ = false;
    }

    void Run() {
        dlssnr::Runtime runtime;
        std::string err;
        SetStatus("initialising NR runtime...");
        if (!runtime.Initialize(snippet_, core_, &err, adapter_)) {
            SetStatus("init failed: " + err);
            return;
        }
        ready_ = true;
        SetStatus("ready");

        for (;;) {
            std::shared_ptr<dlssnr::Image> input;
            std::shared_ptr<dlssnr::DepthImage> depth;
            std::shared_ptr<dlssnr::DepthImage> mask;
            dlssnr::Settings settings;
            uint64_t serial = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock,
                         [this] {
                             return quit_ || sequence_pending_ ||
                                    stream_pending_ || requested_ != served_;
                         });
                if (quit_) return;
                if (stream_pending_) {
                    stream_pending_ = false;
                    const std::wstring src = stream_source_, out = stream_output_;
                    const dlssnr::VideoInfo info = stream_info_;
                    dlssnr::Settings vs = seq_settings_;
                    const dlssnr::EncodeOptions venc = seq_encode_;
                    lock.unlock();
                    ProcessVideoStream(runtime, src, out, info, vs, venc);
                    continue;
                }
                if (sequence_pending_) {
                    sequence_pending_ = false;
                    dlssnr::PassSet ps = pending_passes_;
                    dlssnr::Settings s = seq_settings_;
                    const std::wstring dir = seq_out_dir_, video = seq_video_;
                    const dlssnr::EncodeOptions encode = seq_encode_;
                    lock.unlock();
                    ProcessSequence(runtime, ps, s, dir, video, encode);
                    continue;
                }
                served_ = requested_;
                serial = served_;
                input = input_;
                depth = depth_;
                mask = mask_;
                settings = settings_;
            }
            if (!input || !input->Valid()) continue;

            busy_ = true;
            auto out = std::make_shared<dlssnr::Image>();
            dlssnr::Report report;
            const bool ok = runtime.Process(*input, depth ? depth.get() : nullptr,
                                            mask ? mask.get() : nullptr, nullptr, settings, out.get(),
                                            &report, &err);
            busy_ = false;

            std::lock_guard<std::mutex> lock(mutex_);
            if (!ok) {
                status_ = "error: " + err;
                continue;
            }
            // Drop the result if the user has already asked for something newer.
            if (serial != requested_) continue;
            result_ = out;
            report_ = report;
            fresh_ = true;
            double total = 0.0;
            for (const auto& e : report.evaluations) total += e.gpu_wait_ms;
            char buf[160];
            sprintf_s(buf, "%zu passes, %.0f ms total (%.0f ms/pass)",
                      report.evaluations.size(), total,
                      report.evaluations.empty() ? 0.0 : total / report.evaluations.size());
            status_ = buf;
        }
    }

    std::wstring snippet_, core_;
    int adapter_ = -1;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<dlssnr::Image> input_, result_;
    std::shared_ptr<dlssnr::DepthImage> depth_, mask_;
    dlssnr::Settings settings_;
    dlssnr::Report report_;
    std::string status_;
    uint64_t requested_ = 0, served_ = 0;
    bool quit_ = false, fresh_ = false;
    std::atomic<bool> busy_{false}, ready_{false};

    // sequence job
    dlssnr::Sequence pending_sequence_;
    dlssnr::PassSet pending_passes_;
    int seq_bits_ = 8;
    dlssnr::Settings seq_settings_;
    std::wstring seq_out_dir_, seq_video_;
    dlssnr::EncodeOptions seq_encode_;
    bool sequence_pending_ = false;
    std::shared_ptr<dlssnr::Image> source_preview_;
    std::atomic<int> seq_done_{0}, seq_total_{0};
    std::atomic<bool> seq_running_{false}, seq_cancel_{false};
    std::atomic<bool> seq_encoding_{false};
    std::atomic<double> seq_started_{0.0}, seq_frame_ms_{0.0};
    bool seq_estimate_motion_ = true;
    int seq_motion_res_ = 512;
    bool stream_pending_ = false;
    std::wstring stream_source_, stream_output_;
    dlssnr::VideoInfo stream_info_;
    int stream_start_frame_ = 0, stream_frame_count_ = 0;
};

Engine g_engine;

// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// Scrubber: pulls single frames out of a video so a reference shot can be
// picked before committing to a render. One worker, always serving the newest
// requested position -- dragging across a clip would otherwise queue one
// ffmpeg seek per pixel of travel and fall minutes behind the cursor.
// --------------------------------------------------------------------------
class Scrubber {
public:
    ~Scrubber() { Stop(); }

    void Start(std::wstring source, std::wstring cache_dir) {
        Stop();
        source_ = std::move(source);
        cache_ = std::move(cache_dir);
        quit_ = false;
        thread_ = std::thread([this] { Run(); });
    }

    void Stop() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    void Request(double seconds) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wanted_ = seconds;
            has_request_ = true;
        }
        cv_.notify_all();
    }

    bool Take(std::wstring* path) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fresh_) return false;
        fresh_ = false;
        *path = ready_;
        return true;
    }

    bool Busy() const { return busy_.load(); }

    std::string Error() {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

private:
    void Run() {
        int slot = 0;
        for (;;) {
            double seconds = 0.0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return quit_ || has_request_; });
                if (quit_) return;
                seconds = wanted_;
                has_request_ = false;
            }
            busy_ = true;
            // Alternate two files so the UI never opens the one being written.
            const std::wstring out =
                (std::filesystem::path(cache_) /
                 (slot ? L"scrub_a.png" : L"scrub_b.png")).wstring();
            slot ^= 1;
            std::string err;
            const bool ok = dlssnr::ExtractFrameAt(source_, seconds, out, &err);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ok) {
                    ready_ = out;
                    fresh_ = true;
                    error_.clear();
                } else {
                    error_ = err.empty() ? "seek failed" : err;
                }
            }
            busy_ = false;
        }
    }

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::wstring source_, cache_, ready_;
    std::string error_;
    double wanted_ = 0.0;
    bool has_request_ = false, fresh_ = false, quit_ = false;
    std::atomic<bool> busy_{false};
};

bool OpenFileDialog(HWND owner, const wchar_t* filter, std::wstring* out) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    *out = path;
    return true;
}

bool SaveFileAsDialog(HWND owner, const wchar_t* filter, const wchar_t* default_ext,
                      const std::wstring& suggested, std::wstring* out) {
    wchar_t path[MAX_PATH]{};
    wcsncpy_s(path, suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = default_ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    *out = path;
    return true;
}

bool SaveFileDialog(HWND owner, std::wstring* out) {
    wchar_t path[MAX_PATH] = L"dlssnr-out.png";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"PNG image\0*.png\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    *out = path;
    return true;
}

// --------------------------------------------------------------------------
// D3D12 plumbing
// --------------------------------------------------------------------------
void WaitForGpu() {
    if (!g_queue || !g_fence) return;
    const UINT64 target = ++g_fence_last;
    g_queue->Signal(g_fence.Get(), target);
    if (g_fence->GetCompletedValue() < target) {
        g_fence->SetEventOnCompletion(target, g_fence_event);
        WaitForSingleObject(g_fence_event, INFINITE);
    }
}

void CreateRenderTargets() {
    for (int i = 0; i < kFramesInFlight; ++i) {
        ComPtr<ID3D12Resource> back;
        g_swapchain->GetBuffer(UINT(i), IID_PPV_ARGS(&back));
        g_device->CreateRenderTargetView(back.Get(), nullptr, g_rtv[i]);
        g_backbuffers[i] = back;
    }
}

bool CreateDeviceD3D(HWND hwnd) {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)))) {
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = kFramesInFlight;
    if (FAILED(g_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtv_heap)))) return false;
    const UINT rtv_inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < kFramesInFlight; ++i) {
        g_rtv[i] = h;
        h.ptr += rtv_inc;
    }
    if (!g_srv.Create(g_device.Get())) return false;

    for (auto& f : g_frames) {
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    IID_PPV_ARGS(&f.allocator)))) {
            return false;
        }
    }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_frames[0].allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&g_cmd_list)))) {
        return false;
    }
    g_cmd_list->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        return false;
    }
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.BufferCount = kFramesInFlight;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGISwapChain1> swap1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateSwapChainForHwnd(g_queue.Get(), hwnd, &sd, nullptr, nullptr,
                                               &swap1)) ||
        FAILED(swap1.As(&g_swapchain))) {
        return false;
    }
    g_swapchain->SetMaximumFrameLatency(kFramesInFlight);
    g_swapchain_waitable = g_swapchain->GetFrameLatencyWaitableObject();
    CreateRenderTargets();
    return true;
}

void CleanupDeviceD3D() {
    WaitForGpu();
    for (auto& b : g_backbuffers) b.Reset();
    if (g_swapchain_waitable) CloseHandle(g_swapchain_waitable);
    if (g_fence_event) CloseHandle(g_fence_event);
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (g_device && wparam != SIZE_MINIMIZED) {
                WaitForGpu();
                for (auto& b : g_backbuffers) b.Reset();
                g_swapchain->ResizeBuffers(0, UINT(LOWORD(lparam)), UINT(HIWORD(lparam)),
                                           DXGI_FORMAT_UNKNOWN,
                                           DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
                CreateRenderTargets();
            }
            return 0;
        case WM_DROPFILES: {
            auto drop = reinterpret_cast<HDROP>(wparam);
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> paths;
            for (UINT i = 0; i < n; ++i) {
                wchar_t buf[MAX_PATH]{};
                if (DragQueryFileW(drop, i, buf, MAX_PATH)) paths.emplace_back(buf);
            }
            DragFinish(drop);
            {
                std::lock_guard<std::mutex> lock(g_drop_mutex);
                g_dropped = std::move(paths);
            }
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance, nullptr, nullptr,
                   nullptr, nullptr, L"DlssnrStudio", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"DLSS 5 Neural Rendering - Studio",
                              WS_OVERLAPPEDWINDOW, 60, 60, 1600, 950, nullptr, nullptr,
                              instance, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        MessageBoxW(nullptr, L"Failed to create the D3D12 device.", L"dlssnr-studio", MB_ICONERROR);
        return 1;
    }
    DragAcceptFiles(hwnd, TRUE);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini litter beside the exe
    ApplyTheme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_InitInfo info{};
    info.Device = g_device.Get();
    info.CommandQueue = g_queue.Get();
    info.NumFramesInFlight = kFramesInFlight;
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.SrvDescriptorHeap = g_srv.heap.Get();
    info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*,
                                   D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                                   D3D12_GPU_DESCRIPTOR_HANDLE* gpu) { g_srv.Alloc(cpu, gpu); };
    info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
                                  D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                  D3D12_GPU_DESCRIPTOR_HANDLE gpu) { g_srv.Free(cpu, gpu); };
    ImGui_ImplDX12_Init(&info);

    // Locate the snippet beside the executable, as the CLI does.
    std::wstring snippet;
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        snippet = (std::filesystem::path(exe).parent_path() / L"nvngx_dlssnr.dll").wstring();
    }
    g_engine.Start(snippet, L"");

    // ---- application state ----
    dlssnr::Settings settings;
    settings.iterations = 4;
    auto source = std::make_shared<dlssnr::Image>();
    std::shared_ptr<dlssnr::Image> processed;
    std::shared_ptr<dlssnr::DepthImage> depth_map;
    dlssnr::Report report;
    PreviewTexture tex_before, tex_after;
    std::wstring source_path, depth_path;

    // ---- depth estimation state ----
    dlssnr::DepthEstimator estimator;
    dlssnr::DepthOptions depth_opts;
    std::shared_ptr<dlssnr::DepthImage> estimated_depth;
    std::atomic<bool> estimating_depth{false};
    std::mutex depth_mutex;
    std::string depth_error;
    bool depth_from_ai = false;
    bool show_depth = false;
    PreviewTexture tex_depth, tex_mask;
    dlssnr::Sequence sequence;
    dlssnr::PassSet passes;
    std::string seq_error;
    bool seq_encode_video = true;
    bool seq_estimate_motion = true;
    int seq_motion_res = 512;
    int render_start = 0, render_end = 0;  // end 0 = to the last frame
    int seq_fps = 24;
    int seq_bits_index = 0;  // 0 = 8-bit, 1 = 16-bit
    std::shared_ptr<dlssnr::DepthImage> control_mask;
    bool show_mask = false;

    // ---- video source ----
    Scrubber scrubber;
    dlssnr::VideoInfo video_info;
    std::wstring video_path, video_out_path, video_cache;
    std::wstring video_frames_dir, video_audio_path, video_badge_path;
    int video_frame = 0;
    bool video_loaded = false;
    std::string video_error;
    // 0 idle, 1 extracting, 2 extracted, 3 extraction failed
    std::atomic<int> video_stage{0};

    // ---- AI disclosure badge (EU AI Act Art. 50) ----
    bool wm_enabled = true;
    float wm_size_pct = 5.0f, wm_opacity = 0.35f, wm_margin_pct = 2.5f;
    int wm_position = 0;  // 0 bottom-right, 1 bottom-left, 2 bottom-centre
    char wm_text[16] = "AI";
    std::wstring depth_model_path;
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        depth_model_path = (std::filesystem::path(exe).parent_path() /
                            L"depth-anything-v2-small.onnx").wstring();
    }

    float zoom = 1.0f;
    float rotation = 0.0f;  // degrees, -180..180
    ImVec2 pan{0.0f, 0.0f};

    // Loading a still and importing a sequence are reachable from a button, a
    // dropped file and a keyboard shortcut, so both live in one place.
    auto load_source = [&](const std::wstring& path) {
        std::string err;
        auto loaded = std::make_shared<dlssnr::Image>();
        if (!dlssnr::LoadImageFile(path, loaded.get(), &err)) {
            seq_error = err;
            return;
        }
        source = loaded;
        source_path = path;
        sequence = dlssnr::Sequence();
        passes = dlssnr::PassSet();
        depth_map.reset();
        estimated_depth.reset();
        control_mask.reset();
        processed.reset();
        seq_error.clear();
        depth_error.clear();
        show_depth = show_mask = false;
        tex_before.Upload(*source);
        tex_after.Release();
        tex_depth.Release();
        tex_mask.Release();
        zoom = 1.0f;
        rotation = 0.0f;
        pan = ImVec2(0, 0);
        g_engine.SetInput(source, nullptr, nullptr);
        g_engine.Request(settings);
    };

    auto import_sequence = [&](const std::wstring& path) {
        dlssnr::PassSet found;
        std::string err;
        if (!dlssnr::DetectPasses(path, &found, &err)) {
            seq_error = err;
            return;
        }
        passes = found;
        sequence = found.beauty;
        seq_error.clear();
        load_source(sequence.frames.front());
        // load_source clears the sequence, so restore it afterwards.
        passes = found;
        sequence = found.beauty;
    };

    // A video is probed, not extracted: opening a 4K clip has to be instant,
    // and the frames are only worth writing once a render is actually asked
    // for. Scrubbing seeks single frames on demand instead.
    auto load_video = [&](const std::wstring& path) {
        dlssnr::VideoInfo info;
        std::string err;
        if (!dlssnr::ProbeVideo(path, &info, &err)) {
            video_error = err;
            return;
        }
        video_info = info;
        video_path = path;
        video_loaded = true;
        video_error.clear();
        seq_error.clear();
        video_frame = 0;
        sequence = dlssnr::Sequence();
        passes = dlssnr::PassSet();

        std::error_code ec;
        video_cache = (std::filesystem::temp_directory_path() / L"dlssnr_studio").wstring();
        std::filesystem::create_directories(video_cache, ec);

        const std::filesystem::path src(path);
        video_out_path =
            (src.parent_path() / (src.stem().wstring() + L"_nr.mp4")).wstring();

        scrubber.Start(path, video_cache);
        scrubber.Request(0.0);
    };

    auto start_video_render = [&]() {
        video_error.clear();

        dlssnr::EncodeOptions enc;
        enc.fps = (std::max)(1, int(video_info.fps + 0.5));
        enc.crf = 16;
        enc.wm_margin_pct = wm_margin_pct;
        enc.wm_position = wm_position == 1   ? L"bottom-left"
                          : wm_position == 2 ? L"bottom-center"
                                             : L"bottom-right";

        if (wm_enabled) {
            // Badge size follows the output height, so one percentage looks the
            // same whether or not the render upscales.
            const int out_h = (settings.upscaling && settings.output_height)
                                  ? int(settings.output_height)
                                  : video_info.height;
            dlssnr::BadgeOptions badge;
            badge.diameter = dlssnr::BadgeDiameterFor(out_h, wm_size_pct);
            badge.text.assign(wm_text, wm_text + std::strlen(wm_text));
            badge.opacity = wm_opacity;
            video_badge_path =
                (std::filesystem::path(video_cache) / L"ai_badge.png").wstring();
            std::string badge_error;
            if (dlssnr::BuildBadgePng(badge, video_badge_path, &badge_error)) {
                enc.watermark = video_badge_path;
            } else {
                video_error = "badge: " + badge_error;
            }
        }

        dlssnr::Settings s = settings;
        s.iterations = 1;  // history carries across frames instead
        const int count = (std::max)(1, render_end - render_start + 1);
        g_engine.RunVideoStream(video_path, video_out_path, video_info, s, enc,
                                seq_estimate_motion, render_start, count);
    };

    // Progress for both the streaming video render and the image-sequence
    // render: they are the same engine run, so they get the same readout.
    auto draw_sequence_progress = [&]() {
        const int done = g_engine.SeqDone(), total = g_engine.SeqTotal();
        const bool encoding = g_engine.SeqEncoding();

        char label[64];
        if (encoding) {
            sprintf_s(label, "finishing the encode...");
        } else {
            sprintf_s(label, "%d / %d   %.0f%%", done, total,
                      total ? 100.0f * float(done) / float(total) : 0.0f);
        }
        ImGui::ProgressBar(total ? float(done) / float(total) : 0.0f,
                           ImVec2(-FLT_MIN, 0), label);

        char elapsed[24], eta[24];
        FormatDuration(g_engine.SeqElapsed(), elapsed, sizeof(elapsed));
        FormatDuration(g_engine.SeqEta(), eta, sizeof(eta));
        const double ms = g_engine.SeqFrameMs();
        if (encoding) {
            ImGui::TextColored(col::kTextDim, "muxing, almost there");
        } else if (ms > 0.0) {
            ImGui::TextColored(col::kTextDim, "%.2f fps   %.0f ms/frame", 1000.0 / ms, ms);
            ImGui::TextColored(col::kTextDim, "elapsed %s   left %s", elapsed, eta);
        } else {
            ImGui::TextColored(col::kTextDim, "starting...");
        }
        if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0))) g_engine.CancelSequence();
        ImGui::TextColored(col::kTextDim, "preview follows the render");
    };

    bool preview_borrowed = false;  // viewer is showing sequence frames
    std::string preview_error;

    // GPU list is fixed for the session; enumerating it per frame would hit
    // DXGI on the UI thread for no reason.
    const std::vector<dlssnr::AdapterInfo> adapters = dlssnr::ListAdapters();
    int adapter_choice = 0;  // 0 = automatic; otherwise adapter_map[choice - 1]
    std::string adapter_items;
    std::vector<int> adapter_map;  // combo position -> DXGI enumeration index
    {
        adapter_items = "Automatic";
        adapter_items.push_back('\0');
        for (size_t i = 0; i < adapters.size(); ++i) {
            // WARP is enumerated too. Offering it as a choice would be a
            // mis-click that runs the whole thing on the CPU, so it is left out
            // -- while the DXGI index it occupies is still accounted for.
            if (adapters[i].software) continue;

            const std::wstring& wide = adapters[i].name;
            const int need = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0,
                                                 nullptr, nullptr);
            std::string name(need > 0 ? need - 1 : 0, '\0');
            if (need > 0) {
                WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, name.data(), need,
                                    nullptr, nullptr);
            }
            if (adapters[i].dedicated_vram > 0) {
                char vram[32];
                sprintf_s(vram, "  %llu GB",
                          (unsigned long long)(adapters[i].dedicated_vram /
                                               (1024ull * 1024 * 1024)));
                name += vram;
            }
            adapter_items += name;
            adapter_items.push_back('\0');
            adapter_map.push_back(int(i));
        }
        adapter_items.push_back('\0');
    }
    bool auto_apply = true;
    bool show_split = true;
    bool show_original = false;
    bool show_original_latched = false;
    float split = 0.5f;
    bool settings_dirty = false;
    double last_change = 0.0;

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        WaitForSingleObjectEx(g_swapchain_waitable, 100, TRUE);
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ---- pick up finished work ----
        {
            std::shared_ptr<dlssnr::Image> fresh;
            dlssnr::Report fresh_report;
            // During a render the engine can finish frames faster than the UI
            // can usefully show them, and every upload is synchronous GPU work
            // on the UI thread. Take one about every 80 ms and let the engine
            // overwrite the rest; it still reads as the clip playing through.
            static double last_preview = 0.0;
            const double now_s = ImGui::GetTime();
            const bool throttle =
                g_engine.SeqRunning() && (now_s - last_preview) < 0.08;

            std::shared_ptr<dlssnr::Image> fresh_source;
            if (!throttle && g_engine.TakeResult(&fresh, &fresh_report, &fresh_source)) {
                last_preview = now_s;
                processed = fresh;
                report = fresh_report;
                if (!tex_after.Upload(*processed)) {
                    preview_error = "could not upload the result to the preview";
                }
                // A sequence run drives the before-image too, so the compare
                // shows one moment rather than a still against a moving result.
                // `source` itself is left alone: it is what the engine is still
                // tuning against, and the render is only borrowing the viewer.
                if (fresh_source && fresh_source->Valid()) {
                    if (!tex_before.Upload(*fresh_source)) {
                        preview_error = "could not upload the source to the preview";
                    }
                    preview_borrowed = true;
                }
            }
        }

        // Hand the viewer back once the run ends, otherwise the next slider
        // move would compare the new result against the last sequence frame.
        if (preview_borrowed && !g_engine.SeqRunning()) {
            preview_borrowed = false;
            if (source && source->Valid()) tex_before.Upload(*source);
        }

        // ---- a scrubbed frame arrived: make it the thing being tuned ----
        {
            std::wstring scrubbed;
            if (scrubber.Take(&scrubbed)) {
                std::string err;
                auto loaded = std::make_shared<dlssnr::Image>();
                if (dlssnr::LoadImageFile(scrubbed, loaded.get(), &err)) {
                    source = loaded;
                    source_path = video_path;
                    processed.reset();
                    depth_map.reset();
                    estimated_depth.reset();
                    tex_before.Upload(*source);
                    tex_after.Release();
                    g_engine.SetInput(source, nullptr, control_mask);
                    g_engine.Request(settings);
                }
            }
        }

        // ---- pick up a finished depth estimate ----
        {
            std::lock_guard<std::mutex> lock(depth_mutex);
            if (estimated_depth && estimated_depth != depth_map) {
                depth_map = estimated_depth;
                depth_from_ai = true;
                tex_depth.Upload(dlssnr::DepthEstimator::Visualise(*depth_map));
                g_engine.SetInput(source, depth_map, control_mask);
                g_engine.Request(settings);
            }
        }

        // ---- files dropped on the window ----
        {
            std::vector<std::wstring> dropped;
            {
                std::lock_guard<std::mutex> lock(g_drop_mutex);
                dropped.swap(g_dropped);
            }
            if (!dropped.empty()) {
                std::error_code dec;
                // A folder can only mean a sequence; a single file might be
                // either, so try the sequence reading first and fall back.
                if (std::filesystem::is_directory(dropped.front(), dec)) {
                    import_sequence(dropped.front());
                } else {
                    dlssnr::PassSet probe;
                    std::string ignored;
                    if (dropped.size() > 1 &&
                        dlssnr::DetectPasses(dropped.front(), &probe, &ignored)) {
                        import_sequence(dropped.front());
                    } else {
                        load_source(dropped.front());
                    }
                }
            }
        }

        // ---- keyboard ----
        if (!ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
                zoom = 1.0f;
                rotation = 0.0f;
                pan = ImVec2(0, 0);
            }
            // Hold to peek at the original, which is what you actually want
            // while comparing rather than a toggle you have to undo.
            show_original = ImGui::IsKeyDown(ImGuiKey_Space) || show_original_latched;
        }

        // ---- debounce parameter edits ----
        const double now = ImGui::GetTime();
        if (settings_dirty && auto_apply && !g_engine.Busy() && now - last_change > 0.25) {
            settings_dirty = false;
            g_engine.Request(settings);
        }

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);

        // ================= left control panel =================
        ImGui::BeginChild("controls", ImVec2(372, 0), ImGuiChildFlags_Borders);

        auto mark = [&](bool changed) {
            if (changed) {
                settings_dirty = true;
                last_change = now;
            }
        };

        // ===================== SOURCE =====================
        Section("Source");

        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Open image", ImVec2(half, 0))) {
            std::wstring picked;
            if (OpenFileDialog(hwnd,
                               L"Images\0*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.bmp;*.exr\0"
                               L"All files\0*.*\0",
                               &picked)) {
                load_source(picked);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Import sequence", ImVec2(half, 0))) {
            std::wstring picked;
            if (OpenFileDialog(hwnd,
                               L"Any frame of the sequence\0"
                               L"*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.bmp;*.exr\0All files\0*.*\0",
                               &picked)) {
                import_sequence(picked);
            }
        }
        ImGui::SetItemTooltip(
            "Pick any single frame; the rest is found from the numbering.\n"
            "Depth and velocity passes are detected automatically from\n"
            "sibling folders or filenames.");

        if (ImGui::Button("Open video", ImVec2(-FLT_MIN, 0))) {
            std::wstring picked;
            if (OpenFileDialog(hwnd,
                               L"Video\0*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v;*.wmv\0"
                               L"All files\0*.*\0",
                               &picked)) {
                load_video(picked);
            }
        }
        ImGui::SetItemTooltip(
            "Opens instantly -- the clip is probed, not extracted.\n"
            "Scrub to a representative shot, dial the settings in on it,\n"
            "then render the whole thing.");

        if (video_loaded) {
            ImGui::Dummy(ImVec2(0, 4));
            char vinfo[96];
            sprintf_s(vinfo, "%d x %d  %.3g fps", video_info.width, video_info.height,
                      video_info.fps);
            StatRow("video", vinfo);
            char vlen[96];
            sprintf_s(vlen, "%d frames  (%.1f s)%s", video_info.frames,
                      video_info.duration, video_info.has_audio ? "  audio" : "");
            StatRow("length", vlen);
            ImGui::TextColored(
                col::kTextDim, "%s",
                std::filesystem::path(video_path).filename().string().c_str());

            ImGui::Dummy(ImVec2(0, 4));
            const int last_frame = (std::max)(0, video_info.frames - 1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderInt("##scrub", &video_frame, 0, last_frame, "frame %d") &&
                video_info.fps > 0.0) {
                scrubber.Request(video_frame / video_info.fps);
            }
            ImGui::SetItemTooltip("Drag to find the shot you want to tune on.");

            const float third =
                (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) /
                3.0f;
            auto step = [&](int delta) {
                video_frame = (std::min)(last_frame, (std::max)(0, video_frame + delta));
                if (video_info.fps > 0.0) scrubber.Request(video_frame / video_info.fps);
            };
            if (ImGui::Button("-1s", ImVec2(third, 0))) step(-int(video_info.fps + 0.5));
            ImGui::SameLine();
            if (ImGui::Button(scrubber.Busy() ? "seeking" : "reload", ImVec2(third, 0))) {
                if (video_info.fps > 0.0) scrubber.Request(video_frame / video_info.fps);
            }
            ImGui::SameLine();
            if (ImGui::Button("+1s", ImVec2(third, 0))) step(int(video_info.fps + 0.5));
        }
        {
            const std::string scrub_error = scrubber.Error();
            if (!scrub_error.empty()) {
                ImGui::TextColored(col::kBad, "seek: %s", scrub_error.c_str());
            }
        }
        if (!video_error.empty()) ImGui::TextColored(col::kBad, "%s", video_error.c_str());

        if (!source->Valid()) {
            ImGui::Dummy(ImVec2(0, 2));
            ImGui::TextColored(col::kTextDim, "or drop a file or folder on the window");
        } else {
            ImGui::Dummy(ImVec2(0, 2));
            char dims[64];
            sprintf_s(dims, "%u x %u", source->width, source->height);
            StatRow("resolution", dims);
            if (!source_path.empty()) {
                const auto name = std::filesystem::path(source_path).filename().string();
                ImGui::TextColored(col::kTextDim, "%s", name.c_str());
            }
        }

        if (sequence.Valid()) {
            ImGui::Dummy(ImVec2(0, 4));
            char range[80];
            sprintf_s(range, "%zu frames  (%d-%d)", sequence.Count(), sequence.first,
                      sequence.last);
            StatRow("sequence", range);
            ImGui::TextColored(passes.HasDepth() ? col::kGood : col::kTextDim,
                               passes.HasDepth() ? "depth pass found" : "no depth pass");
            ImGui::TextColored(passes.HasVelocity() ? col::kGood : col::kTextDim,
                               passes.HasVelocity() ? "velocity pass found" : "no velocity pass");
        }
        if (!seq_error.empty()) ImGui::TextColored(col::kBad, "%s", seq_error.c_str());

        // ===================== NEURAL =====================
        Section("Neural rendering");

        mark(ImGui::SliderInt("passes", &settings.iterations, 1, 12));
        ImGui::SetItemTooltip(
            "Temporal passes for a still image. Pass 1 cold-starts;\n"
            "the rest let the history settle. Sequences use 1 per frame.");

        static bool use_intensity = false;
        static float intensity = 1.0f;  // runtime default is 1
        {
            ImGui::PushID("intensity");
            if (ImGui::Checkbox("##on", &use_intensity)) {
                settings.intensity = use_intensity ? intensity : dlssnr::kUnset;
                mark(true);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!use_intensity);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat("##v", &intensity, 0.0f, 2.0f, "intensity  %.2f")) {
                settings.intensity = intensity;
                mark(true);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::SetItemTooltip(
                "Linear 0 to 1, then clamps -- above 1 does nothing.\n"
                "0 is a verified exact identity: output matches input.");
        }

        static bool on_style = (dlssnr::Settings().style >= 0);
        static int v_style = (std::max)(0, dlssnr::Settings().style);
        {
            ImGui::PushID("style");
            if (ImGui::Checkbox("##on", &on_style)) {
                settings.style = on_style ? v_style : -1;
                mark(true);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!on_style);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderInt("##v", &v_style, 0, 6, "style  %d")) {
                settings.style = v_style;
                mark(true);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            ImGui::SetItemTooltip(
                "Three real behaviours, not seven:\n"
                "  0     runtime default: warms and darkens\n"
                "  1     over-cooked: orange cast, crushed shadows\n"
                "  2-6   identical to each other, and cleanest");
        }
        {
            bool automask = settings.use_auto_mask != 0;
            if (ImGui::Checkbox("auto mask", &automask)) {
                settings.use_auto_mask = automask ? 1 : 0;
                mark(true);
            }
            ImGui::SetItemTooltip(
                "The runtime's own mask, used when no control mask is given.\n"
                "Measurably active: diff 0.0141 off vs 0.0132 on.");
            ImGui::SameLine();
            bool uicorr = settings.ui_correction > 0;
            if (ImGui::Checkbox("UI correction", &uicorr)) {
                settings.ui_correction = uicorr ? 1 : -1;
                mark(true);
            }
            ImGui::SetItemTooltip("DLSSNR.UICorrection.");
        }

        // These three only bite when depth or motion guides are present, which
        // is why an early sweep on a bare still wrongly wrote them off. The
        // state is shown on screen because it is not otherwise discoverable.
        {
            ImGui::Dummy(ImVec2(0, 2));
            static bool on_ls = false, on_skin = false, on_lt = false;
            static float v_ls = 1.0f, v_skin = 1.0f, v_lt = 1.0f;
            auto opt = [&](const char* label, float* store, float* value, bool* enabled,
                           float lo, float hi, const char* tip) {
                ImGui::PushID(label);
                if (ImGui::Checkbox("##on", enabled)) {
                    *store = *enabled ? *value : dlssnr::kUnset;
                    mark(true);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!*enabled);
                ImGui::SetNextItemWidth(-FLT_MIN);
                char fmt[64];
                sprintf_s(fmt, "%s  %%.2f", label);
                if (ImGui::SliderFloat("##v", value, lo, hi, fmt)) {
                    *store = *value;
                    mark(true);
                }
                ImGui::EndDisabled();
                if (tip) ImGui::SetItemTooltip("%s", tip);
                ImGui::PopID();
            };
            opt("local structure", &settings.local_structure, &v_ls, &on_ls, 0.0f, 2.0f,
                "Surface detail. Range 0-2, default 1.\n"
                "diff 0.0085 / 0.0132 / 0.0142 at 0 / 1 / 1.5.");
            opt("skin structure", &settings.skin_structure, &v_skin, &on_skin, -1.0f, 2.0f,
                "Pore and micro-detail on faces. Range -1 to 2, default 1.\n"
                "Only 0 measurably differs here; -1 is a real value, not 'off'.");
            opt("local tone", &settings.local_tone, &v_lt, &on_lt, 0.0f, 2.0f,
                "Responds across the whole range, unlike the others:\n"
                "diff 0.0063 / 0.0132 / 0.0162 at 0 / 1 / 2.");
        }

        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Checkbox("auto apply", &auto_apply);
        ImGui::SameLine();
        ImGui::BeginDisabled(g_engine.Busy() || !source->Valid());
        if (ImGui::Button("Render now", ImVec2(-FLT_MIN, 0))) g_engine.Request(settings);
        ImGui::EndDisabled();

        // ===================== GUIDES =====================
        Section("Guides");
        ImGui::TextColored(col::kTextDim, "Optional engine inputs. Depth has no");
        ImGui::TextColored(col::kTextDim, "measurable effect on output.");

        if (ImGui::TreeNode("Depth")) {
            ImGui::BeginDisabled(!source->Valid() || estimating_depth);
            if (ImGui::Button(estimating_depth ? "Estimating..." : "Estimate from image",
                              ImVec2(-FLT_MIN, 0))) {
                estimating_depth = true;
                std::thread([&] {
                    std::string err;
                    if (!estimator.Loaded()) estimator.Load(depth_model_path, &err);
                    auto d = std::make_shared<dlssnr::DepthImage>();
                    if (estimator.Loaded() &&
                        estimator.Estimate(*source, depth_opts, d.get(), &err)) {
                        std::lock_guard<std::mutex> lock(depth_mutex);
                        estimated_depth = d;
                        depth_error.clear();
                    } else {
                        std::lock_guard<std::mutex> lock(depth_mutex);
                        depth_error = err;
                    }
                    estimating_depth = false;
                }).detach();
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Depth Anything V2. A real Z pass is always better.");

            if (ImGui::Button("Load depth file", ImVec2(-FLT_MIN, 0))) {
                std::wstring picked;
                if (OpenFileDialog(hwnd, L"Images\0*.exr;*.png;*.tif;*.tiff;*.jpg\0All\0*.*\0",
                                   &picked)) {
                    std::string err;
                    auto d = std::make_shared<dlssnr::DepthImage>();
                    if (dlssnr::LoadDepth(picked, d.get(), &err) && source->Valid() &&
                        d->width == source->width && d->height == source->height) {
                        depth_map = d;
                        depth_from_ai = false;
                        tex_depth.Upload(dlssnr::DepthEstimator::Visualise(*depth_map));
                        g_engine.SetInput(source, depth_map, control_mask);
                        g_engine.Request(settings);
                    } else {
                        depth_error = err.empty() ? "size does not match the image" : err;
                    }
                }
            }
            if (depth_map) {
                ImGui::TextColored(col::kGood, depth_from_ai ? "estimated" : "loaded");
                ImGui::SameLine();
                if (ImGui::SmallButton("clear##d")) {
                    depth_map.reset();
                    estimated_depth.reset();
                    show_depth = false;
                    g_engine.SetInput(source, nullptr, control_mask);
                    g_engine.Request(settings);
                }
                ImGui::Checkbox("show depth", &show_depth);
            } else {
                ImGui::TextColored(col::kTextDim, "flat plane");
            }
            if (!depth_error.empty()) ImGui::TextColored(col::kBad, "%s", depth_error.c_str());

            ImGui::SliderInt("res", &depth_opts.resolution, 154, 1036);
            ImGui::Checkbox("invert (near = 0)", &depth_opts.invert);
            ImGui::SliderFloat("gamma", &depth_opts.gamma, 0.25f, 4.0f);
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Control mask")) {
            if (ImGui::Button("Load mask", ImVec2(-FLT_MIN, 0))) {
                std::wstring picked;
                if (OpenFileDialog(hwnd, L"Images\0*.png;*.exr;*.jpg;*.tif;*.tiff\0All\0*.*\0",
                                   &picked)) {
                    std::string err;
                    auto m = std::make_shared<dlssnr::DepthImage>();
                    if (dlssnr::LoadDepth(picked, m.get(), &err) && source->Valid() &&
                        m->width == source->width && m->height == source->height) {
                        control_mask = m;
                        tex_mask.Upload(dlssnr::DepthEstimator::Visualise(*control_mask));
                        g_engine.SetInput(source, depth_map, control_mask);
                        g_engine.Request(settings);
                    }
                }
            }
            ImGui::SetItemTooltip(
                "DLSSNR.ControlMask, read from the red channel.\n"
                "White applies, black suppresses entirely.\n"
                "An all-black mask is a verified exact identity.");
            if (control_mask) {
                ImGui::TextColored(col::kGood, "loaded");
                ImGui::SameLine();
                if (ImGui::SmallButton("clear##m")) {
                    control_mask.reset();
                    tex_mask.Release();
                    show_mask = false;
                    g_engine.SetInput(source, depth_map, nullptr);
                    g_engine.Request(settings);
                }
                ImGui::Checkbox("show mask", &show_mask);
            } else {
                ImGui::TextColored(col::kTextDim, "none - runtime uses its own");
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Inert parameters")) {
            ImGui::TextColored(col::kTextDim, "Byte-identical output at every value,");
            ImGui::TextColored(col::kTextDim, "with and without guides.");
            ImGui::Dummy(ImVec2(0, 3));
            ImGui::SetNextItemWidth(-FLT_MIN);
            mark(ImGui::SliderInt("##preset", &settings.render_preset, 0, 3, "preset  %d"));
            ImGui::SetItemTooltip("The addon offers Default plus three; none differ.");
            ImGui::SetNextItemWidth(-FLT_MIN);
            mark(ImGui::SliderInt("##perf", &settings.perf_quality, 0, 5, "perf/quality  %d"));
            ImGui::TextColored(col::kTextDim, "global tone: not a runtime key at all;");
            ImGui::TextColored(col::kTextDim, "it exists only in the addon's shader.");
            ImGui::TreePop();
        }

        // ===================== OUTPUT =====================
        Section("Output");

        if (sequence.Valid() || video_loaded) {
            ImGui::BeginDisabled(g_engine.SeqRunning());
            ImGui::Checkbox("estimate motion", &seq_estimate_motion);
            ImGui::SetItemTooltip(
                "Optical flow (RAFT) for footage with no velocity pass.\n"
                "NR carries history between frames and needs motion to\n"
                "reproject it; without this a render is measurably worse.\n"
                "It is also the slowest part -- roughly a second a frame.");
            if (seq_estimate_motion) {
                // The bundled raft.onnx is exported at a frozen input size, so
                // it dictates its own resolution and this setting changes
                // nothing: 256, 512 and 768 all measured the same. Left visible
                // but disabled rather than removed, because a re-exported model
                // with dynamic axes would make it live again.
                ImGui::BeginDisabled(true);
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderInt("##mres", &seq_motion_res, 256, 1024, "flow res %d");
                ImGui::EndDisabled();
                ImGui::TextColored(col::kTextDim, "model has a fixed input size");
            }
            if (passes.HasVelocity()) {
                ImGui::TextColored(col::kGood, "velocity pass found; flow not needed");
            }
            ImGui::EndDisabled();
            ImGui::Dummy(ImVec2(0, 4));
        }

        if (sequence.Valid()) {
            ImGui::Checkbox("encode video", &seq_encode_video);
            if (seq_encode_video) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90);
                ImGui::InputInt("fps", &seq_fps);
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::Combo("##bits", &seq_bits_index, "8-bit PNG\0" "16-bit PNG\0");
            ImGui::SetItemTooltip(
                "8-bit is ~12 MB a frame at 4K, 16-bit is ~45 MB.\n"
                "Both feed the same h.264 encode.");

            if (g_engine.SeqRunning()) {
                draw_sequence_progress();
            } else if (ImGui::Button("Process sequence", ImVec2(-FLT_MIN, 0))) {
                const auto dir = std::filesystem::path(sequence.directory) / L"nr_out";
                std::wstring video;
                if (seq_encode_video) {
                    video =
                        (std::filesystem::path(sequence.directory) / L"nr_result.mp4").wstring();
                }
                dlssnr::Settings s = settings;
                s.iterations = 1;  // one evaluation per frame; history carries across
                dlssnr::EncodeOptions enc;
                enc.fps = seq_fps;
                g_engine.RunSequence(passes, s, dir.wstring(), video, enc,
                                     seq_bits_index == 0 ? 8 : 16,
                                     seq_estimate_motion, seq_motion_res);
            }
            ImGui::TextColored(col::kTextDim, "writes to nr_out beside the source");
        }

        if (video_loaded) {
            const auto out_name =
                std::filesystem::path(video_out_path).filename().string();
            StatRow("output", out_name.empty() ? "(not set)" : out_name.c_str());
            const int last_frame = (std::max)(0, video_info.frames - 1);
            if (render_end <= 0 || render_end > last_frame) render_end = last_frame;

            ImGui::BeginDisabled(g_engine.SeqRunning());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderInt("##rstart", &render_start, 0, last_frame, "from %d")) {
                if (render_start > render_end) render_end = render_start;
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderInt("##rend", &render_end, 0, last_frame, "to %d")) {
                if (render_end < render_start) render_start = render_end;
            }
            const float halfw =
                (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("Start here", ImVec2(halfw, 0))) {
                render_start = video_frame;
                if (render_end < render_start) render_end = last_frame;
            }
            ImGui::SetItemTooltip("Begin at the frame you scrubbed to.");
            ImGui::SameLine();
            if (ImGui::Button("Whole clip", ImVec2(halfw, 0))) {
                render_start = 0;
                render_end = last_frame;
            }
            {
                const int count = (std::max)(0, render_end - render_start + 1);
                char span[96];
                if (video_info.fps > 0.0) {
                    sprintf_s(span, "%d frames  (%.1f s)", count, count / video_info.fps);
                } else {
                    sprintf_s(span, "%d frames", count);
                }
                StatRow("range", span);
            }
            ImGui::EndDisabled();
            ImGui::Dummy(ImVec2(0, 4));

            if (ImGui::Button("Choose output...", ImVec2(-FLT_MIN, 0))) {
                std::wstring picked;
                if (SaveFileAsDialog(hwnd, L"MP4 video\0*.mp4\0", L"mp4",
                                     video_out_path, &picked)) {
                    video_out_path = picked;
                }
            }

            ImGui::Dummy(ImVec2(0, 4));
            ImGui::Checkbox("AI disclosure badge", &wm_enabled);
            ImGui::SetItemTooltip(
                "EU AI Act Art. 50 visible mark. The machine-readable half of\n"
                "that obligation is written into the file's metadata either way --\n"
                "a burned-in badge alone does not satisfy it.");
            if (wm_enabled) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::Combo("##wmpos", &wm_position,
                             "bottom right\0" "bottom left\0" "bottom centre\0");
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderFloat("##wmsize", &wm_size_pct, 2.0f, 12.0f, "size %.1f%%");
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderFloat("##wmop", &wm_opacity, 0.10f, 1.0f, "opacity %.2f");
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputText("##wmtext", wm_text, sizeof(wm_text));
            }

            ImGui::Dummy(ImVec2(0, 4));
            if (g_engine.SeqRunning()) {
                draw_sequence_progress();
            } else {
                ImGui::BeginDisabled(video_out_path.empty());
                if (ImGui::Button("Render video", ImVec2(-FLT_MIN, 0))) {
                    start_video_render();
                }
                ImGui::EndDisabled();
                char estimate[128];
                sprintf_s(estimate,
                          "%d frames at the current settings -- extraction first,\n"
                          "then the neural pass, which is the slow part.",
                          video_info.frames);
                ImGui::SetItemTooltip("%s", estimate);
                ImGui::TextColored(col::kTextDim, "settings above apply to every frame");
            }
        }

        ImGui::BeginDisabled(!processed || !processed->Valid());
        if (ImGui::Button("Save image as PNG", ImVec2(-FLT_MIN, 0))) {
            std::wstring out;
            if (SaveFileDialog(hwnd, &out)) {
                std::string err;
                dlssnr::SaveImage(out, *processed, 16, true, &err);
            }
        }
        ImGui::EndDisabled();

        if (ImGui::Button("Reset parameters", ImVec2(-FLT_MIN, 0))) {
            const int p = settings.iterations;
            settings = dlssnr::Settings();
            settings.iterations = p;
            use_intensity = false;
            on_style = (settings.style >= 0);
            v_style = (std::max)(0, settings.style);
            mark(true);
        }

        // ===================== VIEW =====================
        Section("View");
        ImGui::Checkbox("split compare", &show_split);
        ImGui::SameLine();
        ImGui::Checkbox("original", &show_original_latched);
        ImGui::SetItemTooltip("Hold SPACE for a momentary look at the original.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##zoom", &zoom, 0.1f, 8.0f, "zoom  %.2fx");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##rot", &rotation, -180.0f, 180.0f, "rotate  %+.0f°");
        ImGui::SetItemTooltip(
            "Freely rotate the view, -180° to +180°. Display only -- the image\n"
            "sent to the runtime and saved to disk is never rotated.");
        if (ImGui::Button("Fit  (F)", ImVec2(-FLT_MIN, 0))) {
            zoom = 1.0f;
            rotation = 0.0f;
            pan = ImVec2(0, 0);
        }

        // ===================== STATUS =====================
        // ===================== GPU =====================
        // Only worth showing when there is a choice to make. On a single-GPU
        // box the combo would be a control with one meaningful setting.
        if (adapter_map.size() == 1) {
            Section("GPU");
            const auto& only = adapters[adapter_map.front()];
            char gpu_name[128];
            WideCharToMultiByte(CP_UTF8, 0, only.name.c_str(), -1, gpu_name,
                                sizeof(gpu_name), nullptr, nullptr);
            StatRow("device", gpu_name);
            ImGui::TextColored(col::kTextDim, "only one GPU present; nothing to choose");
        } else if (adapter_map.size() > 1) {
            Section("GPU");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::BeginDisabled(g_engine.SeqRunning());
            if (ImGui::Combo("##gpu", &adapter_choice, adapter_items.c_str())) {
                // A different GPU means a different D3D12 device, so the whole
                // runtime comes down and back up.
                const int dxgi_index =
                    adapter_choice > 0 && adapter_choice <= int(adapter_map.size())
                        ? adapter_map[adapter_choice - 1]
                        : -1;
                g_engine.Restart(dxgi_index);
                if (source && source->Valid()) {
                    g_engine.SetInput(source, depth_map, control_mask);
                    g_engine.Request(settings);
                }
            }
            ImGui::EndDisabled();
            if (g_engine.SeqRunning()) {
                ImGui::TextColored(col::kTextDim, "locked while a render is running");
            } else {
                ImGui::TextColored(col::kTextDim, "switching reloads the NR runtime");
            }
        }

        Section("Status");
        if (!preview_error.empty()) {
            ImGui::TextColored(col::kBad, "%s", preview_error.c_str());
        }
        if (!g_engine.Ready()) {
            ImGui::TextColored(col::kWarn, "%s", g_engine.Status().c_str());
        } else if (g_engine.Busy() || g_engine.SeqRunning()) {
            ImGui::TextColored(col::kAccent, "working...");
        } else {
            ImGui::TextColored(col::kGood, "ready");
        }
        ImGui::TextWrapped("%s", g_engine.Status().c_str());

        if (!report.evaluations.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            const auto& e = report.evaluations.back();
            char buf[64];
            sprintf_s(buf, "%.0f ms", e.gpu_wait_ms);
            StatRow("last pass", buf);
            sprintf_s(buf, "%.4f", e.mean_absolute_rgb);
            StatRow("mean |rgb|", buf);
            sprintf_s(buf, "%.4f", e.mean_absolute_difference_from_input);
            StatRow("diff vs input", buf);
            if (report.feature_id >= 0) {
                sprintf_s(buf, "%d", report.feature_id);
                StatRow("NGX feature", buf);
            }
        }
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::EndChild();

        // ================= image view =================
        ImGui::SameLine();
        ImGui::BeginChild("view", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const ImVec2 total = ImGui::GetContentRegionAvail();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const bool showing_mask = show_mask && tex_mask.width;
        const bool showing_depth = (show_depth && tex_depth.width) || showing_mask;
        const bool split_active =
            show_split && tex_after.width && !show_original && !showing_depth;

        // Reserve the bottom strip for the split slider BEFORE sizing the
        // canvas. The canvas is an InvisibleButton spanning its whole rect and
        // takes ActiveId on mouse-down, so anything overlapping it never sees a
        // click -- they have to occupy disjoint space.
        const float bar_height = split_active ? 28.0f : 0.0f;
        const ImVec2 avail(total.x, (std::max)(1.0f, total.y - bar_height));

        // Canvas dimensions come from whichever texture actually has content.
        // Gating the whole viewport on the before-image meant that a render
        // whose source preview had not landed drew nothing at all: a black
        // canvas with a perfectly good result sitting unused in tex_after.
        const uint32_t view_w = tex_before.width ? tex_before.width : tex_after.width;
        const uint32_t view_h = tex_before.width ? tex_before.height : tex_after.height;
        if (view_w && view_h) {
            const float base = (std::min)(avail.x / float(view_w),
                                          avail.y / float(view_h));
            const ImVec2 size(view_w * base * zoom, view_h * base * zoom);
            const ImVec2 center(origin.x + avail.x * 0.5f + pan.x,
                                origin.y + avail.y * 0.5f + pan.y);

            // Rotate the image around its own centre. Positive angles turn the
            // top edge to the right (clockwise on screen).
            const float rot = rotation * (float)M_PI / 180.0f;
            const float c = std::cos(rot), s = std::sin(rot);
            auto xform = [&](float px, float py) {
                const float dx = px - center.x, dy = py - center.y;
                return ImVec2(center.x + dx * c - dy * s, center.y + dx * s + dy * c);
            };
            // Image corners in screen space: top-left, top-right, bottom-right,
            // bottom-left -- the order AddImageQuad expects. ALL FOUR are rotated
            // through xform so the quad stays a flat 2D in-plane rotation (a plain
            // "spin the image on a table") with no skew.
            const ImVec2 tl = xform(center.x - size.x * 0.5f, center.y - size.y * 0.5f);
            const ImVec2 tr = xform(center.x + size.x * 0.5f, center.y - size.y * 0.5f);
            const ImVec2 br = xform(center.x + size.x * 0.5f, center.y + size.y * 0.5f);
            const ImVec2 bl = xform(center.x - size.x * 0.5f, center.y + size.y * 0.5f);

            // Screen-space bounding box of the rotated image (all four corners).
            // The split divider lives in this box, so it stays a fixed vertical
            // line on screen.
            const float bb_left  = std::min(std::min(tl.x, tr.x), std::min(bl.x, br.x));
            const float bb_right = std::max(std::max(tl.x, tr.x), std::max(bl.x, br.x));
            const float bb_top   = std::min(std::min(tl.y, tr.y), std::min(bl.y, br.y));
            const float bb_bot   = std::max(std::max(tl.y, tr.y), std::max(bl.y, br.y));
            const float split_x  = bb_left + (bb_right - bb_left) * split;

            const ImGuiIO& io = ImGui::GetIO();
            const bool over_divider =
                split_active && std::fabs(io.MousePos.x - split_x) < 8.0f &&
                io.MousePos.y >= bb_top && io.MousePos.y <= bb_bot;

            ImGui::InvisibleButton("canvas", avail,
                                   ImGuiButtonFlags_MouseButtonLeft |
                                       ImGuiButtonFlags_MouseButtonRight);

            // Latch the gesture on press: grabbing the seam moves the divider,
            // anywhere else pans.
            static bool dragging_divider = false;
            if (ImGui::IsItemActivated()) dragging_divider = over_divider;
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                if (dragging_divider && bb_right > bb_left) {
                    split = (std::max)(0.0f, (std::min)(1.0f,
                            (io.MousePos.x - bb_left) / (bb_right - bb_left)));
                } else {
                    pan.x += io.MouseDelta.x;
                    pan.y += io.MouseDelta.y;
                }
            }
            if (over_divider || (ImGui::IsItemActive() && dragging_divider)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
                zoom = (std::max)(0.1f, (std::min)(8.0f, zoom * (1.0f + io.MouseWheel * 0.1f)));
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImTextureID after = tex_after.width ? (ImTextureID)tex_after.gpu.ptr
                                                      : (ImTextureID)tex_before.gpu.ptr;
            const ImTextureID before = tex_before.width ? (ImTextureID)tex_before.gpu.ptr
                                                        : (ImTextureID)tex_after.gpu.ptr;

            // Keep an on-canvas label inside the visible canvas rect. Rotating
            // the image pushes its bounding box past the canvas edge, so anchoring
            // a label to the image (bb_*) would clip it off-screen. Clamping to
            // [origin, origin+avail] keeps the label visible while still tracking
            // the image (same reason the rotation readout at the top-left is
            // always visible).
            auto label_pos = [&](float x, float y, const char* text) {
                ImVec2 ts = ImGui::CalcTextSize(text);
                float lx = (std::max)(origin.x + 4.0f,
                           (std::min)(x, origin.x + avail.x - ts.x - 4.0f));
                float ly = (std::max)(origin.y + 4.0f,
                           (std::min)(y, origin.y + avail.y - ts.y - 4.0f));
                return ImVec2(lx, ly);
            };

            if (showing_depth) {
                dl->AddImageQuad((ImTextureID)(showing_mask ? tex_mask.gpu.ptr : tex_depth.gpu.ptr),
                                 tl, tr, br, bl);
                dl->AddText(label_pos(tl.x + 8, tl.y + 6,
                            showing_mask ? "control mask" : "estimated depth"),
                            IM_COL32(255, 255, 255, 200),
                            showing_mask ? "control mask" : "estimated depth");
            } else if (show_original || !tex_after.width) {
                dl->AddImageQuad(before, tl, tr, br, bl);
            } else if (show_split) {
                // Screen-space split: a fixed vertical divider that does NOT
                // rotate with the image. Each half draws the full rotated image
                // (no UV remapping, so nothing distorts) behind a screen-space
                // clip rect computed from the rotated bounding box above.
                // Left half: original
                dl->PushClipRect(ImVec2(bb_left, bb_top), ImVec2(split_x, bb_bot), true);
                dl->AddImageQuad(before, tl, tr, br, bl);
                dl->PopClipRect();
                // Right half: neural
                dl->PushClipRect(ImVec2(split_x, bb_top), ImVec2(bb_right, bb_bot), true);
                dl->AddImageQuad(after, tl, tr, br, bl);
                dl->PopClipRect();
                // Screen-space vertical divider line
                dl->AddLine(ImVec2(split_x, bb_top), ImVec2(split_x, bb_bot),
                            IM_COL32(255, 220, 60, 220), over_divider ? 3.0f : 1.5f);
                // Labels track the top of each half but are clamped to the canvas
                // so they never rotate off-screen.
                const float lbl_off = 8.0f;
                const float lbl_y   = bb_top + 6.0f;
                const ImVec2 p_orig = label_pos(bb_left + lbl_off, lbl_y, "original");
                const float min_neural_x = p_orig.x + ImGui::CalcTextSize("original").x + 12.0f;
                const ImVec2 p_neural = label_pos(
                    (std::max)(split_x + lbl_off, min_neural_x), lbl_y, "neural");
                dl->AddText(p_orig, IM_COL32(255, 255, 255, 200), "original");
                dl->AddText(p_neural, IM_COL32(255, 255, 255, 200), "neural");
            } else {
                dl->AddImageQuad(after, tl, tr, br, bl);
            }
            // A render that starts on a dark stretch of source looks broken
            // without this: the preview is genuinely black, and nothing else on
            // the canvas says which frame it is.
            if (g_engine.SeqRunning()) {
                const int done = g_engine.SeqDone();
                const int absolute = g_engine.SeqStartFrame() + done;
                char stamp[96];
                if (video_loaded && video_info.fps > 0.0) {
                    const double t = absolute / video_info.fps;
                    sprintf_s(stamp, "frame %d / %d   %02d:%05.2f", absolute,
                              g_engine.SeqStartFrame() + g_engine.SeqTotal(),
                              int(t) / 60, std::fmod(t, 60.0));
                } else {
                    sprintf_s(stamp, "frame %d / %d", done, g_engine.SeqTotal());
                }
                const ImVec2 ts = ImGui::CalcTextSize(stamp);
                const ImVec2 at(origin.x + avail.x - ts.x - 10.0f,
                                origin.y + avail.y - ts.y - 10.0f);
                dl->AddRectFilled(ImVec2(at.x - 6, at.y - 4),
                                  ImVec2(at.x + ts.x + 6, at.y + ts.y + 4),
                                  IM_COL32(0, 0, 0, 150), 4.0f);
                dl->AddText(at, IM_COL32(255, 220, 60, 230), stamp);
            }
            if (rotation != 0.0f) {
                char rbuf[32];
                sprintf_s(rbuf, "%+.0f°", rotation);
                dl->AddText(ImVec2(origin.x + 10, origin.y + 8), IM_COL32(255, 220, 60, 200), rbuf);
            }

            if (split_active) {
                ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + avail.y + 4.0f));
                ImGui::SetNextItemWidth(total.x);
                ImGui::SliderFloat("##split", &split, 0.0f, 1.0f, "split %.2f");
            }
        } else {
            ImGui::Dummy(ImVec2(0, avail.y * 0.45f));
            const char* msg = "Open an image to begin.";
            ImGui::SetCursorPosX((avail.x - ImGui::CalcTextSize(msg).x) * 0.5f);
            ImGui::TextDisabled("%s", msg);
        }
        ImGui::EndChild();
        ImGui::End();

        // ---- present ----
        ImGui::Render();
        FrameContext& frame = g_frames[g_frame_index % kFramesInFlight];
        frame.allocator->Reset();
        g_cmd_list->Reset(frame.allocator.Get(), nullptr);

        const UINT back_index = g_swapchain->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_backbuffers[back_index].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_cmd_list->ResourceBarrier(1, &barrier);

        const float clear[4] = {0.09f, 0.09f, 0.11f, 1.0f};
        g_cmd_list->ClearRenderTargetView(g_rtv[back_index], clear, 0, nullptr);
        g_cmd_list->OMSetRenderTargets(1, &g_rtv[back_index], FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {g_srv.heap.Get()};
        g_cmd_list->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmd_list.Get());

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        g_cmd_list->ResourceBarrier(1, &barrier);
        g_cmd_list->Close();
        ID3D12CommandList* lists[] = {g_cmd_list.Get()};
        g_queue->ExecuteCommandLists(1, lists);
        g_swapchain->Present(1, 0);

        const UINT64 signal = ++g_fence_last;
        g_queue->Signal(g_fence.Get(), signal);
        frame.fence_value = signal;
        ++g_frame_index;
        FrameContext& next = g_frames[g_frame_index % kFramesInFlight];
        if (next.fence_value && g_fence->GetCompletedValue() < next.fence_value) {
            g_fence->SetEventOnCompletion(next.fence_value, g_fence_event);
            WaitForSingleObject(g_fence_event, INFINITE);
        }
    }

    g_engine.Stop();
    WaitForGpu();
    tex_before.Release();
    tex_after.Release();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
