@echo off
setlocal DisableDelayedExpansion
set "STP_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if defined PROCESSOR_ARCHITEW6432 set "STP_POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%STP_POWERSHELL%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build-windows.ps1" %*
set "STP_BUILD_EXIT=%ERRORLEVEL%"
echo.
echo STP Viewer build exit code: %STP_BUILD_EXIT%
echo Reports and logs: dist\windows-build-report.json and dist\validation\windows
pause
exit /b %STP_BUILD_EXIT%
