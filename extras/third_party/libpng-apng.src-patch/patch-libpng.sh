#!/usr/bin/env bash

# --forward is optional, and added to make scripting calls to this easier

# apng support
echo "--- Patching libpng with apng support ---"
patch -d libpng -p0 --forward < libpng-apng/libpng-1.6.59-apng.patch
# error check, fail and return error if failed
if [ $? -ne 0 ]; then
	echo apng patch failed
	exit 1
fi


# 64-bit support
# Regenerate this patch whenever libpng is upgraded: it edits the vcxproj files
# and the context moves between releases. The current one is generated against
# libpng 1.6.59 (upstream dropped arm/filter_neon.c there, so the old 1.6.40
# patch failed one hunk and patch therefore exited non-zero).
echo "--- Patching libpng with 64-bit support ---"
patch -d libpng -p1 --forward < libpng-x64.patch
if [ $? -ne 0 ]; then
	echo x64 patch failed
	exit 1
fi

# The whole point of the x64 patch is the "Release Library|x64" configuration.
# Verify it actually landed instead of trusting the patch exit code.
if ! grep -q "Release Library|x64" libpng/projects/vstudio/libpng/libpng.vcxproj; then
	echo "x64 patch applied but 'Release Library|x64' is missing from libpng.vcxproj"
	exit 1
fi
