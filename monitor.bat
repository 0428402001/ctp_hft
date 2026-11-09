@echo off
rem ============================================================================
rem  Attach the read-only monitor to the running system.
rem ============================================================================
setlocal
pushd "%~dp0"
if not exist "build\bin\monitor.exe" (
    echo [ERROR] monitor.exe not found - build first ^(build.bat or build_mingw.bat^)
    popd & exit /b 1
)
build\bin\monitor.exe config\config.ini 1000
popd
exit /b 0
