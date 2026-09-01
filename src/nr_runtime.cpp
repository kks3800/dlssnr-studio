#include "nr_runtime.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_params.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dlssnr {
namespace {

// ---------------------------------------------------------------------------
// NGX entry points. Resolved by name from the driver-store core rather than
// link-time, because we never want to bind against a particular driver.
// ---------------------------------------------------------------------------
using PFN_Init_Ext = NVSDK_NGX_Result(__cdecl*)(unsigned long long, const wchar_t*,
                                                ID3D12Device*, NVSDK_NGX_Version,
                                                const NVSDK_NGX_Parameter*);
using PFN_Shutdown1 = NVSDK_NGX_Result(__cdecl*)(ID3D12Device*);
using PFN_AllocateParameters = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter**);
using PFN_DestroyParameters = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter*);
using PFN_CreateFeature = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*,
                                                     NVSDK_NGX_Feature,
                                                     NVSDK_NGX_Parameter*,
                                                     NVSDK_NGX_Handle**);
using PFN_ReleaseFeature = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Handle*);
using PFN_EvaluateFeature = NVSDK_NGX_Result(__cdecl*)(ID3D12GraphicsCommandList*,
                                                       const NVSDK_NGX_Handle*,
                                                       const NVSDK_NGX_Parameter*,
                                                       void*);
using PFN_GetScratchBufferSize = NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Feature,
                                                            const NVSDK_NGX_Parameter*,
                                                            size_t*);

using PFN_GetSnippetVersion = unsigned(__cdecl*)();

// The application-facing NVSDK_NGX_D3D12_Init -- note the argument order
// differs from the snippet-facing one: the FeatureCommonInfo sits *before* the
// version. Its PathListInfo is the documented way to tell the core where to
// look for feature DLLs beyond the application folder, which is the whole
// reason an out-of-tree snippet can be found at all.
using PFN_CoreInit = NVSDK_NGX_Result(__cdecl*)(unsigned long long, const wchar_t*,
                                                ID3D12Device*,
                                                const NVSDK_NGX_FeatureCommonInfo*,
                                                NVSDK_NGX_Version);

// The snippet DLL exports the whole NGX surface itself -- Init_Ext,
// CreateFeature, EvaluateFeature, the lot -- but NOT AllocateParameters. So the
// core is used purely as a parameter allocator and every feature call goes
// straight to the snippet. This is what lets an unreleased feature run on a
// driver core that has never heard of it: we never ask the core to resolve a
// feature id it has no mapping for.
struct NgxApi {
    HMODULE core = nullptr;
    HMODULE snippet = nullptr;

    // The core is only good for initialising the NGX runtime and allocating a
    // parameter block -- this driver's core is older than the NR feature and
    // answers OutOfDate if asked to dispatch it. Feature calls therefore go
    // straight to the snippet, which exports the full API itself.
    PFN_CoreInit core_init = nullptr;
    PFN_AllocateParameters allocate_parameters = nullptr;
    PFN_DestroyParameters destroy_parameters = nullptr;

    PFN_Init_Ext init = nullptr;
    PFN_CreateFeature create_feature = nullptr;
    PFN_ReleaseFeature release_feature = nullptr;
    PFN_EvaluateFeature evaluate_feature = nullptr;
    PFN_GetScratchBufferSize get_scratch_size = nullptr;
    PFN_Shutdown1 shutdown = nullptr;

    bool Complete() const {
        return init && allocate_parameters && create_feature && evaluate_feature;
    }
};

// Parameter keys, recovered from dlssnr-repro.pdb.
constexpr const char* kColor = "DLSSNR.Color";
constexpr const char* kDepth = "DLSSNR.Depth";
constexpr const char* kMVec = "DLSSNR.MVec";
constexpr const char* kOutput = "DLSSNR.Output";
constexpr const char* kInputWidth = "DLSSNR.InputWidth";
constexpr const char* kInputHeight = "DLSSNR.InputHeight";
constexpr const char* kOutputWidth = "DLSSNR.OutputWidth";
constexpr const char* kOutputHeight = "DLSSNR.OutputHeight";
constexpr const char* kWidth = "DLSSNR.Width";
constexpr const char* kHeight = "DLSSNR.Height";
constexpr const char* kOutputWidthDotted = "DLSSNR.Output.Width";
constexpr const char* kOutputHeightDotted = "DLSSNR.Output.Height";
constexpr const char* kMVecScaleX = "DLSSNR.MVecScaleX";
constexpr const char* kMVecScaleY = "DLSSNR.MVecScaleY";
constexpr const char* kDepthInverted = "DLSSNR.DepthInverted";
constexpr const char* kReset = "DLSSNR.Reset";
constexpr const char* kEnabled = "DLSSNR.Enabled";
constexpr const char* kUpscaling = "DLSSNR.Upscaling";
constexpr const char* kIntensity = "DLSSNR.Intensity";
constexpr const char* kStyle = "DLSSNR.Style";
constexpr const char* kRenderPreset = "DLSSNR.Hint.Render.Preset";
constexpr const char* kSkinStructure = "DLSSNR.SkinStructureStrength";
constexpr const char* kLocalStructure = "DLSSNR.LocalStructureStrength";
constexpr const char* kLocalTone = "DLSSNR.LocalToneStrength";
constexpr const char* kGlobalTone = "DLSSNR.GlobalToneStrength";
constexpr const char* kUseAutoMask = "DLSSNR.UseAutoMask";
// Recovered by scanning the snippet itself rather than the addon PDB, which
// listed a superset containing keys the runtime does not actually read.
constexpr const char* kControlMask = "DLSSNR.ControlMask";
constexpr const char* kBackbuffer = "DLSSNR.Backbuffer";
constexpr const char* kDistortion =
    "DLSSNR.BidirectionalDistortionField";
constexpr const char* kUiCorrection = "DLSSNR.UICorrection";
constexpr const char* kScale = "DLSSNR.Scale";
constexpr const char* kScalingRatio = "DLSSNR.ScalingRatio";
constexpr const char* kCreationNodeMask = "CreationNodeMask";
constexpr const char* kVisibilityNodeMask = "VisibilityNodeMask";
constexpr const char* kPerfQuality = "PerfQualityValue";
constexpr const char* kPerfQualityDlssnr = "DLSSNR.PerfQualityValue";
constexpr const char* kScratch = "Scratch";
constexpr const char* kScratchSize = "Scratch.SizeInBytes";

// The NR feature id is not in the public SDK. NVSDK_NGX_Feature_Count is 19 in
// the 310.7 header we build against, so a 310.8 feature can legitimately sit at
// or beyond that -- we probe well past the end of the published enum rather
// than assuming it reuses a Reserved slot.
constexpr int kProbeFirst = 1;
constexpr int kProbeLast = 40;
// Anything below this is a published feature that may answer for reasons that
// have nothing to do with our snippet, so a hit there is only a fallback.
constexpr int kFirstUnpublishedFeature = 14;

// ---------------------------------------------------------------------------
// Caller-origin check.
//
// The snippet only accepts calls from a module whose file name contains
// "nvngx.dll" -- in a game that is the NGX core, which is what normally drives
// it. An application calling the snippet directly gets 0xBAD00002
// PlatformError from every entry point. We patch GetModuleFileNameW in the
// snippet's import table so that when it asks about *our* module it is told a
// path that satisfies the check. Queries about any other module pass straight
// through untouched.
// ---------------------------------------------------------------------------
using PFN_GetModuleFileNameW = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

PFN_GetModuleFileNameW g_real_get_module_file_name = nullptr;
HMODULE g_host_module = nullptr;
std::wstring g_spoofed_host_path;

DWORD WINAPI HookedGetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size) {
    if (module == g_host_module && !g_spoofed_host_path.empty() && buffer && size) {
        const DWORD len = DWORD(g_spoofed_host_path.size());
        const DWORD copied = len < size - 1 ? len : size - 1;
        std::wmemcpy(buffer, g_spoofed_host_path.c_str(), copied);
        buffer[copied] = L'\0';
        SetLastError(copied < len ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS);
        return copied;
    }
    return g_real_get_module_file_name
               ? g_real_get_module_file_name(module, buffer, size)
               : 0;
}

// Redirects one imported function in an already-loaded module. Matches on the
// function name across every import descriptor, so it does not matter whether
// the import resolves through kernel32 or an api-ms-win-* stub.
bool PatchImport(HMODULE module, const char* function, void* replacement,
                 void** original) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;

    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         desc->Name; ++desc) {
        const DWORD names_rva =
            desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        if (!names_rva || !desc->FirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + names_rva);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* imp = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(imp->Name, function) != 0) continue;

            DWORD old_protect = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE,
                                &old_protect)) {
                return false;
            }
            if (original) *original = reinterpret_cast<void*>(iat->u1.Function);
            iat->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&iat->u1.Function, sizeof(void*), old_protect, &old_protect);
            return true;
        }
    }
    return false;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

D3D12_RESOURCE_DESC Tex2D(DXGI_FORMAT fmt, uint32_t w, uint32_t h,
                          D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Alignment = 0;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_DESC Buffer(uint64_t bytes) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    return d;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
             D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

}  // namespace

// ---------------------------------------------------------------------------

std::wstring FindNgxCore() {
    wchar_t win[MAX_PATH]{};
    if (!GetWindowsDirectoryW(win, MAX_PATH)) return {};
    const std::filesystem::path repo =
        std::filesystem::path(win) / L"System32" / L"DriverStore" / L"FileRepository";
    std::error_code ec;
    if (!std::filesystem::exists(repo, ec)) return {};

    std::filesystem::path best;
    std::filesystem::file_time_type best_time{};
    for (auto it = std::filesystem::directory_iterator(
             repo, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto candidate = it->path() / L"nvngx.dll";
        if (!std::filesystem::exists(candidate, ec)) continue;
        const auto t = std::filesystem::last_write_time(candidate, ec);
        if (ec) continue;
        if (best.empty() || t > best_time) {
            best = candidate;
            best_time = t;
        }
    }
    return best.empty() ? std::wstring{} : best.wstring();
}

// ---------------------------------------------------------------------------

struct Runtime::Impl {
    NgxApi ngx;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0;

    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    NVSDK_NGX_Feature feature_id = NVSDK_NGX_Feature_Reserved0;
    ComPtr<ID3D12Resource> scratch;

    // Textures and staging buffers are reused between calls. Recreating ~200 MB
    // of committed resources for every frame cost measurable wall time and
    // taxed every interactive slider tweak; they only depend on the extents.
    struct Staged {
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        uint64_t total = 0, row_bytes = 0;
        uint32_t rows = 0;
    };
    ComPtr<ID3D12Resource> tex_color, tex_motion, tex_depth, tex_out, tex_mask;
    ComPtr<ID3D12Resource> tex_backbuffer, tex_distortion;
    bool backbuffer_primed = false;  // nothing useful in it on the first frame
    Staged up_color, up_motion, up_depth, up_mask, up_distortion, read_out;
    uint32_t res_in_w = 0, res_in_h = 0, res_out_w = 0, res_out_h = 0;
    bool res_has_mask = false;

    uint32_t feature_in_w = 0, feature_in_h = 0, feature_out_w = 0, feature_out_h = 0;
    bool ngx_initialised = false;

    // Deliberately no NVSDK_NGX_D3D12_Shutdown1 here. The reference harness
    // skips it too ("one-shot process exit owns driver-core teardown") -- an
    // unreleased snippet does not survive an orderly core shutdown, and calling
    // it faults inside the driver.
    ~Impl() {
        if (fence_event) CloseHandle(fence_event);
    }

    bool Flush(double* elapsed_ms) {
        LARGE_INTEGER freq{}, t0{}, t1{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        const uint64_t target = ++fence_value;
        if (FAILED(queue->Signal(fence.Get(), target))) return false;
        if (fence->GetCompletedValue() < target) {
            if (FAILED(fence->SetEventOnCompletion(target, fence_event))) return false;
            WaitForSingleObject(fence_event, INFINITE);
        }
        QueryPerformanceCounter(&t1);
        if (elapsed_ms) {
            *elapsed_ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(freq.QuadPart);
        }
        return true;
    }

    bool Reset() {
        return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr));
    }

    bool CloseAndExecute() {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        return true;
    }
};

Runtime::Runtime() : impl_(new Impl()) {}
Runtime::~Runtime() { delete impl_; }

bool Runtime::Initialize(const std::wstring& snippet_dll,
                         const std::wstring& core_dll, std::string* error,
                         int adapter_index) {
    auto& d = *impl_;

    // The NGX core discovers feature snippets among already-loaded modules, so
    // the snippet must be resident before Init. This is what lets us run an
    // arbitrary nvngx_dlssnr.dll instead of whatever the driver ships.
    d.ngx.snippet = LoadLibraryW(snippet_dll.c_str());
    if (!d.ngx.snippet) {
        if (error) {
            *error = "could not load snippet " + Narrow(snippet_dll) +
                     " (error " + std::to_string(GetLastError()) + ")";
        }
        return false;
    }

    std::wstring core = core_dll.empty() ? FindNgxCore() : core_dll;
    if (core.empty()) {
        if (error) *error = "no nvngx.dll found in the driver store";
        return false;
    }
    d.ngx.core = LoadLibraryW(core.c_str());
    if (!d.ngx.core) {
        if (error) {
            *error = "could not load NGX core " + Narrow(core) + " (error " +
                     std::to_string(GetLastError()) + ")";
        }
        return false;
    }

    auto from_core = [&](const char* name) { return GetProcAddress(d.ngx.core, name); };
    auto from_snippet = [&](const char* name) { return GetProcAddress(d.ngx.snippet, name); };

    d.ngx.core_init = reinterpret_cast<PFN_CoreInit>(from_core("NVSDK_NGX_D3D12_Init"));
    d.ngx.allocate_parameters = reinterpret_cast<PFN_AllocateParameters>(
        from_core("NVSDK_NGX_D3D12_AllocateParameters"));
    d.ngx.destroy_parameters = reinterpret_cast<PFN_DestroyParameters>(
        from_core("NVSDK_NGX_D3D12_DestroyParameters"));

    d.ngx.init = reinterpret_cast<PFN_Init_Ext>(from_snippet("NVSDK_NGX_D3D12_Init_Ext"));
    d.ngx.create_feature =
        reinterpret_cast<PFN_CreateFeature>(from_snippet("NVSDK_NGX_D3D12_CreateFeature"));
    d.ngx.release_feature =
        reinterpret_cast<PFN_ReleaseFeature>(from_snippet("NVSDK_NGX_D3D12_ReleaseFeature"));
    d.ngx.evaluate_feature =
        reinterpret_cast<PFN_EvaluateFeature>(from_snippet("NVSDK_NGX_D3D12_EvaluateFeature"));
    d.ngx.get_scratch_size = reinterpret_cast<PFN_GetScratchBufferSize>(
        from_snippet("NVSDK_NGX_D3D12_GetScratchBufferSize"));
    d.ngx.shutdown = reinterpret_cast<PFN_Shutdown1>(from_snippet("NVSDK_NGX_D3D12_Shutdown1"));
    if (!d.ngx.Complete()) {
        if (error) *error = "core/snippet did not export the required entry points";
        return false;
    }

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        if (error) *error = "CreateDXGIFactory2 failed";
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    // -1 means "let DXGI choose", which is the right answer on a single-GPU
    // machine and on most laptops. An explicit index is for the case DXGI gets
    // wrong: an eGPU, or a box where the render GPU is not the display one.
    const UINT wanted = adapter_index >= 0 ? UINT(adapter_index) : 0u;
    if (FAILED(factory->EnumAdapterByGpuPreference(
            wanted, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
        if (adapter_index >= 0 &&
            SUCCEEDED(factory->EnumAdapterByGpuPreference(
                0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
            // Asked-for GPU has gone (unplugged eGPU, driver reset): fall back
            // rather than refusing to start.
        } else {
            if (error) *error = "no such adapter";
            return false;
        }
    }
    DXGI_ADAPTER_DESC1 adesc{};
    adapter->GetDesc1(&adesc);
    adapter_name_ = adesc.Description;

    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&d.device)))) {
        if (error) *error = "D3D12CreateDevice failed";
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(d.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&d.queue))) ||
        FAILED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&d.allocator))) ||
        FAILED(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           d.allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&d.list))) ||
        FAILED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)))) {
        if (error) *error = "could not create D3D12 command objects";
        return false;
    }
    d.list->Close();
    d.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d.fence_event) {
        if (error) *error = "CreateEvent failed";
        return false;
    }

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path app_data = std::filesystem::path(temp) / L"dlssnr-image";
    std::error_code ec;
    std::filesystem::create_directories(app_data, ec);

    auto hex = [](NVSDK_NGX_Result v) {
        char b[16];
        sprintf_s(b, "%08X", unsigned(v));
        return std::string(b);
    };

    // Core init, with no path list -- pointing it at the snippet only makes it
    // notice a feature library newer than itself and refuse with OutOfDate. We
    // want the core up purely so it can hand us a parameter block.
    const NVSDK_NGX_Result rc =
        d.ngx.core_init(0x0, app_data.c_str(), d.device.Get(), nullptr, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(rc)) {
        if (error) *error = "NGX core Init failed (0x" + hex(rc) + ")";
        return false;
    }
    if (NVSDK_NGX_FAILED(d.ngx.allocate_parameters(&d.params)) || !d.params) {
        if (error) *error = "NVSDK_NGX_D3D12_AllocateParameters failed";
        return false;
    }

    // Satisfy the snippet's caller-origin check before calling into it. The
    // path we hand back is the real NGX core we loaded, which genuinely ends in
    // nvngx.dll, so the substring test passes.
    g_host_module = GetModuleHandleW(nullptr);
    g_spoofed_host_path = core;
    if (!PatchImport(d.ngx.snippet, "GetModuleFileNameW",
                     reinterpret_cast<void*>(&HookedGetModuleFileNameW),
                     reinterpret_cast<void**>(&g_real_get_module_file_name))) {
        if (error) {
            *error = "could not redirect GetModuleFileNameW in the snippet import "
                     "table; the snippet gates its entry points on the caller "
                     "module and will refuse to initialise";
        }
        return false;
    }

    // Now bring the snippet up, handing it the core's parameter block. A
    // snippet-build Init_Ext takes that block as its final argument -- it is how
    // the core passes configuration down.
    const NVSDK_NGX_Result rs =
        d.ngx.init(0x0, app_data.c_str(), d.device.Get(), NVSDK_NGX_Version_API, d.params);
    if (NVSDK_NGX_FAILED(rs)) {
        if (error) *error = "snippet Init_Ext failed (0x" + hex(rs) + ")";
        return false;
    }
    d.ngx_initialised = true;
    return true;
}

bool Runtime::Process(const Image& color, const DepthImage* depth,
                      const DepthImage* control_mask, const MotionField* motion,
                      const Settings& settings, Image* out, Report* report,
                      std::string* error) {
    auto& d = *impl_;
    if (!color.Valid()) {
        if (error) *error = "input image is invalid";
        return false;
    }
    if (depth && (depth->width != color.width || depth->height != color.height)) {
        if (error) *error = "depth map dimensions do not match the colour image";
        return false;
    }

    const uint32_t in_w = color.width, in_h = color.height;
    const uint32_t out_w = settings.output_width ? settings.output_width : in_w;
    const uint32_t out_h = settings.output_height ? settings.output_height : in_h;

    // --- resources, matching the reference harness exactly -----------------
    // colour/motion R16G16B16A16_FLOAT, depth R32_FLOAT, all in
    // NON_PIXEL_SHADER_RESOURCE; output R16G16B16A16_FLOAT with
    // ALLOW_UNORDERED_ACCESS in UNORDERED_ACCESS.
    if (control_mask && (control_mask->width != color.width ||
                         control_mask->height != color.height)) {
        if (error) *error = "control mask dimensions do not match the colour image";
        return false;
    }

    auto make_tex = [&](DXGI_FORMAT fmt, uint32_t w, uint32_t h, bool uav,
                        D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>* dst) {
        const auto props = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = Tex2D(fmt, w, h,
                                uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                    : D3D12_RESOURCE_FLAG_NONE);
        return SUCCEEDED(d.device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(dst->GetAddressOf())));
    };
    const auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const bool want_mask = control_mask != nullptr;
    const bool resources_stale = d.res_in_w != in_w || d.res_in_h != in_h ||
                                 d.res_out_w != out_w || d.res_out_h != out_h ||
                                 (want_mask && !d.res_has_mask);
    auto& tex_color = d.tex_color;
    auto& tex_motion = d.tex_motion;
    auto& tex_depth = d.tex_depth;
    auto& tex_out = d.tex_out;
    auto& tex_mask = d.tex_mask;
    if (resources_stale &&
        (!make_tex(DXGI_FORMAT_R16G16B16A16_FLOAT, in_w, in_h, false, kSrv, &tex_color) ||
        !make_tex(DXGI_FORMAT_R16G16B16A16_FLOAT, in_w, in_h, false, kSrv, &tex_motion) ||
        !make_tex(DXGI_FORMAT_R32_FLOAT, in_w, in_h, false, kSrv, &tex_depth) ||
         !make_tex(DXGI_FORMAT_R16G16B16A16_FLOAT, out_w, out_h, true,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &tex_out))) {
        if (error) *error = "could not create NR textures";
        return false;
    }
    if (resources_stale) {
        d.tex_backbuffer.Reset();
        d.backbuffer_primed = false;
    }
    if (resources_stale) {
        d.tex_distortion.Reset();
        d.up_distortion.buffer.Reset();
    }
    if (settings.use_distortion && !d.tex_distortion) {
        if (!make_tex(DXGI_FORMAT_R16G16B16A16_FLOAT, in_w, in_h, false, kSrv,
                      &d.tex_distortion)) {
            if (error) *error = "could not create the distortion texture";
            return false;
        }
    }
    if (settings.use_backbuffer && !d.tex_backbuffer) {
        // Same format and size as the output, since that is what gets copied
        // into it after every evaluate.
        if (!make_tex(DXGI_FORMAT_R16G16B16A16_FLOAT, out_w, out_h, false, kSrv,
                      &d.tex_backbuffer)) {
            if (error) *error = "could not create the backbuffer texture";
            return false;
        }
    }
    // Same single-channel float layout as depth; the mask is authoring data,
    // not colour, so it gets no transfer function of any kind.
    if (want_mask && (resources_stale || !tex_mask) &&
        !make_tex(DXGI_FORMAT_R32_FLOAT, in_w, in_h, false, kSrv, &tex_mask)) {
        if (error) *error = "could not create the control mask texture";
        return false;
    }

    // --- staging ------------------------------------------------------------
    auto footprint = [&](ID3D12Resource* res, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp,
                         uint64_t* total, uint32_t* rows, uint64_t* row_bytes) {
        const auto desc = res->GetDesc();
        d.device->GetCopyableFootprints(&desc, 0, 1, 0, fp, rows, row_bytes, total);
    };
    auto make_buffer = [&](D3D12_HEAP_TYPE type, uint64_t bytes, D3D12_RESOURCE_STATES state,
                           ComPtr<ID3D12Resource>* dst) {
        const auto props = HeapProps(type);
        const auto desc = Buffer(bytes);
        return SUCCEEDED(d.device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(dst->GetAddressOf())));
    };

    using Staged = Runtime::Impl::Staged;
    auto& up_color = d.up_color;
    auto& up_motion = d.up_motion;
    auto& up_depth = d.up_depth;
    auto& up_mask = d.up_mask;
    auto& read_out = d.read_out;
    footprint(tex_color.Get(), &up_color.fp, &up_color.total, &up_color.rows, &up_color.row_bytes);
    footprint(tex_motion.Get(), &up_motion.fp, &up_motion.total, &up_motion.rows, &up_motion.row_bytes);
    footprint(tex_depth.Get(), &up_depth.fp, &up_depth.total, &up_depth.rows, &up_depth.row_bytes);
    footprint(tex_out.Get(), &read_out.fp, &read_out.total, &read_out.rows, &read_out.row_bytes);
    if (resources_stale &&
        (!make_buffer(D3D12_HEAP_TYPE_UPLOAD, up_color.total, D3D12_RESOURCE_STATE_GENERIC_READ, &up_color.buffer) ||
        !make_buffer(D3D12_HEAP_TYPE_UPLOAD, up_motion.total, D3D12_RESOURCE_STATE_GENERIC_READ, &up_motion.buffer) ||
        !make_buffer(D3D12_HEAP_TYPE_UPLOAD, up_depth.total, D3D12_RESOURCE_STATE_GENERIC_READ, &up_depth.buffer) ||
         !make_buffer(D3D12_HEAP_TYPE_READBACK, read_out.total, D3D12_RESOURCE_STATE_COPY_DEST, &read_out.buffer))) {
        if (error) *error = "could not create staging buffers";
        return false;
    }
    if (settings.use_distortion && d.tex_distortion && !d.up_distortion.buffer) {
        footprint(d.tex_distortion.Get(), &d.up_distortion.fp, &d.up_distortion.total,
                  &d.up_distortion.rows, &d.up_distortion.row_bytes);
        if (!make_buffer(D3D12_HEAP_TYPE_UPLOAD, d.up_distortion.total,
                         D3D12_RESOURCE_STATE_GENERIC_READ, &d.up_distortion.buffer)) {
            if (error) *error = "could not create the distortion upload buffer";
            return false;
        }
    }
    d.res_in_w = in_w;
    d.res_in_h = in_h;
    d.res_out_w = out_w;
    d.res_out_h = out_h;
    if (want_mask) d.res_has_mask = true;

    auto fill = [&](Staged& s, auto&& row_writer) {
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED(s.buffer->Map(0, &none, &mapped))) return false;
        auto* base = static_cast<uint8_t*>(mapped) + s.fp.Offset;
        for (uint32_t y = 0; y < s.rows; ++y) {
            row_writer(y, base + size_t(y) * s.fp.Footprint.RowPitch);
        }
        s.buffer->Unmap(0, nullptr);
        return true;
    };

    fill(up_color, [&](uint32_t y, uint8_t* dst) {
        std::memcpy(dst, color.texels.data() + size_t(y) * in_w * 4, size_t(in_w) * 8);
    });
    // Zeros for a still; a real field when one is supplied. NR reads screen-space
    // motion from RG, so B and A stay clear.
    const bool have_motion =
        motion && motion->Valid() && motion->width == in_w && motion->height == in_h;
    fill(up_motion, [&](uint32_t y, uint8_t* dst) {
        auto* h = reinterpret_cast<uint16_t*>(dst);
        if (!have_motion) {
            std::memset(dst, 0, size_t(in_w) * 8);
            return;
        }
        const float* row = motion->xy.data() + size_t(y) * in_w * 2;
        for (uint32_t x = 0; x < in_w; ++x) {
            h[x * 4 + 0] = FloatToHalf(row[x * 2 + 0]);
            h[x * 4 + 1] = FloatToHalf(row[x * 2 + 1]);
            h[x * 4 + 2] = 0;
            h[x * 4 + 3] = 0;
        }
    });
    if (settings.use_distortion && d.tex_distortion) {
        const float cx = in_w * 0.5f, cy = in_h * 0.5f;
        fill(d.up_distortion, [&](uint32_t y, uint8_t* dst) {
            auto* h = reinterpret_cast<uint16_t*>(dst);
            if (settings.use_distortion == 1) {
                std::memset(dst, 0, size_t(in_w) * 8);
                return;
            }
            // A swirl about the centre, tens of pixels at the edges. If the
            // snippet reads this at all, a displacement this large cannot come
            // out looking the same.
            for (uint32_t x = 0; x < in_w; ++x) {
                const float dx = (x - cx) / cx, dy = (y - cy) / cy;
                h[x * 4 + 0] = FloatToHalf(-dy * 40.0f);
                h[x * 4 + 1] = FloatToHalf(dx * 40.0f);
                h[x * 4 + 2] = 0;
                h[x * 4 + 3] = 0;
            }
        });
    }
    fill(up_depth, [&](uint32_t y, uint8_t* dst) {
        auto* f = reinterpret_cast<float*>(dst);
        if (depth) {
            std::memcpy(f, depth->texels.data() + size_t(y) * in_w, size_t(in_w) * 4);
        } else {
            for (uint32_t x = 0; x < in_w; ++x) f[x] = settings.constant_depth;
        }
    });
    if (control_mask) {
        footprint(tex_mask.Get(), &up_mask.fp, &up_mask.total, &up_mask.rows, &up_mask.row_bytes);
        if (!up_mask.buffer &&
            !make_buffer(D3D12_HEAP_TYPE_UPLOAD, up_mask.total,
                         D3D12_RESOURCE_STATE_GENERIC_READ, &up_mask.buffer)) {
            if (error) *error = "could not create the control mask staging buffer";
            return false;
        }
        fill(up_mask, [&](uint32_t y, uint8_t* dst) {
            std::memcpy(dst, control_mask->texels.data() + size_t(y) * in_w, size_t(in_w) * 4);
        });
    }

    if (!d.Reset()) {
        if (error) *error = "command list reset failed";
        return false;
    }
    auto upload = [&](Staged& s, ID3D12Resource* tex, D3D12_RESOURCE_STATES state) {
        Barrier(d.list.Get(), tex, state, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = s.buffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = s.fp;
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(d.list.Get(), tex, D3D12_RESOURCE_STATE_COPY_DEST, state);
    };
    upload(up_color, tex_color.Get(), kSrv);
    upload(up_motion, tex_motion.Get(), kSrv);
    if (settings.use_distortion && d.tex_distortion) {
        upload(d.up_distortion, d.tex_distortion.Get(), kSrv);
    }
    upload(up_depth, tex_depth.Get(), kSrv);
    if (tex_mask) upload(up_mask, tex_mask.Get(), kSrv);
    if (!d.CloseAndExecute() || !d.Flush(nullptr)) {
        if (error) *error = "fixture upload failed";
        return false;
    }

    // --- feature ------------------------------------------------------------
    auto* p = d.params;
    auto set_dims = [&] {
        p->Set(kCreationNodeMask, 1u);
        p->Set(kVisibilityNodeMask, 1u);
        p->Set(kInputWidth, in_w);
        p->Set(kInputHeight, in_h);
        p->Set(kWidth, in_w);
        p->Set(kHeight, in_h);
        p->Set(kOutputWidth, out_w);
        p->Set(kOutputHeight, out_h);
        p->Set(kOutputWidthDotted, out_w);
        p->Set(kOutputHeightDotted, out_h);
        p->Set(kEnabled, 1);
        p->Set(kUpscaling, settings.upscaling ? 1 : 0);
        // ComputeScalingRatio in the reference implementation derives this from
        // the input/output extents; 1.0 is the identity case.
        const float ratio = float(out_w) / float(in_w ? in_w : 1);
        p->Set(kScale, ratio);
        p->Set(kScalingRatio, ratio);
        // Mirrors "create.allocate_parameters size=... performance=3 preset=1"
        // from the reference harness breadcrumbs.
        p->Set(kPerfQuality, settings.perf_quality);
        p->Set(kPerfQualityDlssnr, settings.perf_quality);
        if (settings.render_preset >= 0) {
            p->Set(kRenderPreset, unsigned(settings.render_preset));
        }
    };

    const bool need_feature = !d.feature || d.feature_in_w != in_w || d.feature_in_h != in_h ||
                              d.feature_out_w != out_w || d.feature_out_h != out_h;
    if (need_feature) {
        if (d.feature) {
            d.ngx.release_feature(d.feature);
            d.feature = nullptr;
        }
        set_dims();

        // Identify the feature slot before touching a command list.
        // GetScratchBufferSize answers FAIL_FeatureNotSupported for a slot the
        // loaded snippets do not implement, which separates "wrong slot" from
        // "right slot, unhappy parameters" -- a distinction CreateFeature alone
        // does not give us. It also yields the scratch allocation NGX needs.
        std::string probe_log;
        size_t scratch_bytes = 0;
        int chosen = -1, fallback = -1;
        size_t fallback_bytes = 0;
        if (d.ngx.get_scratch_size) {
            for (int id = kProbeFirst; id <= kProbeLast; ++id) {
                size_t bytes = 0;
                const NVSDK_NGX_Result r =
                    d.ngx.get_scratch_size(NVSDK_NGX_Feature(id), p, &bytes);
                if (NVSDK_NGX_FAILED(r)) continue;  // only log what answers
                char line[128];
                sprintf_s(line, "    feature %-2d -> supported (scratch %zu bytes)\n", id, bytes);
                probe_log += line;
                if (id >= kFirstUnpublishedFeature && chosen < 0) {
                    chosen = id;
                    scratch_bytes = bytes;
                } else if (fallback < 0) {
                    fallback = id;
                    fallback_bytes = bytes;
                }
            }
        }
        if (chosen < 0 && fallback >= 0) {
            chosen = fallback;
            scratch_bytes = fallback_bytes;
        }
        const bool found = chosen >= 0;
        if (!found) chosen = kFirstUnpublishedFeature;

        if (scratch_bytes) {
            const auto props = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
            const auto desc = Buffer(scratch_bytes);
            if (FAILED(d.device->CreateCommittedResource(
                    &props, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                    IID_PPV_ARGS(d.scratch.ReleaseAndGetAddressOf())))) {
                if (error) *error = "could not allocate the NGX scratch buffer";
                return false;
            }
            p->Set(kScratch, d.scratch.Get());
            p->Set(kScratchSize, unsigned(scratch_bytes));
        }

        if (!d.Reset()) {
            if (error) *error = "command list reset failed before CreateFeature";
            return false;
        }
        NVSDK_NGX_Result last = NVSDK_NGX_Result_Fail;
        std::string create_log;
        for (int id = kProbeFirst; id <= kProbeLast; ++id) {
            if (found && id != chosen) continue;
            if (!found && id < kFirstUnpublishedFeature) continue;
            NVSDK_NGX_Handle* handle = nullptr;
            last = d.ngx.create_feature(d.list.Get(), NVSDK_NGX_Feature(id), p, &handle);
            char line[80];
            sprintf_s(line, "    feature %-2d -> 0x%08X\n", id, unsigned(last));
            create_log += line;
            if (!NVSDK_NGX_FAILED(last) && handle) {
                d.feature = handle;
                d.feature_id = NVSDK_NGX_Feature(id);
                break;
            }
            if (handle) d.ngx.release_feature(handle);
        }
        probe_log += "  CreateFeature results:\n" + create_log;
        if (!d.feature) {
            d.list->Close();
            if (error) {
                char b[16];
                sprintf_s(b, "%08X", unsigned(last));
                *error = std::string("CreateFeature failed (last result 0x") + b + ").\n" +
                         "  features answering GetScratchBufferSize:\n" +
                         (probe_log.empty() ? std::string("    none\n") : probe_log) +
                         "  0xBAD00001 FeatureNotSupported  0xBAD0000B UnableToInitializeFeature"
                         "  0xBAD0000C OutOfDate";
            }
            return false;
        }
        if (!d.CloseAndExecute() || !d.Flush(nullptr)) {
            if (error) *error = "CreateFeature submission failed";
            return false;
        }
        d.feature_in_w = in_w;
        d.feature_in_h = in_h;
        d.feature_out_w = out_w;
        d.feature_out_h = out_h;
    }
    if (report) {
        report->feature_id = int(d.feature_id);
        report->adapter = adapter_name_;
    }

    // --- evaluate -----------------------------------------------------------
    const int iterations = std::max(1, settings.iterations);
    std::vector<uint16_t> latest;
    for (int i = 0; i < iterations; ++i) {
        if (!d.Reset()) {
            if (error) *error = "command list reset failed before evaluate";
            return false;
        }
        set_dims();
        p->Set(kColor, tex_color.Get());
        p->Set(kDepth, tex_depth.Get());
        p->Set(kMVec, tex_motion.Get());
        p->Set(kOutput, tex_out.Get());
        if (tex_mask) p->Set(kControlMask, tex_mask.Get());
        // One reading of the key: in a renderer the backbuffer is the
        // previously presented image. Only bound once there is a previous
        // frame to offer, so the first frame of a sequence is unchanged.
        if (settings.use_distortion && d.tex_distortion) {
            p->Set(kDistortion, d.tex_distortion.Get());
        }
        if (settings.use_backbuffer == 2) {
            // The other reading of the key: the image itself, which is what a
            // single-frame renderer would have sitting in its backbuffer. Bound
            // from the first frame, since no history is needed for it.
            p->Set(kBackbuffer, tex_color.Get());
        } else if (settings.use_backbuffer && d.tex_backbuffer && d.backbuffer_primed) {
            p->Set(kBackbuffer, d.tex_backbuffer.Get());
        }

        // Every resource has its own subrect quartet. We use whole textures, so
        // these are origin plus full extent -- but they are real keys in the
        // runtime and leaving them unset means relying on undocumented defaults.
        auto set_subrect = [&](const char* prefix, uint32_t w, uint32_t h) {
            const std::string base = std::string("DLSSNR.") + prefix + "Subrect";
            p->Set((base + "BaseX").c_str(), 0u);
            p->Set((base + "BaseY").c_str(), 0u);
            p->Set((base + "Width").c_str(), w);
            p->Set((base + "Height").c_str(), h);
        };
        set_subrect("Color", in_w, in_h);
        set_subrect("Depth", in_w, in_h);
        set_subrect("MVec", in_w, in_h);
        set_subrect("Output", out_w, out_h);
        if (tex_mask) set_subrect("ControlMask", in_w, in_h);
        if (settings.use_distortion && d.tex_distortion) {
            set_subrect("BidirectionalDistortionField", in_w, in_h);
        }
        if (settings.use_backbuffer == 2) {
            set_subrect("Backbuffer", in_w, in_h);
        } else if (settings.use_backbuffer && d.tex_backbuffer && d.backbuffer_primed) {
            set_subrect("Backbuffer", out_w, out_h);
        }
        p->Set(kMVecScaleX, 1.0f);
        p->Set(kMVecScaleY, 1.0f);
        p->Set(kDepthInverted, settings.depth_inverted ? 1 : 0);
        // Only the first pass starts cold, and only when the caller asks for it
        // -- a sequence resets on frame one and then accumulates.
        p->Set(kReset, (i == 0 && settings.reset_history) ? 1 : 0);
        if (settings.intensity != kUnset) p->Set(kIntensity, settings.intensity);
        if (settings.style >= 0) p->Set(kStyle, settings.style);
        if (settings.render_preset >= 0) p->Set(kRenderPreset, unsigned(settings.render_preset));
        if (settings.skin_structure != kUnset) p->Set(kSkinStructure, settings.skin_structure);
        if (settings.local_structure != kUnset) p->Set(kLocalStructure, settings.local_structure);
        if (settings.local_tone != kUnset) p->Set(kLocalTone, settings.local_tone);
        if (settings.global_tone != kUnset) p->Set(kGlobalTone, settings.global_tone);
        if (settings.use_auto_mask >= 0) p->Set(kUseAutoMask, settings.use_auto_mask);
        if (settings.ui_correction >= 0) p->Set(kUiCorrection, settings.ui_correction);

        const NVSDK_NGX_Result r =
            d.ngx.evaluate_feature(d.list.Get(), d.feature, p, nullptr);
        if (NVSDK_NGX_FAILED(r)) {
            d.list->Close();
            char b[16];
            sprintf_s(b, "%08X", unsigned(r));
            if (error) *error = std::string("EvaluateFeature failed (0x") + b + ")";
            return false;
        }

        Barrier(d.list.Get(), tex_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = tex_out.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        dst.pResource = read_out.buffer.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = read_out.fp;
        d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        // Hold on to this frame's result so the next one can offer it as the
        // backbuffer. The copy happens on the GPU, so it costs no round trip.
        if (settings.use_backbuffer && d.tex_backbuffer) {
            Barrier(d.list.Get(), d.tex_backbuffer.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION bb_src{}, bb_dst{};
            bb_src.pResource = tex_out.Get();
            bb_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bb_dst.pResource = d.tex_backbuffer.Get();
            bb_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            d.list->CopyTextureRegion(&bb_dst, 0, 0, 0, &bb_src, nullptr);
            Barrier(d.list.Get(), d.tex_backbuffer.Get(),
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            d.backbuffer_primed = true;
        }

        Barrier(d.list.Get(), tex_out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        double wait_ms = 0.0;
        if (!d.CloseAndExecute() || !d.Flush(&wait_ms)) {
            if (error) *error = "evaluation submission failed";
            return false;
        }

        latest.assign(size_t(out_w) * out_h * 4, 0);
        void* mapped = nullptr;
        D3D12_RANGE range{0, size_t(read_out.total)};
        if (FAILED(read_out.buffer->Map(0, &range, &mapped))) {
            if (error) *error = "readback map failed";
            return false;
        }
        const auto* base = static_cast<const uint8_t*>(mapped) + read_out.fp.Offset;
        for (uint32_t y = 0; y < out_h; ++y) {
            std::memcpy(latest.data() + size_t(y) * out_w * 4,
                        base + size_t(y) * read_out.fp.Footprint.RowPitch, size_t(out_w) * 8);
        }
        D3D12_RANGE none{0, 0};
        read_out.buffer->Unmap(0, &none);

        if (report) {
            EvaluationStat s;
            s.index = i + 1;
            s.gpu_wait_ms = wait_ms;
            s.finite = true;
            double sum = 0.0, diff = 0.0;
            float first = HalfToFloat(latest[0]);
            const bool same_size = (out_w == in_w && out_h == in_h);
            for (size_t t = 0; t < latest.size(); t += 4) {
                for (int c = 0; c < 3; ++c) {
                    const float v = HalfToFloat(latest[t + c]);
                    if (!std::isfinite(v)) s.finite = false;
                    const float a = std::fabs(v);
                    sum += a;
                    if (a > 1e-4f) s.non_black = true;
                    if (std::fabs(v - first) > 1e-4f) s.spatially_varying = true;
                    if (same_size) diff += std::fabs(v - HalfToFloat(color.texels[t + c]));
                }
            }
            const double n = double(latest.size() / 4 * 3);
            s.mean_absolute_rgb = float(sum / n);
            s.mean_absolute_difference_from_input = same_size ? float(diff / n) : 0.0f;
            report->evaluations.push_back(s);
        }
    }

    out->width = out_w;
    out->height = out_h;
    out->texels = std::move(latest);
    return true;
}

std::vector<AdapterInfo> ListAdapters() {
    std::vector<AdapterInfo> out;
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return out;

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByGpuPreference(
                i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;

        AdapterInfo info;
        info.name = desc.Description;
        info.dedicated_vram = desc.DedicatedVideoMemory;
        info.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        out.push_back(std::move(info));
        if (i > 15) break;  // paranoia; no real machine has this many
    }
    return out;
}

}  // namespace dlssnr
