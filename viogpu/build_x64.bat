@echo off
setlocal
set MESA_PREFIX=C:\source\build-mesa\mesa-install-x64

rem Bootstrap the VS 2022 build environment (msbuild + WDK targets).
rem SetVsEnv.bat treats a non-empty %EnterpriseWDK% as "already initialized";
rem the WDK Telemetry task requires it to parse as System.Boolean.
if "%VsDevCmd%"=="" set VsDevCmd=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat
if not defined VSINSTALLDIR call "%VsDevCmd%" -arch=amd64 -host_arch=amd64
set EnterpriseWDK=true

call ..\build\build.bat viogpu3d\viogpu3d.vcxproj "Win10 Win11" Debug x64 %*
