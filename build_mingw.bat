@echo off
rem ============================================================================
rem  ctp_hft - build with MinGW-w64 (simulation + live CTP capable).
rem
rem  Live CTP works here as well: the SDK DLLs are resolved at run time
rem  (src\common\ctp_loader.*) instead of linking the MSVC import libraries
rem  (MinGW cannot link them, but the CTP API classes are pure-virtual, so
rem  calling them through LoadLibrary is ABI safe). In sim mode the SDK DLLs
rem  are not touched at all.
rem
rem  Two live flavours are produced when the SDKs are present:
rem    build\bin         CTP Mini  (SDK at ..\win64, CTPII Mini brokers)
rem    build\bin_simnow  standard CTP 6.7.7 (sdk\stdctp, SimNow / ordinary
rem                      CTP brokers - compiled with -DHFT_CTP_STD=1)
rem  strategy_proc / monitor are flavour independent; only the three
rem  CTP-talking processes are built twice.
rem
rem  Usage:  build_mingw.bat [path\to\g++.exe]
rem ============================================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
pushd "%ROOT%"

set "GPP=%~1"
if "%GPP%"=="" (
    for /f "delims=" %%i in ('where g++ 2^>nul') do if not defined GPP set "GPP=%%i"
)
if "%GPP%"=="" if exist "%ROOT%.toolchain\mgw\mingw64\bin\g++.exe" set "GPP=%ROOT%.toolchain\mgw\mingw64\bin\g++.exe"
if "%GPP%"=="" (
    echo [ERROR] g++ not found. Pass it as the first argument, e.g.
    echo         build_mingw.bat C:\mingw64\bin\g++.exe
    popd & exit /b 1
)
echo [INFO ] Compiler: %GPP%

set "SDK=%ROOT%..\win64"
if not exist "%SDK%\mdapi\ThostFtdcMdApi.h" (
    echo [WARN ] CTP Mini SDK not found at %SDK% - no mini flavour.
    set "MINICTP="
) else (
    set "MINICTP=1"
)

set "SDKSTD=%ROOT%sdk\stdctp"
if not exist "%SDKSTD%\include\ThostFtdcMdApi.h" (
    echo [WARN ] standard CTP SDK not found at %SDKSTD% - no simnow flavour.
    set "STDCTP="
) else (
    set "STDCTP=1"
)

rem  -static / -static-libgcc / -static-libstdc++ pull the MinGW runtime into
rem  the executable. Without them every .exe needs libgcc_s_seh-1.dll,
rem  libstdc++-6.dll and libwinpthread-1.dll at start-up, none of which exist
rem  on a stock Windows installation.
set "CFLAGS=-std=c++17 -O2 -Wall -DFMT_HEADER_ONLY=1 -Iinclude -Isrc -Ithird_party/fmtlog"
set "LDFLAGS=-static -static-libgcc -static-libstdc++"
rem  The SFIT headers are GBK encoded; our gateway/main files are pure
rem  ASCII so they can be read with -finput-charset=GBK.
set "CTPBASE=-DHFT_ENABLE_CTP=1 -finput-charset=GBK"
set "COMMON=src/common/shm.cpp src/common/ctp_loader.cpp src/common/config.cpp src/common/logger.cpp src/common/process_app.cpp src/sim/sim_market.cpp src/sim/sim_matcher.cpp third_party/fmtlog/fmtlog.cc"

rem  Safety net: if the toolchain ignores -static (or was configured without a
rem  static libwinpthread), drop the runtime DLLs next to the executables so the
rem  processes still start. Harmless when they are not needed.
set "GPPDIR=%GPP:\bin\g++.exe=%"
set "GPPDIR=%GPPDIR:\bin\g++=%"

rem ---------------------------------------------------------------------------
rem  Flavour 1: CTP Mini -> build\bin
rem ---------------------------------------------------------------------------
if defined MINICTP (
    set "BINDIR=build\bin"
    if not exist "!BINDIR!" mkdir "!BINDIR!"
    set "MDF=-I%SDK%\mdapi !CTPBASE!"
    set "TRF=-I%SDK%\traderapi !CTPBASE!"

    echo [1/7] md_proc  ^(mini^) ...
    "%GPP%" %CFLAGS% !MDF! %LDFLAGS% -o "!BINDIR!\md_proc.exe" src/md/md_main.cpp src/md/ctp_md_gateway.cpp %COMMON% -pthread
    if errorlevel 1 goto :fail

    echo [2/7] trader_proc ^(mini^) ...
    "%GPP%" %CFLAGS% !TRF! %LDFLAGS% -o "!BINDIR!\trader_proc.exe" src/trader/trader_main.cpp src/trader/ctp_trader_gateway.cpp %COMMON% -pthread
    if errorlevel 1 goto :fail

    echo [3/7] query_proc ^(mini^) ...
    "%GPP%" %CFLAGS% !TRF! %LDFLAGS% -o "!BINDIR!\query_proc.exe" src/query/query_main.cpp src/query/ctp_query_gateway.cpp %COMMON% -pthread
    if errorlevel 1 goto :fail

    if exist "%GPPDIR%\bin\libgcc_s_seh-1.dll" copy /y "%GPPDIR%\bin\libgcc_s_seh-1.dll" "!BINDIR!\" >nul
    if exist "%GPPDIR%\bin\libstdc++-6.dll" copy /y "%GPPDIR%\bin\libstdc++-6.dll" "!BINDIR!\" >nul
    if exist "%GPPDIR%\bin\libwinpthread-1.dll" copy /y "%GPPDIR%\bin\libwinpthread-1.dll" "!BINDIR!\" >nul
    if exist "%SDK%\mdapi\thostmduserapi.dll" copy /y "%SDK%\mdapi\thostmduserapi.dll" "!BINDIR!\" >nul
    if exist "%SDK%\traderapi\thosttraderapi.dll" copy /y "%SDK%\traderapi\thosttraderapi.dll" "!BINDIR!\" >nul
) else (
    echo [SKIP] mini flavour ^(SDK missing^)
)

rem ---------------------------------------------------------------------------
rem  Flavour 2: standard CTP 6.7.7 -> build\bin_simnow
rem ---------------------------------------------------------------------------
if defined STDCTP (
    set "BINDIR=build\bin_simnow"
    if not exist "!BINDIR!" mkdir "!BINDIR!"
    set "MDF=-I%SDKSTD%\include !CTPBASE! -DHFT_CTP_STD=1"
    set "TRF=-I%SDKSTD%\include !CTPBASE! -DHFT_CTP_STD=1"

    echo [4/7] md_proc  ^(standard CTP / SimNow^) ...
    "%GPP%" %CFLAGS% !MDF! %LDFLAGS% -o "!BINDIR!\md_proc.exe" src/md/md_main.cpp src/md/ctp_md_gateway.cpp %COMMON% -pthread
    if errorlevel 1 goto :fail

    echo [5/7] trader_proc ^(standard CTP / SimNow^) ...
    "%GPP%" %CFLAGS% !TRF! %LDFLAGS% -o "!BINDIR!\trader_proc.exe" src/trader/trader_main.cpp src/trader/ctp_trader_gateway.cpp %COMMON% -pthread
    if errorlevel 1 goto :fail

    echo [6/7] query_proc ^(standard CTP / SimNow^) ...
    "%GPP%" %CFLAGS% !TRF! %LDFLAGS% -o "!BINDIR!\query_proc.exe" src/query/query_main.cpp src/query/ctp_query_gateway.cpp %COMMON% -pthread
    if errorlevel 1 goto :fail

    if exist "%GPPDIR%\bin\libgcc_s_seh-1.dll" copy /y "%GPPDIR%\bin\libgcc_s_seh-1.dll" "!BINDIR!\" >nul
    if exist "%GPPDIR%\bin\libstdc++-6.dll" copy /y "%GPPDIR%\bin\libstdc++-6.dll" "!BINDIR!\" >nul
    if exist "%GPPDIR%\bin\libwinpthread-1.dll" copy /y "%GPPDIR%\bin\libwinpthread-1.dll" "!BINDIR!\" >nul
    if exist "%SDKSTD%\dll\thostmduserapi_se.dll" copy /y "%SDKSTD%\dll\thostmduserapi_se.dll" "!BINDIR!\" >nul
    if exist "%SDKSTD%\dll\thosttraderapi_se.dll" copy /y "%SDKSTD%\dll\thosttraderapi_se.dll" "!BINDIR!\" >nul
) else (
    echo [SKIP] simnow flavour ^(SDK missing^)
)

rem ---------------------------------------------------------------------------
rem  Flavour independent targets -> build\bin (also usable from bin_simnow:
rem  pass --config and the exe dir separately, or copy them over).
rem ---------------------------------------------------------------------------
set "BINDIR=build\bin"
if not exist "%BINDIR%" mkdir "%BINDIR%"

echo [7/7] strategy_proc + monitor + shm_reset ^(shared^) ...
"%GPP%" %CFLAGS% %LDFLAGS% -o "%BINDIR%\strategy_proc.exe" src/strategy/strategy_main.cpp src/strategy/obi_voi_strategy.cpp %COMMON% -pthread
if errorlevel 1 goto :fail
"%GPP%" %CFLAGS% %LDFLAGS% -o "%BINDIR%\monitor.exe" src/tools/monitor_main.cpp %COMMON% -pthread
if errorlevel 1 goto :fail
"%GPP%" %CFLAGS% %LDFLAGS% -o "%BINDIR%\shm_reset.exe" src/tools/shm_reset_main.cpp src/common/shm.cpp -pthread
if errorlevel 1 goto :fail
copy /y "%BINDIR%\strategy_proc.exe" "build\bin_simnow\strategy_proc.exe" >nul 2>&1
copy /y "%BINDIR%\monitor.exe" "build\bin_simnow\monitor.exe" >nul 2>&1
copy /y "%BINDIR%\shm_reset.exe" "build\bin_simnow\shm_reset.exe" >nul 2>&1

echo.
echo [ OK ] Done.
echo         Mini flavour : build\bin          ^(CTPII Mini brokers^)
echo         SimNow       : build\bin_simnow   ^(standard CTP 6.7.7^)
echo         Simulation   : start_sim.bat
popd
exit /b 0

:fail
echo [ERROR] Build failed.
popd
exit /b 1
