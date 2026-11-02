@echo off

setlocal
REM this builds libjxl and replaces the libs in the JPEGView src folder

REM for me it takes like 15min to build
echo NOTE: this takes a LONG time to build, don't be alarmed...
echo       if it looks like the build process hung... it didn't, it's just WAY SLOW!

SET XSRC_DIR=%~dp0..\..\src
SET XLIB_DIR=%~dp0..\third_party\libjxl
SET XOUT_DIR=%~dp0libjxl

IF EXIST "%XOUT_DIR%" (
	echo libjxl output exists, please delete folder before trying to build
	exit /b 1
)

call :BUILD_COPY_JXL x86 Win32 ""
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_JXL x64 x64 "64"
IF ERRORLEVEL 1 exit /b 1




echo === HEADER FILES NOT MAINTAINED BY SCRIPT ===
echo NOTE: as for the header files, copy/replace files AS NEEDED
echo TO: src\JPEGView\libjxl\include\jxl
echo FROM: extras\third_party\libjxl\lib\include\jxl
echo FROM: %XOUT_DIR%\[arch]\lib\include\jxl

exit /b 0





:BUILD_COPY_JXL

REM so the environments don't pollute each other
setlocal

SET XBUILD_ARCH=%~1
SET XPLATFORM=%~2
SET XJPV_ARCH_PATH=%~3

SET XBUILD_DIR=%XOUT_DIR%\%XBUILD_ARCH%

mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %XBUILD_ARCH%

pushd "%XBUILD_DIR%"

REM jxl_dec is only the decode-only library; JPEGView links jxl (decode+encode,
REM which is what JXLWrapper writes files with) plus jxl_threads. Building
REM "jxl" also pulls in jxl_cms and the bundled highway/brotli.
REM
REM Everything else is switched off explicitly: the defaults turn on tools,
REM examples, benchmarks, doxygen, man pages, JNI, sjpeg and OpenEXR, none of
REM which we ship, and each one drags in extra configure-time probes and build
REM time. Viewers/plugins stay off so the bundled lcms submodule is not needed.
cmake.exe -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -A %XPLATFORM% ^
  -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_DEVTOOLS=OFF ^
  -DJPEGXL_ENABLE_BENCHMARK=OFF -DJPEGXL_ENABLE_EXAMPLES=OFF ^
  -DJPEGXL_ENABLE_DOXYGEN=OFF -DJPEGXL_ENABLE_MANPAGES=OFF ^
  -DJPEGXL_ENABLE_JNI=OFF -DJPEGXL_ENABLE_SJPEG=OFF ^
  -DJPEGXL_ENABLE_OPENEXR=OFF -DJPEGXL_ENABLE_VIEWERS=OFF ^
  -DJPEGXL_ENABLE_PLUGINS=OFF ^
  "%XLIB_DIR%"
IF ERRORLEVEL 1 exit /b 1
msbuild.exe /p:Platform=%XPLATFORM% /p:configuration="Release" LIBJXL.sln /t:jxl /t:jxl_threads
IF ERRORLEVEL 1 exit /b 1

popd

REM copy the libs over
REM 0.12 emits both the DLL and the import library into lib\<config>\ instead of
REM Release\ at the top of the build tree, so try the new place first and fall
REM back to the old one.
for %%D in ("%XBUILD_DIR%\lib\Release" "%XBUILD_DIR%\Release") do (
	if exist "%%~D\jxl.dll" (
		copy /y "%%~D\jxl*.dll" "%XSRC_DIR%\JPEGView\libjxl\bin%XJPV_ARCH_PATH%\"
		if errorlevel 1 exit /b 1
		copy /y "%%~D\jxl*.lib" "%XSRC_DIR%\JPEGView\libjxl\lib%XJPV_ARCH_PATH%\"
		if errorlevel 1 exit /b 1
		goto :JXL_COPIED
	)
)
echo could not find jxl.dll in the build tree
exit /b 1
:JXL_COPIED

for %%D in ("%XBUILD_DIR%\third_party\brotli\Release" "%XBUILD_DIR%\lib\Release") do (
	if exist "%%~D\brotli*.dll" (
		copy /y "%%~D\brotli*.dll" "%XSRC_DIR%\JPEGView\libjxl\bin%XJPV_ARCH_PATH%\"
		if errorlevel 1 exit /b 1
		goto :BROTLI_COPIED
	)
)
echo could not find brotli DLLs in the build tree
exit /b 1
:BROTLI_COPIED



exit /b 0
