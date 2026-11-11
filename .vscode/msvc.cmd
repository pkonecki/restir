@echo off
rem Runs the given command with the MSVC x64 environment loaded, which Ninja
rem builds need for the compiler and for Ninja itself. Visual Studio (or the
rem Build Tools) is found with vswhere, so no install path is hard-coded.
setlocal

if defined VSCMD_VER goto run

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
	echo vswhere.exe was not found; install Visual Studio or the Build Tools. 1>&2
	exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
	echo No Visual Studio installation with the x64 C++ tools was found. 1>&2
	exit /b 1
)

rem vcvars64 prints a harmless "vswhere.exe is not recognized" when vswhere
rem is not on PATH, so its output is hidden; failures still set errorlevel.
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
	echo Loading the MSVC environment from "%VSINSTALL%" failed. 1>&2
	exit /b 1
)

:run
%*
