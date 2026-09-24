# ===========================================================================
# QtPlayer deploy script (Windows / MSVC)
#   configure + build Release + cmake --install + dependency self-check
#
# NOTE: this file is deliberately ASCII-only. PowerShell 5.1 reads a BOM-less
# file using the ANSI codepage, so non-ASCII comments can swallow the following
# ASCII bytes and corrupt the script (that is exactly how the first version of
# this file broke: $MyInvocation.MyCommand.Path turned into null and Split-Path
# complained "cannot bind argument to parameter 'Path' because it is null").
#
# Usage (PowerShell):
#     cd D:\hilihili\CicadaPlayerNext\platform\QtPlayer
#     .\deploy_win.ps1                      # dynamic Qt -> .\deploy
#     .\deploy_win.ps1 -DeployDir out       # deploy into .\out
#     .\deploy_win.ps1 -Static              # static Qt  -> .\deploy-static
#     .\deploy_win.ps1 -QtPrefix D:\Qt-Static\msvc
#
# The same thing through the .bat (what most people run):
#     deploy_win.bat                         # dynamic Qt
#     deploy_win.bat static                  # static Qt, prefix from QT_STATIC_DIR
#     deploy_win.bat static D:\Qt-Static\msvc
#
# STATIC vs DYNAMIC Qt
# --------------------
#   * The linking mode follows the Qt PREFIX that gets picked -- this script hands
#     it to CMake as -DCICADA_QT_STATIC=ON/OFF, so you never pass that flag by hand.
#   * A prefix counts as static when it has NO bin\Qt6Core.dll (a static Qt install
#     ships .lib files and host tools only, no Qt runtime DLLs).
#   * The two modes get SEPARATE build and deploy directories on purpose. A shared
#     and a static build must never share one CMake cache: the MSVC runtime library
#     (/MD vs /MT) and the plugin set differ, and a stale cache produces link errors
#     that look like source problems.
#
# STATIC MODE: WHERE THE STATIC Qt COMES FROM
# -------------------------------------------
#   The static install location is read from the environment by default -- that is
#   the normal way to use it (set it once, then just run "deploy_win.bat static"):
#
#       cmd:         setx QT_STATIC_DIR D:\Qt-Static\msvc      (new shells)
#                    set  QT_STATIC_DIR D:\Qt-Static\msvc      (this shell only)
#       PowerShell:  $env:QT_STATIC_DIR = "D:\Qt-Static\msvc"
#
#   It can also be passed in per run, in which case the argument wins:
#
#       deploy_win.bat static D:\Qt-Static\msvc        (the .bat exports it for the
#                                                      child process only)
#       .\deploy_win.ps1 -Static -QtPrefix D:\Qt-Static\msvc
#
#   $env:CICADA_QT_STATIC_DIR is still accepted as an alias of QT_STATIC_DIR.
#   If a variable IS set but does not point at a usable Qt prefix, this script stops
#   with a clear message instead of quietly building with some other Qt.
#   -Static never falls back to a shared Qt: "-Static" that silently produced a shared
#   build inside build\msvc-static is exactly the trap this script must not have.
#
# Qt is found automatically, in this order:
#     1. -QtPrefix                                  (explicit, wins over everything)
#     2. -Static: $env:QT_STATIC_DIR, then $env:CICADA_QT_STATIC_DIR
#        otherwise: $env:QTDIR / $env:QT_DIR / $env:QT_ROOT_DIR
#     3. windeployqt / qmake already on PATH        (shared search only)
#     4. -Static: <drive>:\Qt-Static\msvc
#        otherwise: <drive>:\Qt\<version>\<kit>     (online installer, newest first)
# So normally you do not have to set anything; if it picks the wrong Qt, set
# QTDIR explicitly. vcpkg is found by CMakeLists.txt itself ($env:VCPKG_ROOT).
#
# The static Qt install location is looked up in the ENVIRONMENT by default
# (QT_STATIC_DIR) and can also be handed in: to the .bat as
#     deploy_win.bat static D:\Qt-Static\msvc
# or to this script as -QtPrefix. See the STATIC MODE section below.
#
# If script execution is blocked by policy:
#     powershell -NoProfile -ExecutionPolicy Bypass -File .\deploy_win.ps1
#
# Why the logic is here and not in the .bat: there IS a thin deploy_win.bat next to
# this file, but it only maps "static"/"shared" onto the switches below and forwards
# everything else. The work done here (walking drives for a Qt prefix, sorting kit
# directories by version, deriving the linking mode, choosing per-mode directories,
# the dependency self-check) is real scripting that batch cannot express without
# becoming unreadable. Two copies of it would drift apart, so there is exactly one.
#
# Line endings: this file is LF (like the rest of the repository) and PowerShell is
# happy with that. The .bat is written with CRLF because that is batch's native
# format -- a test on Windows 11 / cmd showed that multi-line LF batch files
# actually work too (goto labels and ( ) blocks included), but CRLF removes the
# question entirely and costs nothing.
#
# ASCII-only is NOT about line endings: PowerShell 5.1 reads a BOM-less file using
# the ANSI codepage, so non-ASCII comments can swallow the following ASCII bytes and
# corrupt the script. Keep every byte of this file below 128.
#
# What it does:
#   1. configure (always: an existing CMakeCache.txt does not prove the directory
#      is usable, see the note in step 1 below);
#   2. build Release;
#   3. cmake --install into the deploy directory -- Qt DLLs, the *qml/ modules*
#      and the platform plugins are installed by CMakeLists.txt install rules
#      (qt_generate_deploy_app_script). A bare windeployqt never copies qml/,
#      which is one of the reasons a deployed build exits immediately;
#   4. check that the files it needs are actually there.
#
# Everything is discovered through environment variables, no hardcoded paths:
#     Qt    -> $env:QTDIR (or $env:QT_DIR)
#     vcpkg -> $env:VCPKG_ROOT (CMakeLists.txt finds installed/<triplet> itself)
# ===========================================================================

[CmdletBinding()]
param(
    [string]$DeployDir = "",
    # Static Qt build: the prefix is taken from $env:QT_STATIC_DIR (or
    # $env:CICADA_QT_STATIC_DIR), else from <drive>:\Qt-Static\msvc; it also moves the
    # build/deploy directories so a shared build is not disturbed. It does NOT fall
    # back to a shared Qt -- no static Qt is an error, not a silent shared build.
    [switch]$Static,
    # Explicit Qt prefix. Overrides every automatic search (and the linking mode is
    # then derived from the prefix itself, not from -Static).
    [string]$QtPrefix = "",
    # Print the usage and stop. "-h" works too (PowerShell resolves it as an
    # unambiguous abbreviation of -Help).
    [switch]$Help
)

if ($Help) {
    Write-Host "Usage: deploy_win.ps1 [-Static] [-QtPrefix DIR] [-DeployDir DIR]"
    Write-Host ""
    Write-Host "  (no switch)        dynamic Qt  ->  build\msvc         ->  deploy\"
    Write-Host "  -Static            static Qt   ->  build\msvc-static  ->  deploy-static\"
    Write-Host "  -QtPrefix DIR      use this Qt prefix (the linking mode follows it)"
    Write-Host "  -DeployDir DIR     install into this directory instead of the default"
    Write-Host ""
    Write-Host "The script configures, builds Release, installs and then self-checks the"
    Write-Host "result. A Qt prefix counts as static when it has no bin\Qt6Core.dll."
    Write-Host ""
    Write-Host "Qt search order: -QtPrefix, QTDIR/QT_DIR/QT_ROOT_DIR, PATH, then"
    Write-Host "  otherwise: <drive>:\Qt\<version>\<kit>  (newest first)"
    Write-Host "Static Qt is looked up separately (and never falls back to a shared Qt):"
    Write-Host "  -QtPrefix, QT_STATIC_DIR (alias: CICADA_QT_STATIC_DIR), then"
    Write-Host "  <drive>:\Qt-Static\msvc"
    Write-Host "  e.g.  set QT_STATIC_DIR=D:\Qt-Static\msvc   then   deploy_win.bat static"
    Write-Host "        deploy_win.bat static D:\Qt-Static\msvc"
    Write-Host "See README.md section 3.8 for the full story."
    exit 0
}

$ErrorActionPreference = "Stop"

# $PSCommandPath is the reliable way to get this script's own path; fall back to
# $MyInvocation if it is somehow empty, and fail with a readable message instead
# of letting Split-Path throw a cryptic parameter binding error.
$scriptPath = $PSCommandPath
if ([string]::IsNullOrWhiteSpace($scriptPath)) { $scriptPath = $MyInvocation.MyCommand.Definition }
if ([string]::IsNullOrWhiteSpace($scriptPath) -or -not (Test-Path -LiteralPath $scriptPath)) {
    Write-Host "[ERROR] cannot determine the script path; run it as .\deploy_win.ps1" -ForegroundColor Red
    exit 1
}

$srcDir   = Split-Path -Parent $scriptPath
# $buildDir and $DeployDir are decided AFTER the Qt prefix is known (see below):
# which Qt gets picked decides both the linking mode and therefore the directory
# names, so computing them here would guess wrong half the time.

# ---- locate Qt -------------------------------------------------------------
# Same idea as the vcpkg lookup in CMakeLists.txt: honour the environment first,
# then look around this machine. Nothing is hardcoded to one machine's layout,
# and whatever is picked is printed so a wrong pick is easy to spot.
# A usable Qt prefix must contain BOTH the deploy tool and the CMake package that
# find_package(Qt6) looks for. Checking only windeployqt.exe is how a bogus
# prefix once slipped through (a single-element array indexed with [0] returned
# the first *character* of the path, so CMAKE_PREFIX_PATH ended up as "D").
function Test-QtPrefix([string]$prefix) {
    if ([string]::IsNullOrWhiteSpace($prefix)) { return $false }
    return ((Test-Path -LiteralPath (Join-Path $prefix "bin\windeployqt.exe")) -and
            (Test-Path -LiteralPath (Join-Path $prefix "lib\cmake\Qt6\Qt6Config.cmake")))
}

# A static Qt prefix has no Qt runtime DLLs at all -- that is the cheapest reliable
# signal. (Both kinds ship bin\windeployqt.exe and lib\cmake\Qt6\Qt6Config.cmake, so
# Test-QtPrefix cannot tell them apart.)
function Test-QtPrefixStatic([string]$prefix) {
    if ([string]::IsNullOrWhiteSpace($prefix)) { return $false }
    return (-not (Test-Path -LiteralPath (Join-Path $prefix "bin\Qt6Core.dll")))
}

# The static Qt prefix: ENVIRONMENT FIRST (that is the documented default), then the
# conventional <drive>:\Qt-Static\msvc guess. The shared search below is deliberately
# NOT used as a fallback -- "-Static" that quietly produced a shared build inside
# build\msvc-static is exactly the surprise this script must not have. If nothing is
# found the caller exits with instructions instead of guessing.
#
# A variable that IS set but does not point at a usable prefix is a hard error: the
# user said "use this Qt", so silently building with another one would be worse.
function Find-StaticQtPrefix {
    foreach ($name in @("QT_STATIC_DIR", "CICADA_QT_STATIC_DIR")) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ([string]::IsNullOrWhiteSpace($value)) { continue }

        if (-not (Test-QtPrefix $value)) {
            Write-Host ("[ERROR] {0} is set but is not a usable Qt prefix:" -f $name) -ForegroundColor Red
            Write-Host ("        {0}" -f $value)
            Write-Host "        a static Qt prefix must contain bin\windeployqt.exe and"
            Write-Host "        lib\cmake\Qt6\Qt6Config.cmake (and must NOT contain bin\Qt6Core.dll)"
            exit 1
        }

        Write-Host ("[info] static Qt from {0}" -f $name)
        return $value
    }

    foreach ($drive in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue).Root) {
        $staticCandidate = Join-Path $drive "Qt-Static\msvc"
        if (Test-QtPrefix $staticCandidate) {
            Write-Host ("[info] static Qt from the default guess {0}" -f $staticCandidate)
            return $staticCandidate
        }
    }

    return $null
}

# The shared Qt: environment first, then PATH, then the online installer's layout.
function Find-QtPrefix {
    # 1. environment variables (the documented way)
    foreach ($name in @("QTDIR", "QT_DIR", "QT_ROOT_DIR")) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if (Test-QtPrefix $value) { return $value }
    }

    # 2. anything Qt-ish already on PATH
    foreach ($exe in @("windeployqt.exe", "qmake.exe", "qmake6.exe")) {
        $cmd = Get-Command $exe -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source) {
            $prefix = Split-Path -Parent (Split-Path -Parent $cmd.Source)   # ...\<kit>\bin\x.exe
            if (Test-QtPrefix $prefix) { return $prefix }
        }
    }

    # 3. the usual install root of the online installer: <drive>:\Qt\<version>\<kit>
    $candidates = @()
    foreach ($drive in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue).Root) {
        $root = Join-Path $drive "Qt"
        if (-not (Test-Path -LiteralPath $root)) { continue }
        foreach ($versionDir in (Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue)) {
            foreach ($kitDir in (Get-ChildItem -LiteralPath $versionDir.FullName -Directory -ErrorAction SilentlyContinue)) {
                if (Test-QtPrefix $kitDir.FullName) {
                    $candidates += $kitDir.FullName
                }
            }
        }
    }
    if ($candidates.Count -gt 0) {
        # newest Qt first; sort by the version directory name parsed as a version
        # (plain string sort would rank 6.5 above 6.11). "Select-Object -First 1"
        # instead of [0]: indexing a single string would give back a character.
        return ($candidates | Sort-Object -Descending {
            try { [version](Split-Path -Leaf (Split-Path -Parent $_)) } catch { [version]"0.0.0" }
        } | Select-Object -First 1)
    }

    return $null
}

$qt = ""
$qtFrom = ""

if (-not [string]::IsNullOrWhiteSpace($QtPrefix)) {
    # An explicit prefix wins over every search (and over the environment). Validate it
    # here so a typo fails with a readable message instead of a confusing CMake error
    # much later.
    if (-not (Test-QtPrefix $QtPrefix)) {
        Write-Host ("[ERROR] -QtPrefix is not a usable Qt prefix: {0}" -f $QtPrefix) -ForegroundColor Red
        Write-Host "        it must contain bin\windeployqt.exe and lib\cmake\Qt6\Qt6Config.cmake"
        exit 1
    }
    $qt = $QtPrefix
    $qtFrom = "-QtPrefix"
} elseif ($Static) {
    $qt = Find-StaticQtPrefix
    $qtFrom = "static search"
} else {
    $qt = Find-QtPrefix
    $qtFrom = "shared search"
}

if ([string]::IsNullOrWhiteSpace($qt)) {
    if ($Static) {
        # Reached when no static Qt exists anywhere: neither QT_STATIC_DIR nor the
        # default <drive>:\Qt-Static\msvc guess. Do NOT fall back to a shared Qt here.
        Write-Host "[ERROR] -Static was requested but no static Qt was found." -ForegroundColor Red
        Write-Host ""
        Write-Host "        Point the script at the static install -- either set the variable"
        Write-Host "        (this is the documented default):"
        Write-Host '            setx QT_STATIC_DIR D:\Qt-Static\msvc        (cmd, new shells)'
        Write-Host '            $env:QT_STATIC_DIR = "D:\Qt-Static\msvc"    (PowerShell)'
        Write-Host "        or pass it in for this run:"
        Write-Host "            deploy_win.bat static D:\Qt-Static\msvc"
        Write-Host "            .\deploy_win.ps1 -Static -QtPrefix D:\Qt-Static\msvc"
        Write-Host ""
        Write-Host "        A static Qt prefix has no bin\Qt6Core.dll. Building a static Qt from"
        Write-Host "        source is a separate job (see README.md section 3.8); a shared Qt is"
        Write-Host "        NOT used here on purpose -- drop -Static for the shared build."
    } else {
        Write-Host "[ERROR] no Qt found. Either set QTDIR:" -ForegroundColor Red
        Write-Host '        $env:QTDIR = "D:\Qt\6.11.1\msvc2022_64"'
        Write-Host "        or put <Qt>\bin on PATH, or install Qt under <drive>:\Qt\<version>\<kit>"
        Write-Host "        for a static Qt also try: .\deploy_win.ps1 -Static"
        Write-Host "        or: .\deploy_win.ps1 -QtPrefix D:\Qt-Static\msvc"
    }
    exit 1
}

# The linking mode follows the PREFIX, not the -Static switch: with -QtPrefix the
# switch cannot override the prefix (the prefix is the fact, the switch is a hint).
$isStatic = Test-QtPrefixStatic $qt

# ... but say so out loud when the two disagree, because "I asked for static and got a
# shared build" would otherwise only show up as a [shared] tag in the line below.
if ($Static -and -not $isStatic) {
    Write-Host ("[WARN] -Static was given, but {0} is a SHARED Qt (it has bin\Qt6Core.dll)." -f $qt) -ForegroundColor Yellow
    Write-Host "       The prefix wins: building a shared build. Drop -QtPrefix to let the"
    Write-Host "       static search (QT_STATIC_DIR) pick the static Qt instead."
}
if (-not $Static -and $isStatic -and $qtFrom -eq "-QtPrefix") {
    Write-Host ("[info] -QtPrefix points at a static Qt, so this is a static build.")
}

$staticFlag = "OFF"
if ($isStatic) { $staticFlag = "ON" }

# ---- build / deploy directories -------------------------------------------
# Separate directories per mode: a shared and a static build must not share a CMake
# cache (the MSVC runtime library and the plugin set differ).
$buildDir = Join-Path $srcDir "build\msvc"
if ($isStatic) { $buildDir = Join-Path $srcDir "build\msvc-static" }

if ([string]::IsNullOrWhiteSpace($DeployDir)) {
    $DeployDir = Join-Path $srcDir "deploy"
    if ($isStatic) { $DeployDir = Join-Path $srcDir "deploy-static" }
}

Write-Host ("== Qt: {0}  [{1}]  ({2})" -f $qt, $(if ($isStatic) { "static" } else { "shared" }), $qtFrom)

# Pass the Qt prefix with forward slashes: CMake is happiest with those, and a
# backslash path can end up inside generated CMake code where "\D" is an invalid
# escape (that is exactly how the vcpkg DLL install rule once blew up).
$qtForCMake = $qt -replace '\\', '/'

# ---- 1. configure ----------------------------------------------------------
# Always configure. An existing CMakeCache.txt is NOT proof that the directory is
# usable: a configure that failed half way leaves a cache but no project files,
# and then the build step dies with
#     MSBUILD : error MSB1009: project file does not exist. Switch: ALL_BUILD.vcxproj
# Re-running configure costs a second or two when nothing changed.
Write-Host "[1/4] configure $buildDir"
& cmake -S $srcDir -B $buildDir -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=$qtForCMake" "-DCICADA_QT_STATIC=$staticFlag"
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] configure failed." -ForegroundColor Red
    Write-Host "        If this directory was configured with other options or another generator,"
    Write-Host "        delete it and run again:"
    Write-Host ("        Remove-Item -Recurse -Force `"{0}`"" -f $buildDir)
    exit $LASTEXITCODE
}

# ---- 2. build --------------------------------------------------------------
Write-Host "[2/4] build Release"
& cmake --build $buildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---- 3. install (= deploy) -------------------------------------------------
Write-Host "[3/4] install to $DeployDir"
& cmake --install $buildDir --config Release --prefix $DeployDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# ---- 4. self-check ---------------------------------------------------------
# Qt's layout after `cmake --install`:
#     <prefix>\bin\appQtPlayer.exe        the program, its Qt DLLs, libffmpeg.dll,
#                                         qt.conf  -- all of them together
#     <prefix>\plugins\platforms\qwindows.dll
#     <prefix>\qml\QtQuick\...            QML modules
#     <prefix>\translations\...
# So the DLLs are checked next to the executable, while plugins/qml sit at the
# prefix root. Checking them all in one directory is what made an earlier version
# of this script report [MISSING] for a perfectly fine deployment.
Write-Host "[4/4] dependency check"

$exeCandidates = @((Join-Path $DeployDir "bin\appQtPlayer.exe"), (Join-Path $DeployDir "appQtPlayer.exe"))
$exePath = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($exePath)) {
    Write-Host ("  [MISSING] appQtPlayer.exe (looked in {0})" -f ($exeCandidates -join ", ")) -ForegroundColor Red
    exit 1
}
$binDir = Split-Path -Parent $exePath
Write-Host ("  [ok]      {0}" -f $exePath.Substring($DeployDir.Length).TrimStart('\'))

$missing = @()

# 1. everything that must sit next to the executable
#    libxml2.dll (and its own dependencies zlib1/iconv-2) belongs here too: the
#    framework parses XML with libxml2, and on a clean machine the app dies with
#    "libxml2.dll not found" when only the pthread/SDL2 DLLs were copied.
#    Danmaku.dll is the standalone danmaku engine, libffmpeg.dll the merged FFmpeg
#    build -- neither of them is Qt, so they are needed in BOTH modes.
$inBin = @(
    "libffmpeg.dll",
    "Danmaku.dll",
    "libxml2.dll", "zlib1.dll"
)

# The Qt runtime DLLs only exist in a shared build. In a static build Qt is linked
# into appQtPlayer.exe, so requiring them would report a perfectly fine deployment
# as broken.
if (-not $isStatic) {
    $inBin += @("Qt6Core.dll", "Qt6Gui.dll", "Qt6Qml.dll", "Qt6Quick.dll", "Qt6QuickDialogs2.dll")
}

foreach ($f in $inBin) {
    if (Test-Path -LiteralPath (Join-Path $binDir $f)) {
        Write-Host ("  [ok]      bin\{0}" -f $f)
    } else {
        Write-Host ("  [MISSING] bin\{0}" -f $f) -ForegroundColor Red
        $missing += $f
    }
}

# 2. plugins and QML modules live at the prefix root -- shared builds only. In a
#    static build the platform plugin and the QML modules are linked into the exe
#    (see qt_import_plugins / qt_import_qml_plugins in CMakeLists.txt), so there is
#    nothing on disk to check and an empty list is the correct expectation.
$atPrefix = @()
if (-not $isStatic) {
    $atPrefix = @(
        "plugins\platforms\qwindows.dll",
        "qml\QtQuick\qtquick2plugin.dll",
        "qml\QtQuick\Dialogs\qtquickdialogsplugin.dll"
    )
}
foreach ($f in $atPrefix) {
    if (Test-Path -LiteralPath (Join-Path $DeployDir $f)) {
        Write-Host ("  [ok]      {0}" -f $f)
    } else {
        Write-Host ("  [MISSING] {0}" -f $f) -ForegroundColor Red
        $missing += $f
    }
}

Write-Host ""
if ($missing.Count -gt 0) {
    Write-Host "Some files are missing. Usual causes:" -ForegroundColor Yellow
    if ($isStatic) {
        Write-Host "  * a static build has no Qt DLLs and no qml\ tree by design; if the"
        Write-Host "    missing entries are libffmpeg.dll / Danmaku.dll / libxml2.dll, the"
        Write-Host "    build did not finish or the framework DLLs were not copied;"
    } else {
        Write-Host "  * the qml\ tree: CMakeLists.txt must use qt_generate_deploy_qml_app_script()"
        Write-Host "    (the non-QML variant never deploys QML modules);"
    }
    Write-Host "  * anything else: the build directory was configured before those rules were"
    Write-Host "    added, so delete it and run again:"
    Write-Host ("        Remove-Item -Recurse -Force `"{0}`"" -f $buildDir)
    exit 1
}

Write-Host ("Deploy OK -> {0}" -f $DeployDir) -ForegroundColor Green
Write-Host ("Run: `"{0}`"" -f $exePath)
