@echo off
setlocal
set "PS1=%~dp0deploy_win.ps1"

rem This .bat is only an entry point: every line of build logic lives in
rem deploy_win.ps1, which must sit next to it (that is what %~dp0 resolves to).
rem One copy of the logic on purpose -- two would drift apart.
if exist "%PS1%" goto :run
echo [ERROR] deploy_win.ps1 is missing next to this .bat
exit /b 1

:run
if /I "%~1"=="-h"     goto :usage
if /I "%~1"=="--help" goto :usage
if /I "%~1"=="help"   goto :usage
if /I "%~1"=="static" goto :static
if /I "%~1"=="shared" goto :shared

rem Anything else is forwarded verbatim, so the full ps1 syntax keeps working (that path
rem forwards the raw argument string -- nothing is dropped). One convenience case: a bare
rem path with no leading "-" means -QtPrefix -- "deploy_win.bat D:\Qt-Static\msvc" -- and
rem the linking mode then follows that prefix by itself (static prefix = static build).
set "FIRST=%~1"
if "%FIRST%"=="" goto :passthrough
if "%FIRST:~0,1%"=="-" goto :passthrough
set "ARGS=-QtPrefix %*"
goto :invoke

:passthrough
set "ARGS=%*"
goto :invoke

rem ---------------------------------------------------------------- static ----
rem "static" builds against a STATIC Qt. Where that install lives, in order:
rem   1. the second argument here:      deploy_win.bat static D:\Qt-Static\msvc
rem   2. the environment variable:      set QT_STATIC_DIR=D:\Qt-Static\msvc
rem      (alias: CICADA_QT_STATIC_DIR)  then: deploy_win.bat static
rem   3. the script's own guess:        DRIVE:\Qt-Static\msvc
rem Case 1 is handed to the child PowerShell through the environment instead of the
rem command line: quoted paths inside a "set VAR=..." line are a batch minefield, and
rem an environment variable carries spaces without any quoting at all. setlocal keeps
rem it from leaking into the caller's environment.
rem (No redirection character may appear anywhere in this file, not even inside a rem
rem  line: cmd parses redirection before it decides that the line is a comment, so a
rem  stray angle bracket makes it try to open a file.)
:static
rem Drop the "static" token, then look at what is left.
shift
set "ARGS=-Static"
set "SECOND=%~1"
if "%SECOND%"=="" goto :collect
if "%SECOND:~0,1%"=="-" goto :collect
set "QT_STATIC_DIR=%SECOND%"
echo [info] static Qt prefix from the command line: %SECOND%
shift

:collect
rem Re-append the rest one by one. The old inline form listed the first nine positional
rem parameters explicitly, which silently dropped everything past the ninth (a test
rem caught exactly that: "static -DeployDir out" lost "-DeployDir out"). The raw argument
rem string cannot help either -- shift does not update it.
rem (Nothing in this file may contain a redirection character, not even in rem lines --
rem  see the note above the :static label.)
if "%~1"=="" goto :invoke
set "ARGS=%ARGS% %1"
shift
goto :collect

rem ---------------------------------------------------------------- shared ----
:shared
shift
set "ARGS="
goto :collect

:invoke
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %ARGS%
exit /b %ERRORLEVEL%

:usage
echo Usage: deploy_win.bat [static [QT-PREFIX] / shared / QT-PREFIX / -QtPrefix DIR / -DeployDir DIR]
echo.
echo   (no argument)        dynamic Qt  -^>  build\msvc         -^>  deploy\
echo   static               static Qt   -^>  build\msvc-static  -^>  deploy-static\
echo   static QT-PREFIX     static Qt from this install, e.g.
echo                        deploy_win.bat static D:\Qt-Static\msvc
echo   QT-PREFIX            same thing, shortened: deploy_win.bat D:\Qt-Static\msvc
echo   shared               force the dynamic path even if QT_STATIC_DIR is set
echo.
echo   The linking mode follows the Qt prefix (static Qt = a prefix without bin\Qt6Core.dll).
echo   Static install location, in order: the QT-PREFIX argument, the environment
echo   variable QT_STATIC_DIR (alias CICADA_QT_STATIC_DIR), then ^<drive^>:\Qt-Static\msvc.
echo   -Static without a usable static Qt stops with an error -- it never builds shared.
echo.
echo   Full details: README.md section 3.8, and "deploy_win.ps1 -Help" for every switch.
exit /b 0
