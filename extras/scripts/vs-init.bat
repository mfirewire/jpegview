@echo off

REM this is a routine that would detect and initialize visual studio environment.
REM It's not going to be tested very well for new versions because I only have VS2017 and VS2019 Community installed

REM if a special variable comes in, it'll use that version XVS_INIT_VER=

IF /I "%~1" EQU "" (
	echo ERROR: Pass in an [arch] to be passed to a vcvarsall.bat
	exit /b 1
)

REM 2017/2019 install under "Program Files (x86)", 2022+ under "Program Files"
SET XVS_BASE_COMMON=%ProgramFiles(x86)%\Microsoft Visual Studio
IF NOT EXIST "%XVS_BASE_COMMON%" SET XVS_BASE_COMMON=%ProgramFiles%\Microsoft Visual Studio

IF /I "%XVS_INIT_VER%" EQU "" GOTO SKIP_VER
	REM need to do it this way so that () chars don't cause a syntax error
	SET XVS_BASE_COMMON=%XVS_BASE_COMMON%\%XVS_INIT_VER%
:SKIP_VER

IF NOT EXIST "%XVS_BASE_COMMON%" (
	echo Visual Studio not installed or installed in a non-standard location!
	exit /b 1
)

SET XVS_VCVARS_BAT=

REM Prefer vswhere: shipped with every VS 2017+, and it reports the real
REM installation path, so it does not care whether VS lives under
REM "Program Files" or "Program Files (x86)".
SET XVS_WHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
IF EXIST "%XVS_WHERE%" (
	FOR /F "usebackq tokens=*" %%I IN (`"%XVS_WHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) DO (
		IF EXIST "%%I\VC\Auxiliary\Build\vcvarsall.bat" (
			echo + Found VCVARSALL.BAT via vswhere: %%I\VC\Auxiliary\Build\vcvarsall.bat
			SET XVS_VCVARS_BAT=%%I\VC\Auxiliary\Build\vcvarsall.bat
		)
	)
)

REM Fallback for installations without vswhere: recursive search under the base
IF "%XVS_VCVARS_BAT%"=="" (
	FOR /F "usebackq tokens=*" %%I IN (`dir /b /on /s "%XVS_BASE_COMMON%\vcvarsall.bat" 2^>nul`) DO (
		echo + Found VCVARSALL.BAT at: %%I
		SET XVS_VCVARS_BAT=%%I
	)
)

IF "%XVS_VCVARS_BAT%"=="" (
	echo ERROR: vcvarsall.bat not found, cannot set up the build environment
	exit /b 1
)


echo == Using: %XVS_VCVARS_BAT% ==
call "%XVS_VCVARS_BAT%" %~1



SET XVS_BASE_COMMON=
SET XVS_VCVARS_BAT=


exit /b 0
