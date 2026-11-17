@echo off
setlocal enabledelayedexpansion

rem Builds the viogpu drivers from viogpu.sln.
rem
rem Usage: build.bat [driver...] [arch...] [os...] [Debug^|Release] [/Build^|/Rebuild^|/Clean]
rem
rem   driver   viogpu3d, viogpudo or all            (default: all)
rem   arch     x86, x64 or ARM64                    (default: host architecture)
rem   os       Win10 or Win11                       (default: Win10 Win11)
rem
rem MSBuild is taken from PATH when it is there (EWDK or a Developer Command
rem Prompt); otherwise the newest Visual Studio with MSBuild is located with
rem vswhere. The WDK must be installed for that Visual Studio.

set "ROOT=%~dp0"
set "DRIVERS="
set "ARCHS="
set "OSES="
set "FLAVOR=Release"
set "ACTION=Build"

:parse
if "%~1"=="" goto parsed
set "ARG=%~1"
if /I "%ARG%"=="viogpu3d" set "DRIVERS=!DRIVERS! viogpu3d" & goto next
if /I "%ARG%"=="viogpudo" set "DRIVERS=!DRIVERS! viogpudo" & goto next
if /I "%ARG%"=="all"      set "DRIVERS=!DRIVERS! all" & goto next
if /I "%ARG%"=="x86"      set "ARCHS=!ARCHS! x86" & goto next
if /I "%ARG%"=="x64"      set "ARCHS=!ARCHS! x64" & goto next
if /I "%ARG%"=="amd64"    set "ARCHS=!ARCHS! x64" & goto next
if /I "%ARG%"=="ARM64"    set "ARCHS=!ARCHS! ARM64" & goto next
if /I "%ARG%"=="Win10"    set "OSES=!OSES! Win10" & goto next
if /I "%ARG%"=="Win11"    set "OSES=!OSES! Win11" & goto next
if /I "%ARG%"=="Debug"    set "FLAVOR=Debug" & goto next
if /I "%ARG%"=="Release"  set "FLAVOR=Release" & goto next
if /I "%ARG%"=="/Build"   set "ACTION=Build" & goto next
if /I "%ARG%"=="/Rebuild" set "ACTION=Rebuild" & goto next
if /I "%ARG%"=="/Clean"   set "ACTION=Clean" & goto next
if /I "%ARG%"=="/?"       goto usage
if /I "%ARG%"=="-h"       goto usage
if /I "%ARG%"=="--help"   goto usage
echo error: unknown argument "%ARG%"
goto usage_error
:next
shift
goto parse

:parsed
if not defined DRIVERS set "DRIVERS=all"

set "HOST_ARCH=%PROCESSOR_ARCHITECTURE%"
if defined PROCESSOR_ARCHITEW6432 set "HOST_ARCH=%PROCESSOR_ARCHITEW6432%"

rem The WDK's InfVerif and ApiValidator steps load x86-only binaries that fail
rem on ARM64 hosts after a successful compile and link, which aborts packaging.
set "EXTRA_PROPS="
if /I "%HOST_ARCH%"=="ARM64" set "EXTRA_PROPS=-p:SkipPackageVerification=true -p:ApiValidator_Enable=false"

if not defined ARCHS (
  if /I "!HOST_ARCH!"=="AMD64" set "ARCHS=x64"
  if /I "!HOST_ARCH!"=="ARM64" set "ARCHS=ARM64"
  if /I "!HOST_ARCH!"=="x86"   set "ARCHS=x86"
  if not defined ARCHS (
    echo error: cannot map host architecture "!HOST_ARCH!", pass x86, x64 or ARM64
    exit /b 1
  )
)

if not defined OSES set "OSES=Win10 Win11"

call :find_msbuild
if errorlevel 1 exit /b 1
echo Using "%MSBUILD%"

set "FAILED="
for %%o in (%OSES%) do (
  for %%a in (%ARCHS%) do (
    for %%d in (%DRIVERS%) do call :build %%d %%o %%a
  )
)

echo.
if defined FAILED (
  echo FAILED:!FAILED!
  exit /b 1
)
echo All targets succeeded.
exit /b 0

rem %1 driver, %2 OS, %3 platform
:build
set "TARGET=%~1"
if /I not "%ACTION%"=="Build" set "TARGET=%~1:%ACTION%"
if /I "%~1"=="all" set "TARGET=%ACTION%"
set "LOG=%ROOT%build_%~1_%~2_%~3_%FLAVOR%.log"
echo.
echo ==== %ACTION% %~1 ^| %~2 %FLAVOR% ^| %~3 ====
"%MSBUILD%" "%ROOT%viogpu.sln" -nologo -maxCpuCount -verbosity:minimal ^
  "-t:%TARGET%" "-p:Configuration=%~2 %FLAVOR%" "-p:Platform=%~3" %EXTRA_PROPS% ^
  "-flp:LogFile=%LOG%;Verbosity=normal"
if errorlevel 1 (
  set "FAILED=!FAILED! %~1/%~2/%~3"
  echo ==== FAILED, see !LOG! ====
)
exit /b 0

:find_msbuild
set "MSBUILD="
for /f "delims=" %%i in ('where msbuild.exe 2^>nul') do if not defined MSBUILD set "MSBUILD=%%i"
if defined MSBUILD exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vswhere
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
  if not defined MSBUILD set "MSBUILD=%%i"
)
if defined MSBUILD exit /b 0
echo error: no Visual Studio installation with MSBuild was found
exit /b 1
:no_vswhere
echo error: msbuild.exe is not on PATH and vswhere.exe was not found at
echo   "%VSWHERE%"
echo Install Visual Studio 2022 with the WDK, or run from an EWDK or Developer Command Prompt.
exit /b 1

:usage
echo Usage: build.bat [driver...] [arch...] [os...] [Debug^|Release] [/Build^|/Rebuild^|/Clean]
echo.
echo   driver   viogpu3d, viogpudo or all            (default: all)
echo   arch     x86, x64 or ARM64                    (default: host architecture)
echo   os       Win10 or Win11                       (default: Win10 Win11)
echo.
echo Example: build.bat viogpu3d ARM64 Win11 Debug
exit /b 0
:usage_error
echo Run "build.bat /?" for usage.
exit /b 1
