# Builds tracy-roblox-bridge.exe with the MSVC Build Tools (no Visual Studio IDE needed).
#   powershell -ExecutionPolicy Bypass -File bridge\build.ps1
# Output: build\tracy-roblox-bridge.exe
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# Prefer VS 2022 in its standard folders (vswhere doesn't always list Build Tools),
# then whatever vswhere knows about.
$candidates = @()
foreach ($base in @("${env:ProgramFiles}\Microsoft Visual Studio\2022", "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022")) {
    $candidates += Get-ChildItem -Path "$base\*\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue | ForEach-Object FullName
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $candidates += & $vswhere -products * -sort -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        ForEach-Object { Join-Path $_ "VC\Auxiliary\Build\vcvars64.bat" }
}
$vcvars = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw "No MSVC x64 toolchain found. Install Visual Studio 2022 Build Tools with the C++ workload." }
Write-Host "using $vcvars"
$src = Join-Path $root "bridge"
$out = Join-Path $root "build"
cmd /c "`"$vcvars`" >nul && cmake -S `"$src`" -B `"$out`" -G `"NMake Makefiles`" -DCMAKE_BUILD_TYPE=Release && cmake --build `"$out`""
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "built $out\tracy-roblox-bridge.exe"
