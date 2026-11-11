@echo off
rem  Temporary harness: simulation-only end-to-end check (no broker, no network).
rem  Proves signal -> order -> fill -> bracket exit -> round trip in one run.
setlocal
cd /d "%~dp0"
set CFG=config\config_simfill.ini
set BIN=build\bin

"%BIN%\shm_reset.exe" >nul 2>&1
start /b "" "%BIN%\md_proc.exe" --config %CFG%
ping -n 3 127.0.0.1 >nul
start /b "" "%BIN%\trader_proc.exe" --config %CFG%
ping -n 3 127.0.0.1 >nul
start /b "" "%BIN%\query_proc.exe" --config %CFG%
ping -n 4 127.0.0.1 >nul
start /b "" "%BIN%\strategy_proc.exe" --config %CFG%
ping -n 150 127.0.0.1 >nul

taskkill /f /im strategy_proc.exe >nul 2>&1
taskkill /f /im query_proc.exe >nul 2>&1
taskkill /f /im trader_proc.exe >nul 2>&1
taskkill /f /im md_proc.exe >nul 2>&1
echo HARNESS_DONE
