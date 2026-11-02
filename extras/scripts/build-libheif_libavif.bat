@echo off

setlocal
REM this builds libheif and replaces the libs in the JPEGView src folder
REM it also builds libavif as both share the same dav1d libs

SET XSRC_DIR=%~dp0..\..\src\JPEGView\libheif
SET XLIB_DIR=%~dp0..\third_party\libheif-1.23.7-src
SET XLIBAVIF_DIR=%~dp0..\third_party\libavif
SET XAVIFSRC_DIR=%~dp0..\..\src\JPEGView\libavif
SET XOUT_DIR=%~dp0libheif_libavif

SET XVENV_DIR=%~dp0venv-meson

IF EXIST "%XOUT_DIR%" (
	echo libheif output exists, please delete folder before trying to build
	exit /b 1
)

echo + Looking up NASM
where nasm.exe
IF ERRORLEVEL 1 (
	echo NASM isn't found on path!
	exit /b 1
)

echo + Looking up Python
where python.exe
python -V
IF ERRORLEVEL 1 (
	echo Python isn't found on path!
	exit /b 1
)

echo + Create VENV
python.exe -m venv "%XVENV_DIR%"

echo + Activate VENV
call "%XVENV_DIR%\Scripts\activate.bat"

echo + Install Meson and Ninja inside VENV
python.exe -m pip install -U pip
python.exe -m pip install -U meson ninja


REM libavif only depends on dav1d
REM libheif depends on dav1d and libde265
REM libheif v1.23.7 + libde265 v1.1.3 sources in extras/third_party/libheif-1.23.7-src and libde265-1.1.3-src

call :BUILD_COPY_DAV1D x86 ""
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBAVIF x86 ""
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBDE x86 Win32 ""
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBHEIF x86 Win32 ""
IF ERRORLEVEL 1 exit /b 1


call :BUILD_COPY_DAV1D x64 "64"
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBAVIF x64 "64"
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBDE x64 x64 "64"
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_LIBHEIF x64 x64 "64"
IF ERRORLEVEL 1 exit /b 1




echo === HEADER FILES NOT MAINTAINED BY SCRIPT ===
echo NOTE: as for the header files, copy/replace files AS NEEDED
echo TO: src\JPEGView\libheif\include\libheif
echo .h FROM: extras\third_party\libheif\libheif\libheif
echo FROM: %OUT_DIR%\libheif-[arch]\libheif\heif_version.h

echo;
echo TO: src\JPEGView\libheif\include\libavif
echo .h FROM: extras\third_party\libavif\include\avif

exit /b 0




REM ===============================================================================================

:BUILD_COPY_DAV1D

REM so the environments don't pollute each other
setlocal

SET XBUILD_DIR=%XOUT_DIR%\dav1d-%1\build
SET XDIST_DIR=%XOUT_DIR%\dav1d-%1\dist

mkdir "%XBUILD_DIR%" 2>nul
mkdir "%XDIST_DIR%" 2>nul

call "%~dp0vs-init.bat" %1

REM dav1d lives in its own submodule, not inside the libheif source drop
pushd "%~dp0..\third_party\libheif\dav1d"

REM or use --reconfigure flag
REM rd /s /q "%XLIB_DIR%\dav1d\build" 2>nul

REM part of the commands came from dav1d.cmd in libheif third-party


REM meson.exe from pip install venv\scripts
REM if you want to build static DLL do this:  meson setup --default-library=static --buildtype release --prefix "%XDIST_DIR%" "%XBUILD_DIR%"
meson setup --buildtype release --prefix "%XDIST_DIR%" "%XBUILD_DIR%"
IF ERRORLEVEL 1 exit /b 1

REM ninja.exe from pip install venv\scripts
ninja -C "%XBUILD_DIR%"
IF ERRORLEVEL 1 exit /b 1
REM libheif building requires things to be "installed" to work
ninja -C "%XBUILD_DIR%" install
IF ERRORLEVEL 1 exit /b 1

REM when dynamic, will copy this out
REM copy /y "%XBUILD_DIR%\src\dav1d.dll" "%XSRC_DIR%\bin%~2\"
copy /y "%XDIST_DIR%\bin\dav1d.dll" "%XAVIFSRC_DIR%\bin%~2\"
IF ERRORLEVEL 1 exit /b 1
::copy /y "%XDIST_DIR%\lib\dav1d.lib" "%XAVIFSRC_DIR%\lib%~2\"
::IF ERRORLEVEL 1 exit /b 1

popd

exit /b 0




REM ===============================================================================================
:BUILD_COPY_LIBDE

REM so the environments don't pollute each other
setlocal

SET XLIB_DIR=%~dp0..\third_party\libde265-1.1.3-src

SET XBUILD_DIR=%XOUT_DIR%\libde-%1

mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %1

pushd "%XBUILD_DIR%"
cmake.exe -DCMAKE_BUILD_TYPE=Release -A %2 "%XLIB_DIR%"
IF ERRORLEVEL 1 exit /b 1
msbuild.exe /p:Platform=%2 /p:configuration="Release" libde265.sln /t:de265
IF ERRORLEVEL 1 exit /b 1


copy /y "%XBUILD_DIR%\libde265\Release\libde265.dll" "%XSRC_DIR%\bin%~3\"
IF ERRORLEVEL 1 exit /b 1


popd

exit /b 0







REM ===============================================================================================

:BUILD_COPY_LIBHEIF

REM so the environments don't pollute each other
setlocal

SET XARCH=%1

SET XBUILD_DIR=%XOUT_DIR%\libheif-%XARCH%

SET XDAV1D_DIST=%XOUT_DIR%\dav1d-%XARCH%\dist
SET XDE265_DIR=%~dp0..\third_party\libde265-1.1.3-src

SET XDE265_BUILD=%XOUT_DIR%\libde-%1

mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %XARCH%

copy /y "%XDE265_BUILD%\libde265\de265-version.h" "%XDE265_DIR%\libde265"
IF ERRORLEVEL 1 exit /b 1

pushd "%XBUILD_DIR%"

REM The CMake project root is the libheif source drop itself; libheif/ is just a
REM subdirectory, so pointing CMake at it fails on undefined parent variables.
REM
REM Only libde265 is needed: HEIC files are HEVC, so the HEVC decoder is what
REM matters. Every encoder and the AOM/dav1d decoders are switched off so the
REM configure step does not go looking for libraries that are not on the runner.
cmake.exe -DCMAKE_BUILD_TYPE=Release -A %2 -DBUILD_SHARED_LIBS=ON -DWITH_LIBDE265=ON -DWITH_LIBDE265_PLUGIN=OFF -DLIBDE265_LIBRARY="%XDE265_BUILD%\libde265\Release\de265.lib" -DLIBDE265_INCLUDE_DIR="%XDE265_DIR%" -DWITH_X265=OFF -DWITH_X264=OFF -DWITH_OpenH264_DECODER=OFF -DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF -DWITH_DAV1D=OFF -DWITH_LIBSHARPYUV=OFF -DWITH_GDK_PIXBUF=OFF -DWITH_EXAMPLE_HEIF_THUMB=OFF -DWITH_EXAMPLE_HEIF_VIEW=OFF "%XLIB_DIR%"
IF ERRORLEVEL 1 exit /b 1
msbuild.exe /p:Platform=%2 /p:configuration="Release" libheif.sln /t:heif
IF ERRORLEVEL 1 exit /b 1

popd


copy /y "%XBUILD_DIR%\libheif\Release\heif.dll" "%XSRC_DIR%\bin%~3\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XBUILD_DIR%\libheif\Release\heif.lib" "%XSRC_DIR%\lib%~3\"
IF ERRORLEVEL 1 exit /b 1


exit /b 0






REM ===============================================================================================

:BUILD_COPY_LIBAVIF


REM so the environments don't pollute each other
setlocal

SET XARCH=%1

SET XBUILD_DIR=%XOUT_DIR%\libavif-%XARCH%

SET XDAV1D_DIST=%XOUT_DIR%\dav1d-%XARCH%\dist


mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %XARCH%


pushd "%XBUILD_DIR%"

REM not sure if there's any diff using nmake vs ninja

REM derived from https://github.com/AOMediaCodec/libavif/blob/main/.github/workflows/ci-windows.yml
::cmake.exe -G"NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DAVIF_CODEC_DAV1D=ON -DDAV1D_LIBRARY="%XDAV1D_DIST%\lib\dav1d.lib" -DDAV1D_INCLUDE_DIR="%XDAV1D_DIST%\include" "%XLIBAVIF_DIR%"
REM libavif 1.4 replaced the ON/OFF codec switches with OFF/LOCAL/SYSTEM.
REM Passing =ON silently disables dav1d (the option is neither LOCAL nor SYSTEM,
REM so AVIF_CODEC_DAV1D_ENABLED stays OFF) and you get an avif.dll that cannot
REM decode AV1 - the build still succeeds, which makes it easy to miss.
REM SYSTEM makes it use cmake/Modules/Finddav1d.cmake, which honours the
REM DAV1D_LIBRARY / DAV1D_INCLUDE_DIR cache entries passed below.
cmake.exe -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DAVIF_CODEC_DAV1D=SYSTEM -DDAV1D_LIBRARY="%XDAV1D_DIST%\lib\dav1d.lib" -DDAV1D_INCLUDE_DIR="%XDAV1D_DIST%\include" -DAVIF_ZLIBPNG=OFF -DAVIF_JPEG=OFF -DAVIF_LIBYUV=OFF -DAVIF_LIBSHARPYUV=OFF "%XLIBAVIF_DIR%"
IF ERRORLEVEL 1 exit /b 1
::nmake.exe
ninja.exe
IF ERRORLEVEL 1 exit /b 1

popd


copy /y "%XBUILD_DIR%\avif.dll" "%XAVIFSRC_DIR%\bin%~2\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XBUILD_DIR%\avif.lib" "%XAVIFSRC_DIR%\lib%~2\"
IF ERRORLEVEL 1 exit /b 1




exit /b 0












REM !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
REM OLD CODE ROUTINES

REM ===============================================================================================

REM builds it the old way, not used at the moment, using qbnu's cmake way: https://github.com/qbnu/build-heif-avif-decoders/blob/main/.github/workflows/main.yml
:BUILD_COPY_LIBDE_BUILDBAT

REM so the environments don't pollute each other
setlocal

SET XLIB_DIR=%XLIB_DIR%\libde265-1.1.3-src

REM it builds into its own directory, clear it before proceeding
REM if upstream changes the build locations, then this will have to match what's in build.bat upstream
SET XBUILD_DIR=%XLIB_DIR%\bin_%1

rd /s /q "%XBUILD_DIR%"

REM build.bat has this, but it's fixed on some version which might not be what vs-init detects
call "%~dp0vs-init.bat" %1

pushd "%XLIB_DIR%"
call build.bat %1
IF ERRORLEVEL 1 exit /b 1


copy /y "%XBUILD_DIR%\libde265.dll" "%XSRC_DIR%\bin%~3\"
IF ERRORLEVEL 1 exit /b 1


popd

exit /b 0

REM ===============================================================================================

REM this was the accompanying when using the BUILD.BAT way of libde
:BUILD_COPY_LIBHEIF_OLDLIBDE

REM so the environments don't pollute each other
setlocal

SET XARCH=%1

SET XBUILD_DIR=%XOUT_DIR%\libheif-%XARCH%

SET XDAV1D_DIST=%XOUT_DIR%\dav1d-%XARCH%\dist
SET XDE265_DIR=%XLIB_DIR%\libde265


mkdir "%XBUILD_DIR%" 2>nul

call "%~dp0vs-init.bat" %XARCH%

pushd "%XBUILD_DIR%"


REM de265.h has an include
REM #include <libde265/de265-version.h>
REM which requires doing this since on windows there's no "install" which takes care of this
REM if you try to just include the extra directory, bad things happen since there are other .h files in there ... -DLIBDE265_INCLUDE_DIR="%XDE265_DIR%\extra"
copy /y "%XDE265_DIR%\extra\libde265\de265-version.h" "%XDE265_DIR%\libde265"

REM for static dav1d:  cmake.exe -G"NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DDAV1D_LIBRARY="%XDAV1D_DIST%\lib\libdav1d.a" -DDAV1D_INCLUDE_DIR="%XDAV1D_DIST%\include" -DLIBDE265_LIBRARY="%XDE265_DIR%\bin_%XARCH%\lib\libde265.lib" -DLIBDE265_INCLUDE_DIR="%XDE265_DIR%" "%XLIB_DIR%\libheif"
cmake.exe -G"NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DDAV1D_LIBRARY="%XDAV1D_DIST%\lib\dav1d.lib" -DDAV1D_INCLUDE_DIR="%XDAV1D_DIST%\include" -DLIBDE265_LIBRARY="%XDE265_DIR%\bin_%XARCH%\lib\libde265.lib" -DLIBDE265_INCLUDE_DIR="%XDE265_DIR%" "%XLIB_DIR%\libheif"
IF ERRORLEVEL 1 exit /b 1
nmake.exe
IF ERRORLEVEL 1 exit /b 1

popd

copy /y "%XBUILD_DIR%\libheif\heif.dll" "%XSRC_DIR%\bin%~3\"
IF ERRORLEVEL 1 exit /b 1
copy /y "%XBUILD_DIR%\libheif\heif.lib" "%XSRC_DIR%\lib%~3\"
IF ERRORLEVEL 1 exit /b 1


exit /b 0
