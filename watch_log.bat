@echo off
rem ============================================================================
rem  ctp_hft - live log viewer
rem
rem  The processes run with --no-console (they must: a recycled console used to
rem  block the logger and freeze the strategy), so nothing prints to a window.
rem  This script follows the logs the way `tail -f` does on Linux.
rem
rem  It uses tools\tailf.py (Python), NOT tail.exe: the uutils tail.exe printed
rem  the initial lines and then went silent when attached to a real console, so
rem  the view "did not refresh".  tailf.py polls the files itself and flushes
rem  every line explicitly - reliable on any console - and it decodes each line
rem  as UTF-8 with a GBK fallback, so trader.log's Chinese error text shows up
rem  correctly even under chcp 65001.
rem
rem  Usage:
rem      watch_log.bat              follow strategy.log (signals, fills, PnL)
rem      watch_log.bat trader       follow trader.log (order reports, errors)
rem      watch_log.bat md           follow md.log     (market data / logins)
rem      watch_log.bat query        follow query.log  (positions, fees)
rem      watch_log.bat all          follow strategy + trader together
rem      watch_log.bat errors       follow strategy.log, only warnings/errors
rem      watch_log.bat 200          history lines for a single log (default 30)
rem
rem  Ctrl+C stops the view.  It does NOT touch the trading processes.
rem ============================================================================
setlocal
chcp 65001 >nul 2>&1
title ctp_hft live log - %~1

where python >nul 2>&1
if errorlevel 1 (
    echo [ERROR] python not found in PATH - tailf.py needs Python 3.
    pause
    exit /b 1
)
set "PYTHONIOENCODING=utf-8:replace"

set "WHAT=%~1"
set "HIST=-n 30"
if "%WHAT%"=="" set "WHAT=strategy"
rem  numeric first argument -> history line count
echo %WHAT%| findstr /R "^[0-9][0-9]*$" >nul
if not errorlevel 1 (
    set "HIST=-n %WHAT%"
    set "WHAT=%~2"
    if "%WHAT%"=="" set "WHAT=strategy"
)
set "LOGDIR=%~dp0log"

if /i "%WHAT%"=="all" (
    echo [INFO ] following strategy.log + trader.log  ^(Ctrl+C to stop^)
    echo.
    python "%~dp0tools\tailf.py" %HIST% "%LOGDIR%\strategy.log" "%LOGDIR%\trader.log"
    goto :done
)

if /i "%WHAT%"=="errors" (
    echo [INFO ] strategy.log - WRN/ERR lines only  ^(Ctrl+C to stop^)
    echo.
    python "%~dp0tools\tailf.py" -n 200 --grep "\[WRN\]|\[ERR\]|ORDER ERROR|ROUND TRIP" "%LOGDIR%\strategy.log"
    goto :done
)

if not exist "%LOGDIR%\%WHAT%.log" (
    echo [ERROR] no such log: %LOGDIR%\%WHAT%.log
    echo         valid names: strategy  trader  md  query  all  errors
    echo         or a history count:    watch_log.bat 200 trader
    pause
    exit /b 1
)

echo [INFO ] following %WHAT%.log  ^(Ctrl+C to stop^)
echo.
python "%~dp0tools\tailf.py" %HIST% "%LOGDIR%\%WHAT%.log"

:done
endlocal
