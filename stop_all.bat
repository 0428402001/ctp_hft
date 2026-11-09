@echo off
rem ============================================================================
rem  ctp_hft - STOP everything and KEEP it stopped.
rem
rem  Why the old version did not work: the always-on supervisor
rem  (task ctp_hft_daemon -> guard_daemon.py) repairs the process tree within
rem  20-40 s whenever a process disappears, so a plain "taskkill" is undone
rem  almost immediately.  The real off switch is the file flow\.halt: while it
rem  exists the supervisor logs "HALT file present" and repairs nothing.
rem
rem  Usage:
rem      stop_all.bat              stop the four processes; stay stopped
rem      stop_all.bat --daemon     ALSO stop + disable the supervisor task
rem                                itself - full manual mode, nothing will
rem                                ever restart anything by itself
rem
rem  Resume afterwards:
rem      start_simnow.bat          day front  - clears flow\.halt automatically
rem      start_simnow724.bat       724 front  - same
rem      or manual:                del flow\.halt   (supervisor re-arms itself)
rem      if --daemon was used:     schtasks /Change /TN ctp_hft_daemon /ENABLE
rem                                then start_simnow.bat
rem
rem  WARNING: stopping with an open position leaves it NAKED - no stop loss,
rem  no escape logic, nothing watching it.  The position check runs first.
rem ============================================================================
setlocal
pushd "%~dp0"

echo ============================================================
echo  ctp_hft - full stop ^(supervision disabled until resume^)
echo ============================================================
echo.

rem ---- position check: stopping with inventory is dangerous ----------------
if exist "build\bin_simnow\account_peek.exe" (
    echo [INFO ] position snapshot from shared memory:
    "build\bin_simnow\account_peek.exe" 2>nul | findstr /I /C:"positions" /C:"net_today"
    "build\bin_simnow\account_peek.exe" 2>nul | findstr /I /C:"positions" | findstr /C:": 0" >nul
    if errorlevel 1 (
        echo.
        echo [WARN ] POSITION MAY NOT BE FLAT.  Stopping now leaves it naked
        echo         ^(no stop loss, no escape, no watcher^).  Ctrl+C to abort ...
        ping -n 8 127.0.0.1 >nul
        echo.
    ) else (
        echo [ OK ] flat - safe to stop.
    )
    echo.
)

rem ---- freeze the supervisor FIRST ------------------------------------------
rem  guard_daemon checks flow\.halt at the start of every 20 s tick AND once
rem  more right before it acts, so raising the switch and killing the
rem  processes in the same breath is race-free.
if not exist "flow" mkdir "flow"
echo. > "flow\.halt"
if not exist "flow\.halt" (
    echo [ERROR] cannot write flow\.halt - the supervisor would restart
    echo         everything within seconds.  Aborting without killing.
    popd & exit /b 1
)
echo [ OK ] stop switch raised ^(flow\.halt^) - the supervisor will not repair.

taskkill /F /IM strategy_proc.exe >nul 2>&1
taskkill /F /IM query_proc.exe    >nul 2>&1
taskkill /F /IM trader_proc.exe   >nul 2>&1
taskkill /F /IM md_proc.exe       >nul 2>&1
taskkill /F /IM monitor.exe       >nul 2>&1
echo [ OK ] processes killed.

rem ---- optional: take the supervisor itself down ----------------------------
if /i "%~1"=="--daemon" (
    echo [INFO ] stopping + disabling the supervisor task ctp_hft_daemon ...
    schtasks /End /TN ctp_hft_daemon >nul 2>&1
    schtasks /Change /TN ctp_hft_daemon /DISABLE >nul 2>&1
    if errorlevel 1 (
        echo [WARN ] could not stop or disable the scheduled task - try an
        echo         admin console or Task Scheduler GUI ^(disable ctp_hft_daemon^).
    ) else (
        echo [ OK ] supervisor stopped and disabled.
        echo        re-enable later with:
        echo        schtasks /Change /TN ctp_hft_daemon /ENABLE
    )
)

rem ---- prove it stays down ---------------------------------------------------
echo [INFO ] waiting 25 s to prove nothing respawns ...
ping -n 26 127.0.0.1 >nul
tasklist /FI "IMAGENAME eq strategy_proc.exe" 2>nul | findstr /i /c:"strategy_proc.exe" >nul
if errorlevel 1 (
    echo [ OK ] nothing respawned - ctp_hft is DOWN and stays down.
) else (
    echo [WARN ] a process came back - the halt switch was not honoured.
    echo        check log\guard_daemon.log for what the supervisor did.
)

echo.
echo to resume:  start_simnow.bat   or   start_simnow724.bat
echo             ^(both clear flow\.halt automatically^)
popd
exit /b 0
