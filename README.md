[![Documentation](https://img.shields.io/badge/Docs-Outdated-yellowgreen)](https://htmlpreview.github.io/?https://github.com/mfirewire/jpegview/blob/master/src/JPEGView/Config/readme.html) [![Localization Progress](https://img.shields.io/badge/Localized-91%25-blueviolet)](#Localization) [![Build x64](https://github.com/mfirewire/jpegview/actions/workflows/build-release-x64.yml/badge.svg?branch=master)](https://github.com/mfirewire/jpegview/actions/workflows/build-release-x64.yml) [![OS Support](https://img.shields.io/badge/Windows-XP%20%7C%207%20%7C%208%20%7C%2010%20%7C%2011-blue)](#) [![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue)](https://github.com/mfirewire/jpegview/blob/master/LICENSE.txt)

[![Latest GitHub Release](https://img.shields.io/github/v/release/mfirewire/jpegview?label=GitHub&style=social)](https://github.com/mfirewire/jpegview/releases)[![Downloads](https://badgen.net/github/assets-dl/mfirewire/jpegview?cache=3600&color=grey&label=)](#) [![WinGet](https://repology.org/badge/version-for-repo/winget/jpegview.svg?allow_ignored=1&header=WinGet)](https://winstall.app/apps/sylikc.JPEGView) [![PortableApps](https://img.shields.io/badge/PortableApps-Current-green)](https://portableapps.com/apps/graphics_pictures/jpegview_portable) [![Scoop](https://repology.org/badge/version-for-repo/scoop/jpegview-fork.svg?header=Scoop)](https://scoop.sh/#/apps?q=%22jpegview-fork%22) [![Chocolatey](https://img.shields.io/chocolatey/v/jpegview)](https://community.chocolatey.org/packages/jpegview) [![Npackd](https://repology.org/badge/version-for-repo/npackd_stable/jpegview.svg?allow_ignored=1&header=Npackd)](https://www.npackd.org/p/jpegview)

# JPEGView - Image Viewer and Editor

This is the official re-release of JPEGView.

## Description

JPEGView is a lean, fast and highly configurable image viewer/editor with a minimal GUI.

### Formats Supported

JPEGView has built-in support the following formats:

* Popular: JPEG (including JPS, MPO), GIF
* Lossless: BMP, PNG (including APNG), TIFF, PSD, QOI, PPM (including PGM, PNM, PBM, PAM), HDR (including RGBE), JPEG 2000 (including JP2, J2K, J2C), OpenEXR (.exr)
* Web: WEBP, JXL, HEIF/HEIC, AVIF (including AVIFS, AVIS), SVG (.svg)
* Legacy: TGA (including ICB, VDA, VST), WDP, HDP, JXR, DDS, Windows icon/cursor (ICO, CUR)
* Camera RAW formats:
  * Adobe (DNG), Canon (CRW, CR2, CR3), Nikon (NEF, NRW), Sony (ARW, SR2, SRF)
  * Olympus (ORF), Panasonic (RW2), Fujifilm (RAF)
  * Sigma (X3F), Pentax (PEF), Minolta (MRW), Kodak (KDC, DCR)
  * A full list is available here: [LibRaw supported cameras](https://www.libraw.org/supported-cameras)

Many additional formats are supported by Windows Imaging Component (WIC)

### Save / Export

Besides JPEG, BMP, PNG, TIFF, WebP and JPEG XL, the current image can also be saved as QOI, GIF (256 colours), PNM (PPM/PGM/PNM), PBM, HDR (HDR/RGBE), TGA (TGA/ICB/VDA/VST), JPEG 2000 (JP2/J2K/J2C) and OpenEXR (.exr). WebP, JPEG XL and JPEG 2000 offer a lossless option in the Save As dialog.

### Basic Image Editor

Basic on-the-fly image processing is provided - allowing adjusting typical parameters:

* sharpness
* color balance
* rotation
* perspective
* contrast
* local under-exposure/over-exposure

### Text Recognition (OCR)

Press `Ctrl+Q` (or menu *Recognize text (OCR)*) to recognize the text in the current image. JPEGView uses the built-in Windows OCR engine (Windows 10 and later), so the language packs you need must be installed in Windows Settings > Time & language > Language & region.

* Mixed Chinese/English images are recognized with both engines and the results are merged line by line, so Chinese and English words appear in reading order.
* Click or drag over words to select them; `Ctrl+C` copies the selection (or all recognized text when nothing is selected), `Ctrl+A` selects all. `Esc` hides the result.
* Configure it in `JPEGView.ini`: `OcrShortcut` (default `Ctrl+Q`) and `OcrLanguages` (default `zh-CN,zh-TW,en-US`; empty means the installed languages, in Windows' own order).
* The result belongs to the image as recognized: changing the image, rotating, cropping or mirroring clears it.

### Other Features

* Small and fast, uses AVX2/SSE2 and up to 4 CPU cores
* High quality resampling filter, preserving sharpness of images
* Basic image processing tools can be applied realtime during viewing
* Movie/Slideshow mode - to play folder of JPEGs as movie

### Performance & Stability

* JXL encode: single-pass BGR→RGB copy + 1 MB output pre-allocation (faster save, fewer buffer resizes)
* JXL save/convert: NULL-output guards prevent crashes on encode failure
* TIFF read: out-of-memory now falls back to WIC reader instead of sticking in error state
* CMYK ICC profile: delayed open + path buffer fix (MAX_PATH+16) avoids invalid-path crashes; bundled `Config/cmyk.icm`
* PSD CMYK: fixed 4-channel YMCK avoids alpha-channel rendering bugs
* Library updates: libwebp 1.6.0, libde265 1.1.3, libheif 1.23.7, LibRaw 0.22.3
* JPEG 2000 save fixed: the image rectangle is now set, so JP2/J2K/J2C saves succeed (previously every save failed)
* Animated image switching no longer corrupts the heap (per-thread animation caches)
* Per-thread whole-file read buffer is released after a request once past 64 MB (lower memory after opening a large file)
* Maximum image size (`MaxImagePixelsMP`, default 256 MP): images larger than this are rejected with a "size exceeds the limit" error instead of being decoded. The default used to be 100 MP, which rejected real photos (102 MP medium format, 108 MP phones, 200 MP phones); 256 MP covers every single-shot camera (the largest, a 200 MP phone sensor, is 201 MP) with headroom, while still blocking absurd dimensions. Pixel-shift multi-shot files (up to 400 MP) and stitched panoramas need a higher value (range 1–4096). Existing INI files that already set `MaxImagePixelsMP` keep their value; the new default applies to fresh installs and INI files without the key.
* Large images fail fast: oversize images are rejected after parsing the header, before any pixel data is decoded, and file-size limits bound the read before that (JPEG 300 MB, WebP/JXL/HEIF 150 MB, PSD/BMP 500 MB on 64-bit). An out-of-memory failure also fails fast (the allocator returns NULL); the only slow failure is paging, if the peak working set exceeds physical RAM — a 256 MP image needs a ~1 GB 32-bit DIB (~2.6 GB peak working set). On a 32-bit build the practical ceiling is lower (2 GB address space).

Default runtime settings (INI, key bindings, window behavior) are unchanged from this branch, except the `MaxImagePixelsMP` default (100 → 256 MP, see above).

# Installation

## Official Releases

Official releases will be made to [mfirewire's GitHub Releases](https://github.com/mfirewire/jpegview/releases) page.  Each release includes:

* **Archive Zip/7z** - Portable
* **Windows Installer MSI** - For Installs
* **Source code** - Build it yourself

## Portable

JPEGView _does not require installation_ to run.  Just **unzip, and run** either the 64-bit version, or the 32-bit version depending on which platform you're on.  It can save the settings to the extracted folder and run entirely portable.

## MSI Installer

For those who prefer to have JPEGView installed for All Users, a 32-bit/64-bit installer is available to download starting with v1.0.40.

(Unfortunately, I don't own a code signing certificate yet, so the MSI release is not signed.  Please verify checksums!)

### WinGet

If you're on Windows 11, or Windows 10 (build 1709 or later), you can also download it directly from the official [Microsoft WinGet tool](https://docs.microsoft.com/en-us/windows/package-manager/winget/) repository.  This downloads the latest MSI installer directly from GitHub for installation.

Example Usage:

C:\> `winget search jpegview`
```
Name     Id              Version  Source
-----------------------------------------
JPEGView mfirewire.JPEGView 1.3.47  winget
```

C:\> `winget install jpegview`
```
Found JPEGView [mfirewire.JPEGView] Version 1.3.47
This application is licensed to you by its owner.
Microsoft is not responsible for, nor does it grant any licenses to, third-party packages.
Downloading https://github.com/mfirewire/jpegview/releases/download/v1.3.47/JPEGView64_en-us_1.3.47.msi
  ██████████████████████████████  4.23 MB / 4.23 MB
Successfully verified installer hash
Starting package install...
Successfully installed
```

## PortableApps

Another option is to use the official [JPEGView on PortableApps](https://portableapps.com/apps/graphics_pictures/jpegview_portable) package.  The PortableApps launcher preserves user settings in a separate directory from the extracted application directory.  This release is signed.

## Scoop

[Scoop](https://scoop.sh/) is a Windows command-line installer and manager for portable applications.

To install with Scoop, run the following commands:

```shell
scoop bucket add extras
scoop install extras/jpegview-fork
```

After installation, the configuration file is located at `%UserProfile%\scoop\persist\JPEGView-fork\JPEGView.ini`.

## System Requirements

* 64-bit version: Windows 7/8/10/11 64-bit or later
* 32-bit version: Windows 7 or later
  * A special _32-bit Windows XP SP2_ build is available, which supports most formats (except for formats added after v1.0.37.1, ex. Animated PNG, JXL, HEIC).  Other features and options are the same as the normal builds.

## What's New

* See what has changed in the [latest releases](https://github.com/mfirewire/jpegview/releases)
* Or Check the [CHANGELOG.txt](https://github.com/mfirewire/jpegview/blob/master/CHANGELOG.txt) to review new features in detail.

# Localization

By default, the language is auto-detected to match your Windows Locale.  All the text in the menus and user interface should show in your language.  To override the auto-detection, manually set `Language` option in `JPEGView.ini`

JPEGView is currently translated/localized to 28 languages:

| INI Option | Language |
| ---------- | -------- |
| be | Belarusian |
| bg | Bulgarian |
| cs | Czech |
| de | German |
| el | Greek, Modern |
| es-ar | Spanish (Argentina) |
| es | Spanish |
| eu | Basque |
| fi | Finnish |
| fr | French (Français) |
| hu | Hungarian |
| it | Italian |
| ja | Japanese (日本語) |
| ko | Korean (한국어) |
| pl | Polish |
| pt-br | Portuguese (Brazilian) |
| pt | Portuguese |
| ro | Romanian |
| ru | Russian (Русский) |
| sk | Slovak |
| sl | Slovenian (Slovenščina) |
| sr | Serbian (српски) |
| sv | Swedish |
| ta | Tamil |
| tr | Turkish (Türkçe) |
| uk | Ukrainian (Українська) |
| zh-tw | Chinese, Traditional (繁體中文) |
| zh | Chinese, Simplified (简体中文) |

See the [Localization wiki page](https://github.com/mfirewire/jpegview/wiki/Localization#localization-status) for translation status for each language.

# Help / Documentation

The JPEGView documentation is a little out of the date at the moment, but should still give a good summary of the features.

This [readme.html](https://htmlpreview.github.io/?https://github.com/mfirewire/jpegview/blob/master/src/JPEGView/Config/readme.html) is part of the JPEGView package.

# Brief History

This GitHub repo continues the legacy (is a "fork") of the excellent project [JPEGView by David Kleiner](https://sourceforge.net/projects/jpegview/).  Unfortunately, starting in 2020, the SourceForge project has essentially been abandoned, with the last update being [2018-02-24 (1.0.37)](https://sourceforge.net/projects/jpegview/files/jpegview/).  It's an excellent lightweight image viewer that I use almost daily!

The starting point for this repo was a direct clone from SourceForge SVN to GitHub Git.  By continuing this way, it retains all previous commits and all original author comments.

I'm hoping with this project, some devs might help me keep the project alive!  It's been awhile, and could use some new features or updates.  Looking forward to the community making suggestions, and devs will help with some do pull requests as some of the image code is quite a learning curve for me to pick it up. -sylikc

## Special Thanks

Special thanks to [qbnu](https://github.com/qbnu) for adding additional codec support!
* Animated WebP
* Animated PNG
* JPEG XL with animation support
* HEIF/HEIC/AVIF support
* QOI support
* ICC Profile support for WebP, JPEG XL, HEIF/HEIC, AVIF
* LibRaw support (all updated RAW formats, such as CR3)
* Photoshop PSD support

Thanks to all the _translators_ which keep JPEGView strings up-to-date in different languages!  See [CHANGELOG.txt](https://github.com/mfirewire/jpegview/blob/master/CHANGELOG.txt) to find credits for translators at each release!
