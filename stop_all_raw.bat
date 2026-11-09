@echo off
rem ============================================================================
rem  Stop every ctp_hft process.
rem ============================================================================
echo stopping ctp_hft ...
taskkill /F /IM strategy_proc.exe >nul 2>&1
taskkill /F /IM query_proc.exe    >nul 2>&1
taskkill /F /IM trader_proc.exe   >nul 2>&1
taskkill /F /IM md_proc.exe       >nul 2>&1
taskkill /F /IM monitor.exe       >nul 2>&1
echo done.
ping -n 3 127.0.0.1 >nul
exit /b 0
