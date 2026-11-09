@echo off
rem ============================================================================
rem  ctp_hft - recompile ONLY the strategy process (fast iteration on a demo
rem  strategy). md_proc / trader_proc / query_proc are NOT rebuilt.
rem
rem  The strategy process talks to the other processes through shared memory
rem  only (hft::protocol is unchanged), so swapping the .exe is enough - no
rem  need to restart md / trader / query.
rem
rem  Usage:  build_strategy.bat [path\to\g++.exe]
rem
rem  IMPORTANT - the halt window.
rem  The always-on supervisor (guard_daemon.py) repairs a missing strategy
rem  within one 20 s tick.  If it wakes up in the middle of the compile it
rem  restarts the OLD binary, the linker then cannot replace a running file,
rem  and the "new" strategy silently keeps running the old code.  So this
rem  script raises flow\.halt FIRST and waits one full tick before killing
rem  anything, and drops it again when it is done (also on failure).
rem ============================================================================
setlocal
pushd "%~dp0"

set "GPP=%~1"
if "%GPP%"=="" if exist "%~dp0.toolchain\mgw\mingw64\bin\g++.exe" set "GPP=%~dp0.toolchain\mgw\mingw64\bin\g++.exe"
if "%GPP%"=="" (
    for /f "delims=" %%i in ('where g++ 2^>nul') do if not defined GPP set "GPP=%%i"
)
if "%GPP%"=="" (
    echo [ERROR] g++ not found. Pass it as the first argument, e.g.
    echo         build_strategy.bat C:/mingw64/bin/g++.exe
    popd & exit /b 1
)
echo [INFO ] Compiler: %GPP%

set "BINDIR=build\bin"
if not exist "%BINDIR%" mkdir "%BINDIR%"
if not exist "flow" mkdir "flow"

rem --- freeze the supervisor for the whole edit window ------------------------
echo. > "flow\.halt" 2>nul
if not exist "flow\.halt" (
    echo [ERROR] cannot create flow\.halt - refusing to touch a supervised tree.
    popd & exit /b 1
)
echo [INFO ] flow\.halt raised - supervisor paused, waiting one tick ...
ping -n 26 127.0.0.1 >nul

rem  The executable cannot be overwritten while it is running.
tasklist /FI "IMAGENAME eq strategy_proc.exe" 2>nul | findstr /i /c:"strategy_proc.exe" >nul && (
    echo [INFO ] strategy_proc.exe is running - stopping it ^(the supervisor restarts it^) ...
    taskkill /F /IM strategy_proc.exe >nul 2>&1
    ping -n 2 127.0.0.1 >nul
)

echo [INFO ] compiling strategy_proc.exe ...
"%GPP%" -std=c++17 -O2 -Wall -DFMT_HEADER_ONLY=1 -Iinclude -Isrc -Ithird_party/fmtlog -static -static-libgcc -static-libstdc++ ^
    -o "%BINDIR%\strategy_proc.exe" ^
    src/strategy/strategy_main.cpp src/strategy/obi_voi_strategy.cpp ^
    src/common/shm.cpp src/common/ctp_loader.cpp ^
    src/common/config.cpp ^
    src/common/logger.cpp src/common/process_app.cpp ^
    src/sim/sim_market.cpp src/sim/sim_matcher.cpp third_party/fmtlog/fmtlog.cc ^
    -pthread
if errorlevel 1 (
    echo [ERROR] Build failed - old binary left in place.
    del "flow\.halt" >nul 2>&1
    popd & exit /b 1
)

if exist "build\bin_simnow" copy /y "%BINDIR%\strategy_proc.exe" "build\bin_simnow\strategy_proc.exe" >nul 2>&1

del "flow\.halt" >nul 2>&1
echo [ OK ] %BINDIR%\strategy_proc.exe rebuilt; restart it with:
echo         python tools\guard.py --strategy-only
popd
exit /b 0
