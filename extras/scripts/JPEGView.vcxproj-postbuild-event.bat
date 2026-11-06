@echo off

setlocal

REM The copying of DLLs was formerly the primary function of the Post-Build Event
REM but localization, generating, and copying things around from different places is necessary to avoid duplication

REM NOTE (O-09): a failed DLL copy now fails the build. Each COPY_DLLS call
REM is checked below and the script exits non-zero at once, so a missing DLL
REM can no longer slip into the release zip silently. The Config/LICENSE
REM xcopy steps above stay best-effort (unchanged); only the DLL copies are
REM fatal. (The old NOTE claiming "does not fail on error" was removed: it
REM described the pre-O-09 behaviour.)


IF /I "%~4" EQU "" (
	echo ERROR: This batch file is meant to be run from src\JPEGView vcxproj's Post-Build Event
	echo;
	echo    It requires VS Macro parameters to function properly.
	exit /b 1
)

REM ----------- VARIABLES FROM Visual Studio -----------
REM these match the order in the call from post-build event

REM 32 or 64
SET PLATFORM_ARCHITECTURE=%~1

REM base name of the project, to detect if we're building the VS2017 version
SET PROJECT_NAME=%~2

REM absolute path to src\JPEGView, with trailing "\"
SET PROJECT_DIR=%~3

REM absolute path to the output dir (target to copy to) with trailing backslash
SET OUT_DIR_FULL_PATH=%~4


REM print the output
echo ----- Post-Build batch file: %0 -----
echo %% PLATFORM_ARCHITECTURE=%PLATFORM_ARCHITECTURE%
echo %% PROJECT_NAME=%PROJECT_NAME%
echo %% PROJECT_DIR=%PROJECT_DIR%
echo %% OUT_DIR_FULL_PATH=%OUT_DIR_FULL_PATH%



REM ----------- COPY COMMON CONFIG FILES -----------
REM these are common across all builds, regardless of architecture or version
echo + XCopy Config\* ...
xcopy "%PROJECT_DIR%Config\*" "%OUT_DIR_FULL_PATH%" /Y /C /D
echo ~ ErrorLevel: %ErrorLevel%

echo + XCopy LICENSE.txt ...
xcopy "%PROJECT_DIR%..\..\LICENSE.txt" "%OUT_DIR_FULL_PATH%" /Y /D
echo ~ ErrorLevel: %ErrorLevel%


REM ----------- COPY DLL FILES -----------
REM qbnu standardized JPEGView's DLL structure which puts DLLs in either the bin (32-bit) or bin64 (64-bit) folders

IF /I "%PROJECT_NAME%" EQU "JPEGView_VS2017" (
	REM specifically for VS2017:  Currently, VS2017 project file is used to build the XP-version ONLY
	REM the XP version does NOT support any of the new DLLs at this time
	exit /b 0
)

REM all other projects files get DLLs...
REM O-09: each copy is checked; the first failure exits non-zero at once
REM and no further DLL is copied, so msbuild reports the build as failed.
call :COPY_DLLS libjxl
if errorlevel 1 exit /b 1
call :COPY_DLLS libheif
if errorlevel 1 exit /b 1
call :COPY_DLLS libavif
if errorlevel 1 exit /b 1
call :COPY_DLLS lcms2
if errorlevel 1 exit /b 1
call :COPY_DLLS libraw
if errorlevel 1 exit /b 1
call :COPY_DLLS libopenjpeg
if errorlevel 1 exit /b 1
call :COPY_DLLS libthorvg
if errorlevel 1 exit /b 1


exit /b 0




REM ================================================================================

:COPY_DLLS

setlocal

SET DEP_NAME=%~1

REM set default as 32-bit version
SET SRC_DIR=%PROJECT_DIR%%DEP_NAME%\bin
IF /I "%PLATFORM_ARCHITECTURE%" EQU "64" (
	REM append "64" to the 32-bit version path
	SET SRC_DIR=%SRC_DIR%%PLATFORM_ARCHITECTURE%
)

echo + XCopy %PLATFORM_ARCHITECTURE%-bit DLLs for "%DEP_NAME%" if needed ...
xcopy "%SRC_DIR%\*.dll" "%OUT_DIR_FULL_PATH%" /Y /D
REM O-09: capture xcopy's exit code at once. The echo below resets
REM ErrorLevel to 0, so reading it afterwards would always report success
REM (that is why a failed copy used to pass silently: the old
REM "exit /b %ErrorLevel%" at the end of this subroutine always saw 0).
SET XCOPY_ERR=%ErrorLevel%
echo ~ ErrorLevel: %XCOPY_ERR%
REM xcopy exit codes (Microsoft Learn): 0 copied OK, 1 no files found to
REM copy, 2 Ctrl+C, 4 init error, 5 disk write error.
REM Exit code 1 is ambiguous with /D: it also means "nothing was newer to
REM copy" when the destination DLLs are already up to date, which is a
REM successful incremental build, not a failure. Disambiguate by checking
REM whether the source folder has any DLL at all: if it has, code 1 only
REM means "already up to date"; if it has none, the lib folder is missing
REM its DLLs and the build must fail.
if %XCOPY_ERR% equ 0 exit /b 0
if %XCOPY_ERR% geq 2 (
	echo ERROR: Failed to copy %PLATFORM_ARCHITECTURE%-bit DLLs for "%DEP_NAME%" ^(xcopy exit code %XCOPY_ERR%^)
	exit /b %XCOPY_ERR%
)
dir /b /a-d "%SRC_DIR%\*.dll" >nul 2>&1
if errorlevel 1 (
	echo ERROR: No DLLs found for "%DEP_NAME%" in "%SRC_DIR%" - nothing was copied
	exit /b 1
)
REM Source DLLs exist but none needed copying: already up to date.
exit /b 0

REM ================================================================================
