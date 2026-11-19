@echo off
setlocal EnableExtensions

set "EDITOR_EXE=M:\Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
set "UPROJECT=F:\lshields\UnrealAA\UnrealAA\UnrealAA.uproject"

set "http_proxy="
set "https_proxy="
set "all_proxy="
set "HTTP_PROXY="
set "HTTPS_PROXY="
set "ALL_PROXY="
set "no_proxy=localhost,127.0.0.1,::1"
set "NO_PROXY=localhost,127.0.0.1,::1"

if not exist "%EDITOR_EXE%" (
  echo [AutoTest] ERROR: Missing UnrealEditor-Cmd.exe: %EDITOR_EXE%
  exit /b 2
)

if not exist "%UPROJECT%" (
  echo [AutoTest] ERROR: Missing project file: %UPROJECT%
  exit /b 2
)

"%EDITOR_EXE%" "%UPROJECT%" -unattended -nop4 -nullrhi -nosplash -log -DDC-ForceMemoryCache "-TestExit=Automation Test Queue Empty" -ExecCmds="Automation RunTests AAOriginal.WAD.L30"
exit /b %ERRORLEVEL%
