@echo off
rem ============================================================================
rem  ctp_hft - one click build with Microsoft Visual Studio (MSVC).
rem  Produces the full live-capable version (loads the SFIT CTP Mini SDK DLLs
rem  at run time; both sim and live mode are available).
rem
rem  Usage:  build.bat            (Release, x64)
rem ============================================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
pushd "%ROOT%"

set "BINDIR=build\bin"
set "OBJDIR=build\obj"
if not exist "%BINDIR%" mkdir "%BINDIR%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

rem ---- locate the MSVC toolset ------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found - install Visual Studio 2017 or newer
    echo         with the "Desktop development with C++" workload.
    popd & exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo [ERROR] No MSVC x64 toolset found. Install the C++ workload first.
    popd & exit /b 1
)
echo [INFO ] Using %VSINSTALL%
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 (
    echo [ERROR] vcvarsall.bat failed
    popd & exit /b 1
)

set "SDK=%ROOT%..\win64"
if not exist "%SDK%\mdapi\ThostFtdcMdApi.h" (
    echo [ERROR] CTP SDK not found at %SDK%
    popd & exit /b 1
)

rem  The CTP SDK DLLs are resolved at run time (src\common\ctp_loader.*) instead
rem  of linking the MSVC import libraries, exactly like the MinGW build, so both
rem  toolchains share one live-mode code path.

set "CFLAGS=/nologo /O2 /EHsc /std:c++17 /W3 /wd4819 /D_CRT_SECURE_NO_WARNINGS /DHFT_ENABLE_CTP=1 /Iinclude /Isrc /Fo%OBJDIR%\\"
set "COMMON=src\common\shm.cpp src\common\ctp_loader.cpp src\common\config.cpp src\common\logger.cpp src\common\process_app.cpp src\sim\sim_market.cpp src\sim\sim_matcher.cpp"

echo [1/4] building md_proc ...
cl %CFLAGS% /I"%SDK%\mdapi" /Fe"%BINDIR%\md_proc.exe" src\md\md_main.cpp src\md\ctp_md_gateway.cpp %COMMON% /link /MACHINE:X64
if errorlevel 1 goto :fail

echo [2/4] building trader_proc ...
cl %CFLAGS% /I"%SDK%\traderapi" /Fe"%BINDIR%\trader_proc.exe" src\trader\trader_main.cpp src\trader\ctp_trader_gateway.cpp %COMMON% /link /MACHINE:X64
if errorlevel 1 goto :fail

echo [3/4] building query_proc ...
cl %CFLAGS% /I"%SDK%\traderapi" /Fe"%BINDIR%\query_proc.exe" src\query\query_main.cpp src\query\ctp_query_gateway.cpp %COMMON% /link /MACHINE:X64
if errorlevel 1 goto :fail

echo [4/4] building strategy_proc ...
cl %CFLAGS% /Fe"%BINDIR%\strategy_proc.exe" src\strategy\strategy_main.cpp %COMMON% /link /MACHINE:X64
if errorlevel 1 goto :fail

echo [5/5] building monitor ...
cl %CFLAGS% /Fe"%BINDIR%\monitor.exe" src\tools\monitor_main.cpp %COMMON% /link /MACHINE:X64
if errorlevel 1 goto :fail

copy /Y "%SDK%\mdapi\thostmduserapi.dll" "%BINDIR%\" >nul
copy /Y "%SDK%\traderapi\thosttraderapi.dll" "%BINDIR%\" >nul

echo.
echo [ OK ] All binaries are in %BINDIR%
echo        md_proc.exe  trader_proc.exe  query_proc.exe  strategy_proc.exe  monitor.exe
popd
exit /b 0

:fail
echo [ERROR] Build failed.
popd
exit /b 1
