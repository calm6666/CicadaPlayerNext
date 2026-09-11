@echo off
REM Detect an existing vcpkg installation, in order:
REM   1. VCPKG_ROOT env var
REM   2. vcpkg on PATH (its parent dir is the vcpkg root)
REM   3. C:\vcpkg ... G:\vcpkg and %USERPROFILE%\vcpkg
REM   4. one level under drive roots: <drive>:\<dir>\vcpkg (C-G)
REM If found, use it to install the missing packages and exit.
REM If NOT found, fall back to the original flow: clone pinned vcpkg
REM into external\win\vcpkg and install packages there.
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

if defined vcpkg_root (
    echo use installed vcpkg: %vcpkg_root%
    "%vcpkg_root%\vcpkg.exe" integrate install
    REM build is x86 (-DPLATFORM=x86), install deps to the x86-windows triplet.
    REM new vcpkg port name is pthreads; fall back to pthread (old name) on failure
    "%vcpkg_root%\vcpkg.exe" install openssl sdl2 pthreads dirent libxml2 curl[openssl] --triplet x86-windows || "%vcpkg_root%\vcpkg.exe" install pthread --triplet x86-windows
    pause
    exit /b 0
)

echo.
echo no existing vcpkg found, fall back to original flow:
echo clone pinned vcpkg into external\win\vcpkg and install packages there.
echo.
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
git checkout 109ce457421dbef04011a7e5a4fd482f61e21f62
.\bootstrap-vcpkg.sh
.\vcpkg integrate install
.\vcpkg install openssl sdl2 pthread dirent libxml2
git checkout 5a271a9290282e09149401486f88dc106dc65b71
.\bootstrap-vcpkg.bat
.\vcpkg install curl[openssl]
pause
exit /b 0

REM vcpkg.exe on PATH: its parent dir is the vcpkg root
:check_exe_root
for %%p in ("%~dp1..") do call :validate_root "%%~fp"
exit /b 0

REM a valid vcpkg root contains both vcpkg.exe and scripts\buildsystems\vcpkg.cmake
:validate_root
if exist "%~1\vcpkg.exe" if exist "%~1\scripts\buildsystems\vcpkg.cmake" SET "vcpkg_root=%~1"
exit /b 0
