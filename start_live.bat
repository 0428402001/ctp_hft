@echo off
rem ============================================================================
rem  ctp_hft - start all four processes in LIVE mode.
rem
rem  Every process gets its own console window so you can watch them live.
rem  The market-data process must be up first (it creates the shared memory),
rem  the strategy process goes last (it waits for ticks and the account book).
rem
rem  Usage:  start_live.bat                          (uses config\config_live.ini)
rem          start_live.bat config\my_live.ini      (any config file)
rem ============================================================================
setlocal
pushd "%~dp0"

set "CFG=%~1"
if "%CFG%"=="" set "CFG=config\config_live.ini"
set "BIN=build\bin"

if not exist "%BIN%\md_proc.exe" (
    echo [ERROR] Binaries not found. Run build.bat ^(MSVC^) or build_mingw.bat first.
    popd & exit /b 1
)
if not exist "%CFG%" (
    echo [ERROR] Config file not found: %CFG%
    popd & exit /b 1
)

rem ---- pre-flight: the CTP SDK DLLs must sit next to the executables ---------
if not exist "%BIN%\thosttraderapi.dll" (
    echo [ERROR] %BIN%\thosttraderapi.dll not found.
    echo         Live mode loads the CTP SDK at run time; copy it from the
    echo         win64\traderapi SDK directory ^(build scripts do this for you^).
    popd & exit /b 1
)
if not exist "%BIN%\thostmduserapi.dll" (
    echo [ERROR] %BIN%\thostmduserapi.dll not found.
    echo         Copy it from the win64\mdapi SDK directory.
    popd & exit /b 1
)

rem ---- pre-flight: make sure no processes from an earlier run are alive -----
tasklist /FI "IMAGENAME eq md_proc.exe" 2>nul | find /i "md_proc.exe" >nul && (
    echo [WARN ] md_proc.exe is still running - run stop_all.bat first.
    popd & exit /b 1
)

rem ---- cold start: wipe the shared-memory bus --------------------------------
rem  Leftover queue contents from an earlier run would otherwise be replayed
rem  into the freshly started processes.
echo [INFO ] resetting shared memory ...
"%BIN%\shm_reset.exe"
if errorlevel 1 (
    echo [ERROR] shm_reset failed.
    popd & exit /b 1
)

rem ---- pre-flight: warn outside trading hours --------------------------------
rem  SimNow set-1 fronts only serve during trading hours; the 7x24 environment
rem  serves 16:00 -> 09:00/12:00. A wrong time slot shows up as
rem  "connection refused" / endless reconnects, so say it up front.
python -c "import sys" >nul 2>&1
if not errorlevel 1 (
    python -c "import datetime,sys; t=datetime.datetime.now(); m=t.hour*60+t.minute; ok=(540<=m<900)or(m>=1260)or(m<150); sys.exit(0 if ok else 1)" >nul 2>&1
    if errorlevel 1 (
        echo [WARN ] Now is outside trading hours ^(^~9:00-15:00, 21:00-02:30^).
        echo        SimNow set-1 fronts will reject connections until the next session.
        echo        For after-hours testing switch the fronts to the 7x24
        echo        environment ^ and start after 16:00.
    )
)

echo [INFO ] config : %CFG%
echo [INFO ] starting market data process ...
start "MD" "%BIN%\md_proc.exe" --config %CFG% --mode live
ping -n 4 127.0.0.1 >nul

echo [INFO ] starting trading process ...
start "TRADER" "%BIN%\trader_proc.exe" --config %CFG% --mode live
ping -n 4 127.0.0.1 >nul

echo [INFO ] starting query process ...
start "QUERY" "%BIN%\query_proc.exe" --config %CFG% --mode live
ping -n 4 127.0.0.1 >nul

echo [INFO ] starting strategy process ...
start "STRATEGY" "%BIN%\strategy_proc.exe" --config %CFG% --mode live

echo.
echo [ OK ] All four processes started in LIVE mode.
echo   logs  : log\md.log  log\trader.log  log\query.log  log\strategy.log
echo   watch : monitor.bat
echo   stop  : stop_all.bat
echo.
echo   login progress: watch log\md.log / log\trader.log for
echo   "logged in" ^(or OnRspUserLogin errors if the account is wrong^).
popd
ping -n 4 127.0.0.1 >nul
exit /b 0
