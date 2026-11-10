# JPEGView v1.3.46-20261110

Loader fixes found by the runtime-logic audit, each reproduced before the change
and covered by a regression case.

## Bugs fixed

- **PNM**: a file with a maxval below 255 (4-bit/6-bit samples) is scaled to full
  range instead of being shown at maxval/255 brightness; a legal maxval=15 PGM
  used to render almost black instead of white.
- **TGA**: a 15-bit RLE file consumed one byte per pixel instead of two, so
  everything after the first pixel decoded to noise.
- **JPEG**: an Exif APP1 that is not the first APP1 (XMP written in front of it)
  is found again, so orientation and camera data survive.
- **JPEG**: an over-short APP1 that merely starts with "Exif" is rejected again
  instead of being handed to the EXIF parser.

## Known issues (not fixed in this version)

- A file whose first load fails stays unopenable for the rest of the session.
- A path given with forward slashes opens the image but builds no file list.
- Renaming with name sorting leaves the entry at its old position.

## Downloads

| Platform | File |
|---|---|
| Windows x64 | `JPEGView-x64-v1.3.46-20261110.zip` |
| Windows x86 | `JPEGView-win32-v1.3.46-20261110.zip` |

Extract and run `JPEGView.exe`; no installer required.
