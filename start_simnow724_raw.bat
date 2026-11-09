@echo off
rem ============================================================================
rem  ctp_hft - start all four processes on the SimNow 7x24 environment.
rem
rem  Uses the build\bin_simnow flavour (standard CTP SDK) and
rem  config\config_simnow724.ini:
rem      Trade Front  tcp://182.254.243.31:40001
rem      Market Front tcp://182.254.243.31:40011
rem
rem  The 7x24 environment (看穿式前置) only serves:
rem      trading days     16:00 -> next day 09:00
rem      non-trading days 16:00 -> 12:00
rem  Outside those windows use start_simnow.bat (set 1) instead.
rem
rem  Usage:  start_simnow724.bat
rem          start_simnow724.bat config\my_724.ini   (any config file)
rem ============================================================================
setlocal
pushd "%~dp0"

set "CFG=%~1"
if "%CFG%"=="" set "CFG=config\config_simnow724.ini"
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
rem  (check done via stop_all.bat / the python harness; a filtered tasklist
rem   here would go through WMI which is blocked on locked-down machines)
if exist "%BIN%\md_proc.exe.lock" echo [WARN ] stale lock file found.

rem ---- cold start: wipe the shared-memory bus --------------------------------
echo [INFO ] resetting shared memory ...
"%BIN%\shm_reset.exe"
if errorlevel 1 (
    echo [ERROR] shm_reset failed.
    popd & exit /b 1
)

rem ---- pre-flight: warn outside 7x24 service hours ---------------------------
rem  7x24 serves 16:00 -> 09:00 (trading days) / 16:00 -> 12:00 (holidays).
python -c "import sys" >nul 2>&1
if not errorlevel 1 (
    python -c "import datetime,sys; t=datetime.datetime.now(); m=t.hour*60+t.minute; ok=(m>=960)or(m<540); sys.exit(0 if ok else 1)" >nul 2>&1
    if errorlevel 1 (
        echo [WARN ] Now is inside 09:00 - 16:00: the 7x24 environment is
        echo        typically NOT serving ^(it opens at 16:00^).
        echo        During the day use start_simnow.bat ^(set 1 fronts^) instead.
    )
)

echo [INFO ] config : %CFG%
echo [INFO ] flavour : standard CTP ^(%BIN%^)
echo [INFO ] fronts  : trade 40001 / md 40011 ^(SimNow 7x24, 看穿式前置^)
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
echo [ OK ] All four processes started against SimNow 7x24.
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
