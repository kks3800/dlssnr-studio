# Builds the Release configuration and assembles the release zip in dist/.
#
#   pwsh -File scripts/package.ps1              configure, build, package
#   pwsh -File scripts/package.ps1 -SkipBuild   package what build/Release has
#
# The zip deliberately contains no DLLs and no models. nvngx_dlssnr.dll is not
# ours to distribute, and ONNX Runtime, DirectML and the two ONNX models are
# fetched on the user's machine by get-runtime.ps1, which ships in the zip.
# What goes in:
#
#   dlssnr-studio.exe  dlssnr-image.exe  README.md  LICENSE
#   START-HERE.txt     get-runtime.ps1   tools/fatbin_walk.py
#
# The script refuses to zip if any other binary has crept into the staging
# folder, so a stray copy of the NVIDIA runtime cannot ship by accident.

param(
    [switch]$SkipBuild,
    [string]$Generator = 'Visual Studio 17 2022'
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$dist  = Join-Path $root 'dist'
$stage = Join-Path $dist 'dlssnr-studio'
$rel   = Join-Path $build 'Release'

if (-not $SkipBuild) {
    Write-Host '== configure'
    & cmake -S $root -B $build -G $Generator -A x64
    if ($LASTEXITCODE) { throw "cmake configure failed ($LASTEXITCODE)" }
    Write-Host '== build Release'
    & cmake --build $build --config Release --parallel
    if ($LASTEXITCODE) { throw "cmake build failed ($LASTEXITCODE)" }
}

foreach ($exe in 'dlssnr-studio.exe', 'dlssnr-image.exe') {
    if (-not (Test-Path (Join-Path $rel $exe))) { throw "missing $rel\$exe -- build first" }
}

Write-Host '== stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'tools') | Out-Null

Copy-Item (Join-Path $rel  'dlssnr-studio.exe')         $stage
Copy-Item (Join-Path $rel  'dlssnr-image.exe')          $stage
Copy-Item (Join-Path $root 'README.md')                 $stage
Copy-Item (Join-Path $root 'LICENSE')                   $stage
Copy-Item (Join-Path $root 'packaging/START-HERE.txt')  $stage
Copy-Item (Join-Path $root 'packaging/get-runtime.ps1') $stage
Copy-Item (Join-Path $root 'tools/fatbin_walk.py')      (Join-Path $stage 'tools')

# Guard: only our two executables may be binary. Everything else that could be
# sitting in build/Release -- the NVIDIA runtime above all -- stays out.
$allowed = @('dlssnr-studio.exe', 'dlssnr-image.exe')
$binary  = @('.dll', '.exe', '.onnx', '.lib', '.pdb')
$stray = Get-ChildItem $stage -Recurse -File |
         Where-Object { ($_.Extension -in $binary) -and ($_.Name -notin $allowed) }
if ($stray) { throw "refusing to package: $($stray.Name -join ', ')" }

Write-Host '== zip'
New-Item -ItemType Directory -Force $dist | Out-Null
$zip = Join-Path $dist 'dlssnr-studio-win-x64.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal

Get-ChildItem $stage -Recurse -File | ForEach-Object {
    Write-Host ("  {0,9:n0}  {1}" -f $_.Length, $_.FullName.Substring($stage.Length + 1))
}
Write-Host ("wrote {0} ({1:n1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
