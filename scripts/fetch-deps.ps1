# Fetches everything the build needs except the NVIDIA runtime itself.
#
# Nothing here is committed to the repo: ONNX Runtime is ~300 MB and the two
# ONNX models are another ~160 MB. Run this once after cloning.
#
#   pwsh -File scripts/fetch-deps.ps1
#
# You must supply nvngx_dlssnr.dll yourself -- see README.md.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ext  = Join-Path $root 'external'

function Get-File($url, $dest) {
    if (Test-Path $dest) {
        Write-Host "  have    $(Split-Path -Leaf $dest)"
        return
    }
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Write-Host "  fetch   $(Split-Path -Leaf $dest)"
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
}

# ---- Dear ImGui (GUI) ----
Write-Host 'Dear ImGui'
$imguiBase = 'https://raw.githubusercontent.com/ocornut/imgui/master'
foreach ($f in 'imgui.h','imgui.cpp','imgui_internal.h','imgui_draw.cpp',
               'imgui_tables.cpp','imgui_widgets.cpp','imconfig.h',
               'imstb_rectpack.h','imstb_textedit.h','imstb_truetype.h') {
    Get-File "$imguiBase/$f" (Join-Path $ext "imgui/$f")
}
foreach ($f in 'imgui_impl_win32.h','imgui_impl_win32.cpp',
               'imgui_impl_dx12.h','imgui_impl_dx12.cpp') {
    Get-File "$imguiBase/backends/$f" (Join-Path $ext "imgui/backends/$f")
}

# ---- NVIDIA NGX headers (public DLSS SDK) ----
# Only for enums and struct layouts; the feature itself is resolved at runtime.
Write-Host 'NGX headers'
$ngxBase = 'https://raw.githubusercontent.com/NVIDIA/DLSS/main/include'
foreach ($f in 'nvsdk_ngx.h','nvsdk_ngx_defs.h','nvsdk_ngx_params.h','nvsdk_ngx_helpers.h') {
    Get-File "$ngxBase/$f" (Join-Path $ext "ngx/$f")
}

# ---- tinyexr (reads engine AOV passes) ----
# Needed because WIC cannot read EXR, and velocity is signed while depth needs
# float range -- neither survives an integer PNG.
Write-Host 'tinyexr'
$exrBase = 'https://raw.githubusercontent.com/syoyo/tinyexr/v1.0.8'
Get-File "$exrBase/tinyexr.h"             (Join-Path $ext 'tinyexr/tinyexr.h')
Get-File "$exrBase/deps/miniz/miniz.h"    (Join-Path $ext 'tinyexr/miniz.h')
Get-File "$exrBase/deps/miniz/miniz.c"    (Join-Path $ext 'tinyexr/miniz.c')

# ---- ONNX Runtime, DirectML build (depth + optical flow) ----
# The DirectML build rather than the plain CPU one: RAFT optical flow runs
# ~900 ms a frame on the CPU provider and ~65 ms on the GPU, which on a long
# clip is the difference between an afternoon and an hour. DirectML rather than
# CUDA because this is already a D3D12 program -- no CUDA or cuDNN runtime to
# ship, and it works on any DX12 GPU.
#
# Both come from NuGet, which serves .nupkg files that are ordinary zips.
Write-Host 'ONNX Runtime 1.20.1 (DirectML)'
$ortDir = Join-Path $ext 'onnxruntime/onnxruntime-win-x64-dml-1.20.1'
if (-not (Test-Path (Join-Path $ortDir 'lib/DirectML.dll'))) {
    $staging = Join-Path $ext 'onnxruntime/_staging'
    New-Item -ItemType Directory -Force -Path $staging, (Join-Path $ortDir 'lib'), (Join-Path $ortDir 'include') | Out-Null

    $ortPkg = Join-Path $staging 'ort-dml.zip'
    Get-File 'https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1' $ortPkg
    Write-Host '  expand  onnxruntime.directml'
    Expand-Archive -Path $ortPkg -DestinationPath (Join-Path $staging 'ort') -Force
    Copy-Item (Join-Path $staging 'ort/runtimes/win-x64/native/onnxruntime.dll') (Join-Path $ortDir 'lib') -Force
    Copy-Item (Join-Path $staging 'ort/runtimes/win-x64/native/onnxruntime.lib') (Join-Path $ortDir 'lib') -Force
    Copy-Item (Join-Path $staging 'ort/build/native/include/*.h')                (Join-Path $ortDir 'include') -Force

    # The ORT package carries the provider header but not DirectML.dll itself.
    $dmlPkg = Join-Path $staging 'directml.zip'
    Get-File 'https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.2' $dmlPkg
    Write-Host '  expand  DirectML'
    Expand-Archive -Path $dmlPkg -DestinationPath (Join-Path $staging 'dml') -Force
    Copy-Item (Join-Path $staging 'dml/bin/x64-win/DirectML.dll') (Join-Path $ortDir 'lib') -Force

    Remove-Item $staging -Recurse -Force
} else {
    Write-Host '  have    onnxruntime.directml'
}

# ---- Models ----
# Both are optional: the tool runs without them, you just lose --estimate-depth
# and --estimate-motion.
Write-Host 'Models'
Get-File 'https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model.onnx' `
         (Join-Path $ext 'models/depth-anything-v2-small.onnx')
Get-File 'https://github.com/opencv/opencv_zoo/raw/281d232cd99cd920853106d853c440edd35eb442/models/optical_flow_estimation_raft/optical_flow_estimation_raft_2023aug.onnx' `
         (Join-Path $ext 'models/raft.onnx')

Write-Host ''
Write-Host 'Done. Next:'
Write-Host '  cmake -S . -B build -G "Visual Studio 17 2022" -A x64'
Write-Host '  cmake --build build --config Release'
Write-Host ''
Write-Host 'Then copy nvngx_dlssnr.dll into build/Release/ -- see README.md.'
