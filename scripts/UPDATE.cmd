@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -Command "& '%~dp0UPDATE.ps1' %*"
exit /b %ERRORLEVEL%
