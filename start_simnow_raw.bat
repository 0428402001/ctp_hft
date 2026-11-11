@echo off
rem ============================================================================
rem  ctp_hft - start all four processes on SimNow (standard CTP 6.7.7 counter).
rem
rem  Uses the build\bin_simnow flavour (compiled against the standard CTP SDK)
rem  and config\config_simnow.ini by default.
rem
rem  Usage:  start_simnow.bat                               (config_simnow.ini)

rem          start_simnow.bat config\my_simnow.ini          (any config file)
rem ============================================================================
setlocal
pushd "%~dp0"

set "CFG=%~1"
if "%CFG%"=="" set "CFG=config\config_simnow.ini"
set "BIN=build\bin_simnow"

if not exist "%BIN%\md_proc.exe" (
    echo [ERROR] Binaries not found in %BIN%. Run build_mingw.bat first.
    popd & exit /b 1
)
if not exist "%CFG%" (
    echo [ERROR] Config file not found: %CFG%
    popd & exit /b 1
)

rem ---- pre-flight: the standard CTP SDK DLLs must be next to the exes -------
if not exist "%BIN%\thosttraderapi_se.dll" if not exist "%BIN%\thosttraderapi.dll" (
    echo [ERROR] %BIN%\thosttraderapi_se.dll not found ^(standard CTP SDK^).
    echo         Copy it from sdk\stdctp\dll - build_mingw.bat does this for you.
    popd & exit /b 1
)
if not exist "%BIN%\thostmduserapi_se.dll" if not exist "%BIN%\thostmduserapi.dll" (
    echo [ERROR] %BIN%\thostmduserapi_se.dll not found ^(standard CTP SDK^).
    echo         Copy it from sdk\stdctp\dll - build_mingw.bat does this for you.
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
rem  SimNow set-1 fronts serve during trading hours (and 16:00 -> 02:30 for
rem  the night session); the 7x24 environment serves 16:00 -> 09:00/12:00.
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
echo [INFO ] flavour : standard CTP ^(%BIN%^)
echo [INFO ] starting market data process ...
start "MD" "%BIN%\md_proc.exe" --config %CFG% --mode live --no-console
ping -n 4 127.0.0.1 >nul

echo [INFO ] starting trading process ...
start "TRADER" "%BIN%\trader_proc.exe" --config %CFG% --mode live --no-console
ping -n 4 127.0.0.1 >nul

echo [INFO ] starting query process ...
start "QUERY" "%BIN%\query_proc.exe" --config %CFG% --mode live --no-console
ping -n 4 127.0.0.1 >nul

echo [INFO ] starting strategy process ...
start "STRATEGY" "%BIN%\strategy_proc.exe" --config %CFG% --mode live --no-console

echo.
echo [ OK ] All four processes started against SimNow.
echo   logs  : log\md.log  log\trader.log  log\query.log  log\strategy.log
echo   watch : monitor.bat
echo   stop  : stop_all.bat
echo.
echo   login progress: watch log\md.log / log\trader.log for "logged in",
echo   then "settlement confirmed - orders are enabled" in log\trader.log
echo   ^(SimNow rejects orders until the daily settlement is confirmed^).
popd
ping -n 4 127.0.0.1 >nul
exit /b 0
