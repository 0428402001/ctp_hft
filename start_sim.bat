@echo off
rem ============================================================================
rem  Start all four processes in simulation mode (no broker needed).
rem  Every process gets its own console window so you can watch them live.
rem ============================================================================
setlocal
pushd "%~dp0"

set "CFG=config\config.ini"
set "BIN=build\bin"

if not exist "%BIN%\md_proc.exe" (
    echo [ERROR] Binaries not found. Run build.bat ^(MSVC^) or build_mingw.bat first.
    popd & exit /b 1
)

rem ---- cold start: wipe the shared-memory bus --------------------------------
rem  Leftover queue contents from an earlier run would otherwise be replayed
rem  into the freshly started processes.
if exist "%BIN%\shm_reset.exe" (
    echo [INFO ] resetting shared memory ...
    "%BIN%\shm_reset.exe"
)

echo starting market data process ...
start "MD" "%BIN%\md_proc.exe" --config %CFG%
ping -n 2 127.0.0.1 >nul

echo starting trading process ...
start "TRADER" "%BIN%\trader_proc.exe" --config %CFG%
ping -n 2 127.0.0.1 >nul

echo starting query process ...
start "QUERY" "%BIN%\query_proc.exe" --config %CFG%
ping -n 3 127.0.0.1 >nul

echo starting strategy process ...
start "STRATEGY" "%BIN%\strategy_proc.exe" --config %CFG%

echo.
echo All four processes started.
echo   logs  : log\*.log
echo   watch : monitor.bat
echo   stop  : stop_all.bat
popd
ping -n 4 127.0.0.1 >nul
exit /b 0
