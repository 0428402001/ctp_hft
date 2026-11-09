@echo off
rem ============================================================================
rem  ctp_hft - SimNow 7x24 launcher (SAFE wrapper)
rem
rem  This file no longer starts the four processes directly.  It hands the job
rem  to tools\guard.py, which:
rem    * first checks whether the strategy is already up (it usually is - the
rem      keeper automation restarts it, so re-running shm_reset.exe here used to
rem      fail with "[ERROR] shm_reset failed" and exit before starting anything),
rem    * picks the right front group for the current clock
rem        09:00 - 16:00  -> day front  30001 / 30011 (config_simnow.ini)
rem        otherwise      -> 724 front  40001 / 40011 (config_simnow724.ini)
rem    * cold-starts through the scheduled task "ctp_hft_launch", so the
rem      processes are created by the Task Scheduler service and survive after
rem      this console window closes (processes spawned from a console die with
rem      its job object within seconds),
rem    * refuses to reconnect while SimNow is throttling us (reason=4097):
rem      the throttle keeps a 180 s cooldown and max 3 restarts per 15 min.
rem
rem  The old direct-start logic is preserved in start_simnow724_raw.bat for
rem  reference only.  Do NOT run it while anything is live: its shm_reset.exe
rem  wipes the shared-memory bus the running processes talk over.
rem
rem  Usage:
rem      start_simnow724.bat              start it, or report that it is up
rem      start_simnow724.bat --status     report only, change nothing
rem      start_simnow724.bat --force      restart even inside the cooldown
rem ============================================================================
setlocal
pushd "%~dp0"

echo ============================================================
echo  ctp_hft / SimNow 7x24  -  launcher
echo ============================================================
echo.

where python >nul 2>&1
if errorlevel 1 (
    echo [ERROR] python not found in PATH.
    echo         guard.py needs Python 3 - install it or add it to PATH.
    echo.
    if "%~1"=="" pause
    popd & exit /b 1
)

python "%~dp0tools\guard.py" --env 724 %*
set "RC=%ERRORLEVEL%"

echo.
if not "%RC%"=="0" (
    echo [WARN ] not healthy - see log\launcher.log and log\strategy.log
    echo        If it says "restart refused", just wait: SimNow kicks a client
    echo        that reconnects too often and the account stays locked for a
    echo        few minutes.
) else (
    echo [ OK ] ctp_hft is up on the SimNow 7x24 front.
    echo        logs : log\md.log  log\trader.log  log\query.log  log\strategy.log
    echo        this window can be closed - the processes are owned by the
    echo        always-on supervisor "guard_daemon.py" ^(task ctp_hft_daemon^).
)
echo.

if "%~1"=="" pause
popd
exit /b %RC%
