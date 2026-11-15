# JPEGView v1.3.46-20261115

Fixes from the c2adbf4 review: the three fix batches (cheap batch, OCR batch, tests/guards/docs batch) and their follow-ups, each verified against the code and -- for the device-testable items -- on a real Windows machine.

## Bugs fixed

- **Lossless JPEG**: EXIF auto-rotation no longer marks the image as destructively processed, so the lossless menu stays available for rotated phone photos; the orientation tag is now composed with the transform, so a mirrored orientation-6 photo no longer flips twice on reload.
- **Saving**: a silent save (Ctrl+S) no longer reuses the lossless flag left over from an earlier save dialog -- the flag is recomputed from the target extension, so a lossless WebP dialog save no longer makes a later Ctrl+S on a .jxl/.jp2 file save losslessly. The save filter's lossless choice now also applies to a hand-typed extension's format.
- **16-bit PNM**: a maxval other than 65535 (e.g. 4095) no longer renders far too dark; the range is scaled to full brightness, as already done for 8-bit files.
- **OCR selection**: dragging a word out and back to the same word no longer leaves it unselected; losing focus or mouse capture now ends a word-drag selection.
- **OCR**: start errors are split by exception type instead of always blaming memory; the menu is greyed out with no image; an empty result is cached so re-toggling does not start another run; "No text found" no longer pops a modal dialog (it shows in the canvas status line); an OCR error that completes while a modal dialog is up is deferred until the window is active again. The canvas hint now states that Ctrl+C copies all text when nothing is selected and Esc exits OCR to copy the image (prompt wording only, no behaviour change).
- **Clipboard**: a failed image copy no longer leaves just the file name on the clipboard, and now reports the failure with a message box instead of failing silently.
- **INI**: replacing the user settings file is now a single atomic ReplaceFile call instead of a two-step move, so a failure can no longer lose the settings (the replaced file is still kept as <ini>.old).
- **Window**: closing a maximized or minimized window no longer stores the maximized/minimized rect as the sticky normal rect.
- **Batch copy/rename**: a rollback now checks the result of each delete/move (including directory removal), logs failures, and reports the failed count instead of always claiming a full rollback.
- **Keyboard**: a user KeyMap binding now takes effect for digit keys and F1 (the KeyMap is consulted before the hardcoded branches); with the default KeyMap the behaviour is unchanged.
- **Colour management**: a failed CMYK profile load is no longer memoized for the life of the process, so a transient lock no longer leaves CMYK unconverted until restart.
- **Single instance**: launching with no file now always brings the running window forward; the empty payload is signalled explicitly, so it no longer reopens the current image when one was already open.
- **Slideshow**: advancing no longer sleeps on the UI thread (the timer is re-armed with the remaining time); an overlong line in a slideshow list is skipped instead of rejecting the whole file.
- **Localisation**: the 28 language files were re-synced to the reference strings (447 strings); no existing translation was lost.

## Notes

- Minimum supported OS is now declared as Windows 10 (README, COMPILING.txt, readme.html, HowToInstall.txt and the Russian/Ukrainian readmes list only Windows 10/11). Declaration only -- no build setting was changed, and Windows 7/XP are not blocked from running it.
- The CMYK end-to-end pixel test and the loader regression tests now run in CI.

## Known issues (not fixed in this version)

- A file whose first load fails usually stays unopenable for the rest of the session; the dead request can be evicted under cache pressure, after which a non-RAW/TIFF file may load again on revisit.
- A path given with forward slashes does not open the image at all: the window shows the no-image empty state and no file list is built.
- Renaming with name sorting leaves the entry at its old position.
- Shift+Delete permanently deletes the current file from disk (with a confirmation prompt); it does not use the Recycle Bin. Plain Delete moves the file to the Recycle Bin.
- Colour management (embedded ICC profiles) applies to JPEG and PNG. Exceptions: images decoded through the WIC fallback get no colour management, and CMYK JPEGs always convert - the switch only chooses between the embedded CMYK profile and the shipped cmyk.icm (a naive formula is the last resort).

## Downloads

| Platform | File |
|---|---|
| Windows x64 | `JPEGView-x64-v1.3.46-20261115.zip` |
| Windows x86 | `JPEGView-win32-v1.3.46-20261115.zip` |

Extract and run `JPEGView.exe`; no installer required.
