@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -Command "& '%~dp0BUILD-LINUX.ps1' %*"
exit /b %ERRORLEVEL%
