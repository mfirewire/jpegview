@echo off

setlocal
REM this builds libjpeg-turbo and replaces the libs in the JPEGView src folder

SET XSRC_DIR=%~dp0..\..\src
SET XLIB_DIR=%~dp0..\third_party\libjpeg-turbo
SET XOUT_DIR=%~dp0libjpeg-turbo

IF EXIST "%XOUT_DIR%" (
	echo libjpeg-turbo output exists, please delete folder before trying to build
	exit /b 1
)

echo + Looking up NASM
where nasm.exe
IF ERRORLEVEL 1 (
	echo !!! NASM isn't required for building but highly recommended for speed improvements. !!!
	echo NASM isn't found on path!
	IF /I "%~1" NEQ "nonasm" (
		echo pass in parameter "nonasm" to skip this check
		exit /b 1
	)
)

call :BUILD_COPY_JPEGT x86 lib
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_JPEGT x64 lib64
IF ERRORLEVEL 1 exit /b 1




echo === HEADER FILES NOT MAINTAINED BY SCRIPT ===
echo NOTE: as for the header files, copy/replace files AS NEEDED
echo TO: src\JPEGView\libjpeg-turbo\include
echo FROM: extras\third_party\libjpeg-turbo
echo FROM: jconfig.h is in output directory


exit /b 0





:BUILD_COPY_JPEGT

REM so the environments don't pollute each other
setlocal

SET XBUILD_DIR=%XOUT_DIR%\%1

REM 3.2.x embeds its own zlib for the PNG paths, and zutil.c.obj then defines
REM z_errmsg a second time next to the zlib.lib JPEGView already links
REM (LNK2005: z_errmsg already defined in turbojpeg-static.lib). Point it at the
REM zlib we already ship instead - same /MT build, built in batch 1.
SET XZLIB_LIB=%XSRC_DIR%\JPEGView\libpng-apng\%2\zlib.lib
SET XZLIB_INC=%~dp0..\third_party\libpng-apng.src-patch\zlib

mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %1

pushd "%XBUILD_DIR%"
REM Only the static TurboJPEG library is linked into JPEGView, so skip the DLL
REM to halve the build time. WITH_CRT_DLL=OFF keeps the static CRT (/MT) that
REM JPEGView itself uses; mixing /MT and /MD inside one process is the crash we
REM already had to fix once for libpng.
REM WITH_SYSTEM_ZLIB=ON stops the in-tree zlib objects from being compiled into
REM turbojpeg-static.lib, which is what causes the z_errmsg clash. libspng stays
REM in-tree (it has no conflict - nothing else links spng) and resolves zlib
REM through the paths passed below.
cmake.exe -G"NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_CRT_DLL=OFF -DWITH_SYSTEM_ZLIB=ON -DZLIB_LIBRARY="%XZLIB_LIB%" -DZLIB_INCLUDE_DIR="%XZLIB_INC%" "%XLIB_DIR%"
IF ERRORLEVEL 1 exit /b 1
nmake.exe
IF ERRORLEVEL 1 exit /b 1

popd

REM copy the libs over
REM error checking if a copy fails... throws error to caller
copy /y "%XBUILD_DIR%\turbojpeg-static.lib" "%XSRC_DIR%\JPEGView\libjpeg-turbo\%~2\"
IF ERRORLEVEL 1 exit /b 1



exit /b 0
