#!/usr/bin/env bash
# Build Mizu and its first-run questions.
#
# Nothing here is specific to this project: it hands the sources to the Koi-DOS
# SDK's build script, exactly as any third-party program would. That is half
# the point of Mizu living outside the kernel tree - if this stops building,
# the SDK is broken for everybody, not only for us.
#
# The windowing library, the dialogues, the settings, the language tables and
# the WAV reader are the SDK's, not copies kept here. They used to sit in the
# kernel repository beside Mizu, which was fine while Mizu was in it too; a
# copy carried out here would have been a copy that goes stale, and that is the
# failure this project was split out to avoid rather than to acquire.
set -euo pipefail

cd "$(dirname "$0")"

# Refresh the SDK from a Koi-DOS tree with:
#
#     cp ../Koi-DOS/sdk/* sdk/
#
# The whole directory, not a list of names: a written-down list went stale the
# day the SDK gained a file.
if [ ! -f sdk/koicc ]; then
    echo "build.sh: sdk/ is empty. Copy it from a Koi-DOS tree:" >&2
    echo "    cp ../Koi-DOS/sdk/* sdk/" >&2
    exit 1
fi

build() {
    local main="$1" name="$2"; shift 2

    cp "$main" sdk/
    ( cd sdk && ./koicc "$(basename "$main")" "$@" -o "$name" )
    mv "sdk/$(echo "$name" | tr 'a-z' 'A-Z').EXE" .
    rm -f "sdk/$(basename "$main")"
}

build mizu.c    mizu    window.c editcore.c language.c settings.c wav.c
build mizucfg.c mizucfg dialog.c settings.c language.c

ABI=$(sed -n 's/^#define KOI_ABI_VERSION \([0-9]*\).*/\1/p' sdk/syscall.h)
echo
echo "MIZU.EXE $(stat -c %s MIZU.EXE) bytes, MIZUCFG.EXE $(stat -c %s MIZUCFG.EXE) bytes"
echo "(interface version $ABI)"
echo
echo "Mizu is a package. It installs into a directory of its own:"
echo
echo "    dosget install mizu"
echo
echo "or by hand, onto a Koi-DOS disk image:"
echo
echo "    mmd   -i esp.img ::/MIZU"
echo "    mcopy -o -i esp.img MIZU.EXE MIZUCFG.EXE WALLPAPER.BMP ::/MIZU/"
