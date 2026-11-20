# Builds the NFS World port and packages it for distribution in dist\:
#
#   dist\FRSWorldEditor-NFSW-<version>\     what a player unzips anywhere
#       FRSWorldEditor.exe                  the editor; nothing is installed into NFS World
#       game\scripts\editor-fast-test.asi   for Launch Game: copied into the game by hand
#       README.txt, LICENSE.txt
#   dist\FRSWorldEditor-NFSW-<version>.zip  the same, zipped
#
#   .\package-nfsw.ps1
#   .\package-nfsw.ps1 -Version 0.2.7-nfsw.2
#
# Built in a folder of its own (build-release), without extensions and without
# New Region / Import Region, as the original's package.ps1 does. The
# Underground 2 plugins are not in it: that package is the original's.

# -MinHook <folder>: a copy of MinHook 1.3.4 on disk, instead of downloading it.
param([string]$Version = "0.2.7-nfsw.2", [string]$MinHook)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$name = "FRSWorldEditor-NFSW-$Version"
$stage = Join-Path $root "dist\$name"

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
"[..] building the editor"
& $cmake -S $root -B (Join-Path $root "build-release") -G "Visual Studio 18 2026" -A x64 -DFRS_WITH_EXTENSIONS=OFF -DFRS_WITH_REGIONS=OFF | Out-Null
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
& $cmake --build (Join-Path $root "build-release") --config Release | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build failed" }

"[..] building editor-fast-test.asi (Win32, like the game)"
$fast = Join-Path $root "fasttest-nfsw"
$options = @()
if ($MinHook) { $options += "-DFETCHCONTENT_SOURCE_DIR_MINHOOK=$($MinHook -replace '\\', '/')" }
& $cmake -S $fast -B (Join-Path $fast "build") -G "Visual Studio 18 2026" -A Win32 -Wno-dev -Wno-deprecated @options | Out-Null
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for fasttest-nfsw" }
& $cmake --build (Join-Path $fast "build") --config Release | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build or self-test failed for fasttest-nfsw" }
$minhookLicense = if ($MinHook) { Join-Path $MinHook "LICENSE.txt" } else { Join-Path $fast "build\_deps\minhook-src\LICENSE.txt" }

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage "game\scripts") | Out-Null
Copy-Item (Join-Path $root "build-release\bin\Release\FRSWorldEditor.exe") $stage
Copy-Item (Join-Path $fast "build\Release\editor-fast-test.asi") (Join-Path $stage "game\scripts")

$readme = @(
    "FRS World Editor -- NFS World port $Version",
    "",
    "A fork of FRS World Editor by Chrystian Farias (FRS Mods) that opens Need for",
    "Speed World's map and writes your edits into the map's own files.",
    "",
    "  This port:     https://github.com/letsrock-gf/FRSWorldEditor",
    "  The original:  https://github.com/chrystianfarias/FRSWorldEditor",
    "",
    "1. Back up your game's TRACKS folder. This port is still being tested.",
    "2. Close the game and run FRSWorldEditor.exe.",
    "3. On the setup's `"Your games`" page, give the folder that holds nfsw.exe in the",
    "   Need for Speed World row. Nothing is installed into the game.",
    "4. The map takes about half a minute to load.",
    "5. Ctrl+Shift+S writes your edits into the game. The shipped files are kept in",
    "   TRACKS\frs-original.",
    "",
    "Launch Game / Fast test (optional) start your game from the editor, pass the",
    "title and the driver select and put the car at the editor's camera. They need:",
    "  a. an ASI loader in the game folder (for example Ultimate ASI Loader);",
    "  b. game\scripts\editor-fast-test.asi of this package copied to the game's",
    "     scripts folder;",
    "  c. a file named editor-launch.cmd in the game folder, holding the command",
    "     that starts your game: your launcher, or nfsw.exe with your server's",
    "     arguments.",
    "",
    "What works, what was tested and what does not work yet is on the port's page.",
    "For Need for Speed Underground 2, use the original: this package does not carry",
    "its plugins.",
    "",
    "Fan project, not affiliated with or endorsed by Electronic Arts. No game file",
    "is in this package."
)
($readme -join "`r`n") + "`r`n" | Set-Content (Join-Path $stage "README.txt") -Encoding utf8 -NoNewline

$rule = "=" * 76
$license = @(
    "FRS World Editor -- NFS World port $Version",
    "",
    "This package holds works under four licenses.",
    "",
    $rule,
    "1. FRS World Editor (FRSWorldEditor.exe), by Chrystian Farias",
    "   https://github.com/chrystianfarias/FRSWorldEditor",
    "   with the NFS World port by letsrock-gf, a modified version of it",
    "   https://github.com/letsrock-gf/FRSWorldEditor",
    "   Creative Commons Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)",
    "   You may share and adapt them, with credit, for non-commercial purposes",
    "   only: they may not be sold.",
    $rule,
    "",
    (Get-Content (Join-Path $root "LICENSE") -Raw).TrimEnd(),
    "",
    $rule,
    "2. Win32 property grid, by David MacDermot, compiled into FRSWorldEditor.exe",
    "   The Code Project Open License (CPOL) 1.02",
    $rule,
    "",
    (Get-Content (Join-Path $root "third_party\propertygrid\LICENSE") -Raw).TrimEnd(),
    "",
    $rule,
    "3. Dear ImGui, by Omar Cornut and contributors, compiled into FRSWorldEditor.exe -- MIT",
    $rule,
    "",
    (Get-Content (Join-Path $root "third_party\imgui\LICENSE.txt") -Raw).TrimEnd(),
    "",
    $rule,
    "4. Phosphor Icons, compiled into FRSWorldEditor.exe -- MIT",
    $rule,
    "",
    (Get-Content (Join-Path $root "third_party\phosphor\LICENSE") -Raw).TrimEnd(),
    "",
    $rule,
    "5. MinHook, by Tsuda Kageyu, compiled into game\scripts\editor-fast-test.asi",
    "   BSD 2-Clause",
    $rule,
    "",
    (Get-Content $minhookLicense -Raw).TrimEnd()
)
($license -join "`r`n") + "`r`n" | Set-Content (Join-Path $stage "LICENSE.txt") -Encoding utf8 -NoNewline

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
"[ok] $zip"
Get-ChildItem $stage -Recurse -File | ForEach-Object { "     {0}  {1}" -f $_.FullName.Substring($stage.Length + 1), $_.Length }
