@echo off
setlocal enabledelayedexpansion

REM ============================================================================
REM CicadaPlayerNext - Windows cmdline build (MSVC)
REM
REM Builds BOTH architectures by default:
REM   x86 (32-bit) : -A Win32 -> vcpkg x86-windows -> external\install\*\win32\i686
REM   x64 (64-bit) : -A x64   -> vcpkg x64-windows -> external\install\*\win32\x86_64
REM
REM Usage:  build_win.bat [x86 | x64 | both]        (default: both)
REM
REM Visual Studio is detected at RUN TIME via vswhere. No IDE version is
REM hardcoded: VS 2017 / 2019 / 2022 / 2026 all work. The CMake generator, the
REM VsDevCmd.bat path and the MSVC toolset all follow whatever IDE is actually
REM installed on this machine.
REM
REM Each architecture gets its own build tree, so building one never destroys the
REM other's output:
REM   cmdline\msvc_build\_generated\cmake_x86\Release\cicadaPlayer.exe
REM   cmdline\msvc_build\_generated\cmake_x64\Release\cicadaPlayer.exe
REM ============================================================================

SET currentPath=%~dp0
SET projectPath=%currentPath%..\
SET tmpDIR=%currentPath%_generated\

REM ---- normalise paths -------------------------------------------------------
REM A trailing "\" would escape the closing quote for the CRT argument parser,
REM so strip it first, then let "for %%~fi" resolve the "..\" segment.
SET "sourceDIR=%projectPath%"
if "%sourceDIR:~-1%"=="\" SET "sourceDIR=%sourceDIR:~0,-1%"
for %%i in ("%sourceDIR%") do SET "sourceDIR=%%~fi"
for %%i in ("%sourceDIR%\..") do SET "repoDIR=%%~fi"

SET "tmpDIRN=%tmpDIR%"
if "%tmpDIRN:~-1%"=="\" SET "tmpDIRN=%tmpDIRN:~0,-1%"
for %%i in ("%tmpDIRN%") do SET "tmpDIRN=%%~fi"

REM ---- optional overrides (leave empty for auto-detection) -------------------
REM VS_INSTALL_DIR : pin a specific Visual Studio instance
REM VS_GENERATOR   : pin the CMake generator, e.g. "Visual Studio 17 2022"
REM MSVC_TOOLSET   : pin an MSVC toolset, e.g. v142 / v143 / v141_xp.
REM                  Empty = the IDE's own default toolset (recommended).
SET "VS_INSTALL_DIR="
SET "VS_GENERATOR="
SET "MSVC_TOOLSET="

REM ---- requested architectures -----------------------------------------------
REM Both architectures are available because external\install\<dep>\win32\ holds
REM an i686 AND an x86_64 copy of every dependency, and the vcpkg install has the
REM matching x86-windows / x64-windows triplets. framework\windows.cmake derives
REM ARCH from CMAKE_SIZEOF_VOID_P, so the generator platform (-A) alone decides
REM which dependency set is linked.
SET "ARCH_LIST=x86 x64"
if /i "%~1"=="x86"   SET "ARCH_LIST=x86"
if /i "%~1"=="win32" SET "ARCH_LIST=x86"
if /i "%~1"=="x64"   SET "ARCH_LIST=x64"
if /i "%~1"=="amd64" SET "ARCH_LIST=x64"
if /i "%~1"=="both"  SET "ARCH_LIST=x86 x64"
echo === architectures : %ARCH_LIST%

REM ============================================================================
REM Locate vcpkg
REM   1. VCPKG_ROOT env var
REM   2. vcpkg on PATH (its parent dir is the vcpkg root)
REM   3. C:\vcpkg and %USERPROFILE%\vcpkg
REM   4. one level under drive roots: <drive>:\<dir>\vcpkg (C-G)
REM If NOT found, print diagnostics and STOP (never clone/install vcpkg).
REM ============================================================================
SET vcpkg_root=
if defined VCPKG_ROOT (
    if exist "%VCPKG_ROOT%\vcpkg.exe" if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" SET "vcpkg_root=%VCPKG_ROOT%"
)
if not defined vcpkg_root (
    for /f "delims=" %%i in ('where vcpkg 2^>nul') do if not defined vcpkg_root call :check_exe_root "%%i"
)
if not defined vcpkg_root (
    for %%d in (C D E F G) do if exist "%%d:\vcpkg\vcpkg.exe" if not defined vcpkg_root call :validate_root "%%d:\vcpkg"
)
if not defined vcpkg_root (
    for %%d in (C D E F G) do for /d %%s in ("%%d:\*") do if exist "%%s\vcpkg\vcpkg.exe" if not defined vcpkg_root call :validate_root "%%s\vcpkg"
)
if not defined vcpkg_root (
    if exist "%USERPROFILE%\vcpkg\vcpkg.exe" call :validate_root "%USERPROFILE%\vcpkg"
)
REM fallback: project-local vcpkg installed by external\win\install_external.bat
if not defined vcpkg_root (
    if exist "%projectPath%..\external\win\vcpkg\scripts\buildsystems\vcpkg.cmake" SET "vcpkg_root=%projectPath%..\external\win\vcpkg"
)

if not defined vcpkg_root (
    echo.
    echo === vcpkg detection diagnostics ===
    if defined VCPKG_ROOT (echo VCPKG_ROOT is set to: "%VCPKG_ROOT%") else (echo VCPKG_ROOT is NOT set)
    where vcpkg 2>nul && (echo vcpkg is on PATH) || (echo vcpkg is NOT on PATH)
    echo checked: C-G:\vcpkg, C-G:\*\vcpkg, %USERPROFILE%\vcpkg, external\win\vcpkg
    echo.
    echo ACTION REQUIRED: set VCPKG_ROOT to your existing vcpkg directory, e.g.:
    echo     setx VCPKG_ROOT "D:\path\to\vcpkg"
    echo then open a NEW terminal and run this script again.
    echo.
    pause
    exit /b 1
)

SET toolchain_file=%vcpkg_root%\scripts\buildsystems\vcpkg.cmake
echo use vcpkg         : %vcpkg_root%

REM ============================================================================
REM Detect Visual Studio via vswhere - the newest installed instance wins
REM ============================================================================
SET "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
SET "vs_install="
SET "vs_major="
SET "vs_generator="

if defined VS_INSTALL_DIR SET "vs_install=%VS_INSTALL_DIR%"

if not defined vs_install (
    if exist "%vswhere%" (
        for /f "usebackq tokens=*" %%i in (`"%vswhere%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do SET "vs_install=%%i"
        for /f "usebackq tokens=1 delims=." %%i in (`"%vswhere%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion`) do SET "vs_major=%%i"
    )
)
REM No C++ workload reported, or an older vswhere without -requires: retry
REM without the component filter so the diagnostics below stay useful.
if not defined vs_install (
    if exist "%vswhere%" (
        for /f "usebackq tokens=*" %%i in (`"%vswhere%" -latest -prerelease -products * -property installationPath`) do SET "vs_install=%%i"
        for /f "usebackq tokens=1 delims=." %%i in (`"%vswhere%" -latest -prerelease -products * -property installationVersion`) do SET "vs_major=%%i"
    )
)
if not defined vs_major (
    if exist "%vswhere%" (
        for /f "usebackq tokens=1 delims=." %%i in (`"%vswhere%" -latest -prerelease -products * -property installationVersion`) do SET "vs_major=%%i"
    )
)

if not defined vs_install goto :no_vs

REM Map the IDE major version to its CMake generator, so any of these work
REM without editing this file.
if "%vs_major%"=="18" SET "vs_generator=Visual Studio 18 2026"
if "%vs_major%"=="17" SET "vs_generator=Visual Studio 17 2022"
if "%vs_major%"=="16" SET "vs_generator=Visual Studio 16 2019"
if "%vs_major%"=="15" SET "vs_generator=Visual Studio 15 2017"
if defined VS_GENERATOR SET "vs_generator=%VS_GENERATOR%"
if not defined vs_generator goto :unsupported_vs

SET "vsdev=%vs_install%\Common7\Tools\VsDevCmd.bat"
if not exist "%vsdev%" goto :no_vs

echo use Visual Studio : %vs_install%  [v%vs_major%]
echo use generator     : %vs_generator%
if defined MSVC_TOOLSET echo use toolset       : %MSVC_TOOLSET%

REM ---- Visual Studio developer environment (x86 host tools) ------------------
REM Sourced from the detected install instead of the old hardcoded VS2017
REM Professional path. Run before configure so compiler probes see cl.exe.
call "%vsdev%" -arch=x86 -host_arch=x64 -no_logo
if errorlevel 1 echo WARNING: VsDevCmd.bat reported an error, continuing anyway.

REM ============================================================================
REM Build every requested architecture
REM ============================================================================
SET "BUILD_FAILED=0"
for %%A in (%ARCH_LIST%) do (
    call :build_arch %%A
    if errorlevel 1 set "BUILD_FAILED=1"
)

echo.
if "%BUILD_FAILED%"=="1" (
    echo === BUILD FAILED ===
    echo At least one architecture did not build - see the output above.
    echo.
    pause
    exit /b 1
)

echo === BUILD OK ===
echo.
echo Artifacts:
for %%A in (%ARCH_LIST%) do (
    if /i "%%A"=="x86" echo   x86 ^(32-bit^) : %tmpDIRN%\cmake_x86\Release\cicadaPlayer.exe
    if /i "%%A"=="x64" echo   x64 ^(64-bit^) : %tmpDIRN%\cmake_x64\Release\cicadaPlayer.exe
)
echo.
echo libffmpeg.dll was copied next to each executable automatically.
echo.
pause
exit /b 0

REM ============================================================================
REM :build_arch <x86|x64>
REM Configures and builds one architecture in its own build tree.
REM ============================================================================
:build_arch
SET "ARCH_KEY=%~1"

REM vcpkg derives its triplet from the generator platform, so Win32 -> x86-windows
REM and x64 -> x64-windows. framework\windows.cmake derives ARCH from
REM CMAKE_SIZEOF_VOID_P, which is what selects external\install\*\win32\<arch>.
if /i "%ARCH_KEY%"=="x86" (
    SET "VS_PLATFORM=Win32"
    SET "ARCH_DEFINE=i686"
    SET "PLATFORM_DEFINE=x86"
)
if /i "%ARCH_KEY%"=="x64" (
    SET "VS_PLATFORM=x64"
    SET "ARCH_DEFINE=x86_64"
    SET "PLATFORM_DEFINE=x64"
)

SET "ARCH_BUILD_DIR=%tmpDIRN%\cmake_%ARCH_KEY%"

echo.
echo ============================================================================
echo === building %ARCH_KEY%  ^(platform=%VS_PLATFORM%, deps=%ARCH_DEFINE%^)
echo ============================================================================

REM Only this architecture's tree is wiped, so "build_win.bat x64" leaves a
REM previously built x86 output untouched.
REM
REM A still-running cicadaPlayer.exe cannot be deleted or overwritten. That
REM shows up after the usual "run it, change something, rebuild" cycle as
REM   LNK1104: cannot open file '...\Release\cicadaPlayer.exe'
REM because rd cannot remove the locked exe and the linker cannot replace it.
REM Stop any leftover instance first so the tree wipe and the link can proceed.
taskkill /f /im cicadaPlayer.exe >nul 2>&1
if not errorlevel 1 echo stopped a running cicadaPlayer.exe so it can be rebuilt

if exist "%ARCH_BUILD_DIR%" rd /s /q "%ARCH_BUILD_DIR%"
md "%ARCH_BUILD_DIR%"
cd /d "%ARCH_BUILD_DIR%"

SET "toolset_arg="
if defined MSVC_TOOLSET SET "toolset_arg=-T %MSVC_TOOLSET%"

cmake "%sourceDIR%" -G "%vs_generator%" -A %VS_PLATFORM% %toolset_arg% -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%toolchain_file%" -DPLATFORM=%PLATFORM_DEFINE% -DARCH=%ARCH_DEFINE%
if errorlevel 1 goto :arch_configure_failed

REM Replaces the old `devenv %sln% /rebuild "Release" /project ALL_BUILD`.
REM That line never worked: devenv.exe is not on PATH (VsDevCmd.bat only adds
REM MSBuild), and the script's own %%devenv%% variable was never referenced.
REM --clean-first keeps the /rebuild semantics.
REM echo on is turned on only for the build line, so the comments above are not
REM echoed into the log.
echo on
cmake --build . --config Release --target ALL_BUILD --clean-first
@echo off
if errorlevel 1 goto :arch_build_failed

REM The runtime DLL has to match the architecture, so copy the right one instead
REM of leaving a manual step that is easy to get wrong in a dual-arch build.
SET "FFMPEG_DLL=%repoDIR%\external\install\ffmpeg\win32\%ARCH_DEFINE%\libffmpeg.dll"
if exist "%FFMPEG_DLL%" (
    copy /y "%FFMPEG_DLL%" "%ARCH_BUILD_DIR%\Release\" >nul
    echo copied %ARCH_DEFINE%\libffmpeg.dll next to the exe
) else (
    echo WARNING: %FFMPEG_DLL% not found - copy it manually before running.
)

echo %ARCH_KEY% build OK : %ARCH_BUILD_DIR%\Release\cicadaPlayer.exe
exit /b 0

:arch_configure_failed
echo.
echo === %ARCH_KEY% CMAKE CONFIGURE FAILED ===
echo Check the CMake output above; the .sln was not generated.
exit /b 1

:arch_build_failed
echo.
echo === %ARCH_KEY% BUILD FAILED ===
exit /b 1

REM ============================================================================
REM Failure paths - always return a real exit code (the old script exited 0
REM unconditionally, so failures looked like successes to any caller).
REM ============================================================================

:no_vs
echo.
echo === Visual Studio not found ===
echo vswhere: %vswhere%
echo.
echo Install any Visual Studio 2017 or newer with the
echo   "Desktop development with C++" workload
echo (that workload provides Microsoft.VisualStudio.Component.VC.Tools.x86.x64),
echo or point this script at an existing installation by setting VS_INSTALL_DIR.
echo.
pause
exit /b 1

:unsupported_vs
echo.
echo === Unsupported Visual Studio version ===
echo detected: %vs_install%
echo version : %vs_major%
echo.
echo No generator mapping for this version. Pick one explicitly, e.g.
echo     set VS_GENERATOR=Visual Studio 17 2022
echo then run this script again.
echo.
pause
exit /b 1

REM ============================================================================
REM Helpers
REM ============================================================================

REM vcpkg.exe on PATH: its parent dir is the vcpkg root
:check_exe_root
for %%p in ("%~dp1..") do call :validate_root "%%~fp"
exit /b 0

REM a valid vcpkg root contains both vcpkg.exe and scripts\buildsystems\vcpkg.cmake
:validate_root
if exist "%~1\vcpkg.exe" if exist "%~1\scripts\buildsystems\vcpkg.cmake" SET "vcpkg_root=%~1"
exit /b 0
