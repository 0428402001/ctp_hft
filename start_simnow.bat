@echo off
rem ============================================================================
rem  ctp_hft - SimNow day-session launcher (SAFE wrapper)
rem
rem  Same contract as start_simnow724.bat, only the environment differs:
rem
rem    start_simnow.bat                 day front  30001 / 30011  (09:00-16:00)
rem    start_simnow724.bat              724 front  40001 / 40011  (16:00-09:00)
rem
rem  This file no longer starts the four processes itself.  It hands the job to
rem  tools\guard.py --env day, which:
rem    * first checks whether the strategy is already up instead of blindly
rem      running shm_reset.exe (that reset used to wipe the shared-memory bus of
rem      a healthy running tree and then fail with "shm_reset failed"),
rem    * reports what is actually alive, what is missing, and which front the
rem      clock says should be live,
rem    * hands the cold start to the always-on supervisor (guard_daemon.py,
rem      task ctp_hft_daemon), so the four processes are NOT children of this
rem      console and keep running after the window is closed,
rem    * refuses to reconnect while SimNow is throttling us (reason=4097):
rem      180 s cooldown and at most 3 restarts per 15 minutes.
rem
rem  Usage:
rem      start_simnow.bat                  start/verify the day front
rem      start_simnow.bat --status         report only, change nothing
rem      start_simnow.bat --force          restart even inside the cooldown
rem      start_simnow.bat --strategy-only  swap strategy_proc.exe only
rem                                        (keeps the CTP sessions - no re-login)
rem
rem  Outside 09:00-16:00 the day front does not serve logins; guard.py will
rem  refuse and say so.  After 16:00 use start_simnow724.bat.
rem
rem  Policy / parameters live in config\config_simnow.ini - this script never
rem  takes a config path any more (guard.py always launches that one file).
rem
rem  The old direct-start logic is preserved in start_simnow_raw.bat for
rem  reference only.  Do NOT run it while anything is live: its shm_reset.exe
rem  wipes the bus the running processes talk over.
rem ============================================================================
setlocal
pushd "%~dp0"

echo ============================================================
echo  ctp_hft / SimNow day session  -  launcher
echo ============================================================
echo.

if /i "%~x1"==".ini" (
    echo [ERROR] passing a config path is no longer supported.
    echo         guard.py always launches config\config_simnow.ini for the day
    echo         front.  Edit that file to change parameters, or use the raw
    echo         script start_simnow_raw.bat for a throw-away manual run.
    echo.
    if "%~1"=="" pause
    popd & exit /b 2
)

where python >nul 2>&1
if errorlevel 1 (
    echo [ERROR] python not found in PATH.
    echo         guard.py needs Python 3 - install it or add it to PATH.
    echo.
    if "%~1"=="" pause
    popd & exit /b 1
)

python "%~dp0tools\guard.py" --env day %*
set "RC=%ERRORLEVEL%"

echo.
if not "%RC%"=="0" (
    echo [WARN ] not healthy - see log\launcher.log and log\strategy.log
    echo        "restart refused" means SimNow is throttling: wait a few minutes.
    echo        If it says the day front is not serving, use start_simnow724.bat.
) else (
    echo [ OK ] ctp_hft is up on the SimNow day front ^(30001/30011^).
    echo        logs : log\md.log  log\trader.log  log\query.log  log\strategy.log
    echo        watch: watch_log.bat           ^(strategy: signals, fills, PnL^)
    echo               watch_log.bat all       ^(strategy + trader^)
    echo               watch_log.bat errors    ^(WRN / ERR only^)
    echo        this window can be closed - the processes are owned by the
    echo        always-on supervisor "guard_daemon.py" ^(task ctp_hft_daemon^).
)
echo.

if "%~1"=="" pause
popd
exit /b %RC%
