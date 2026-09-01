# Fetches the redistributable runtime pieces the release zip leaves out and
# puts them beside this script, which sits beside the executables:
#
#   onnxruntime.dll                 ONNX Runtime 1.20.1, DirectML build (Microsoft)
#   DirectML.dll                    DirectML 1.15.2 (Microsoft)
#   depth-anything-v2-small.onnx    for --estimate-depth   (optional, ~95 MB)
#   raft.onnx                       for --estimate-motion  (optional, ~62 MB)
#
# Nothing here is NVIDIA's. nvngx_dlssnr.dll is still yours to supply -- see
# START-HERE.txt.
#
#   powershell -ExecutionPolicy Bypass -File get-runtime.ps1             everything
#   powershell -ExecutionPolicy Bypass -File get-runtime.ps1 -NoModels   just the DLLs
#
# Runs on the PowerShell 5.1 that ships with Windows; pwsh works too. The
# versions are the ones scripts/fetch-deps.ps1 builds against; change both
# together.

param(
    [switch]$NoModels,
    [string]$Dest = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Get-File($url, $dest) {
    if (Test-Path $dest) {
        Write-Host "  have    $(Split-Path -Leaf $dest)"
        return
    }
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Write-Host "  fetch   $(Split-Path -Leaf $dest)"
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
}

$staging = Join-Path $Dest '_runtime_staging'

# ---- ONNX Runtime (DirectML build) + DirectML ----
# Both come from NuGet, which serves .nupkg files that are ordinary zips. The
# ORT package carries the DirectML provider but not DirectML.dll itself.
Write-Host 'ONNX Runtime 1.20.1 (DirectML) and DirectML 1.15.2'
$needOrt = -not (Test-Path (Join-Path $Dest 'onnxruntime.dll'))
$needDml = -not (Test-Path (Join-Path $Dest 'DirectML.dll'))
if ($needOrt -or $needDml) {
    New-Item -ItemType Directory -Force $staging | Out-Null
    if ($needOrt) {
        $pkg = Join-Path $staging 'ort-dml.zip'
        Get-File 'https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.20.1' $pkg
        Expand-Archive -Path $pkg -DestinationPath (Join-Path $staging 'ort') -Force
        Copy-Item (Join-Path $staging 'ort/runtimes/win-x64/native/onnxruntime.dll') $Dest -Force
        Write-Host '  placed  onnxruntime.dll'
    } else {
        Write-Host '  have    onnxruntime.dll'
    }
    if ($needDml) {
        $pkg = Join-Path $staging 'directml.zip'
        Get-File 'https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.2' $pkg
        Expand-Archive -Path $pkg -DestinationPath (Join-Path $staging 'dml') -Force
        Copy-Item (Join-Path $staging 'dml/bin/x64-win/DirectML.dll') $Dest -Force
        Write-Host '  placed  DirectML.dll'
    } else {
        Write-Host '  have    DirectML.dll'
    }
    Remove-Item $staging -Recurse -Force
} else {
    Write-Host '  have    onnxruntime.dll'
    Write-Host '  have    DirectML.dll'
}

# ---- Models ----
# Both are optional: without them the tool runs, you just lose --estimate-depth
# and --estimate-motion (the GUI needs raft.onnx to render video with flow).
if ($NoModels) {
    Write-Host 'Models     skipped (-NoModels)'
} else {
    Write-Host 'Models'
    Get-File 'https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model.onnx' `
             (Join-Path $Dest 'depth-anything-v2-small.onnx')
    Get-File 'https://github.com/opencv/opencv_zoo/raw/281d232cd99cd920853106d853c440edd35eb442/models/optical_flow_estimation_raft/optical_flow_estimation_raft_2023aug.onnx' `
             (Join-Path $Dest 'raft.onnx')
}

Write-Host ''
if (Test-Path (Join-Path $Dest 'nvngx_dlssnr.dll')) {
    Write-Host 'Done. nvngx_dlssnr.dll is present; run dlssnr-studio.exe.'
} else {
    Write-Host 'Done. Now copy nvngx_dlssnr.dll into this folder -- see START-HERE.txt.'
}
