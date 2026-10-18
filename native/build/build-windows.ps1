# Builds runtimes/windows/aqua_filament.dll on Windows with Visual Studio 2022 or newer.
#   .\native\build\build-windows.ps1 -FilamentDir C:\Libraries\filament-v1.78.0-windows
param([Parameter(Mandatory = $true)][string]$FilamentDir, [string]$Generator = "Visual Studio 17 2022")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$build = Join-Path $root "native\build\msvc"
cmake -S (Join-Path $root "native") -B $build -G $Generator -A x64 "-DFILAMENT_DIR=$FilamentDir"
cmake --build $build --config Release
# Ship the VC++ runtime next to the DLL so applications need no separate redistributable installer.
$redist = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
if ($redist) {
  foreach ($dll in "msvcp140.dll", "msvcp140_codecvt_ids.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
    Copy-Item (Join-Path $redist.FullName $dll) (Join-Path $root "runtimes\windows") -Force
  }
}
Write-Host "Built $(Join-Path $root 'runtimes\windows\aqua_filament.dll')"
