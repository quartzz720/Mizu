#!/usr/bin/env bash
# Build Mizu and its first-run questions.
#
# Nothing here is specific to this project: it hands the sources to the Koi-DOS
# SDK's build script, exactly as any third-party program would. That is half
# the point of Mizu living outside the kernel tree - if this stops building,
# the SDK is broken for everybody, not only for us.
#
# The dialogues, the settings, the language tables and the WAV reader are the
# SDK's, not copies kept here. They used to sit in the kernel repository beside
# Mizu, which was fine while Mizu was in it too; a copy carried out here would
# have been a copy that goes stale, and that is the failure this project was
# split out to avoid rather than to acquire.
#
# The windowing library is the exception, and it is ours. window.c draws the
# taskbar, the Start button, the wallpaper, the window frames and the shutdown
# dialogue - that is this desktop's face, not a service the system owes every
# program. It lived in the kernel tree until Koi noticed that Koi-DOS was
# shipping a desktop in its SDK while claiming to be complete without one, and
# that keeping it there meant a tracked copy in each repository: two copies of
# one truth, which is the failure this whole split exists to avoid. It is
# copied into sdk/ for the build and taken out again afterwards, exactly as
# mizu.h is.
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

    cp "$main" mizu.h window.h window.c sdk/
    ( cd sdk && ./koicc "$(basename "$main")" "$@" -o "$name" )
    mv "sdk/$(echo "$name" | tr 'a-z' 'A-Z').EXE" .
    rm -f "sdk/$(basename "$main")" sdk/mizu.h sdk/window.h sdk/window.c
}

# An application: the same compiler with a different entry point, and the
# result is a .APP that Mizu loads rather than a program anybody runs. mizu.h
# goes along with it because it is the agreement between the two, and it lives
# here rather than in the SDK for the same reason - Koi-DOS knows how to load a
# module and has no opinion about what one contains.
application() {
    local main="$1" name="$2"; shift 2

    # minimp3.h and its shim go along too: the player decodes MP3 and this is
    # where a third-party header lives. Copied for every application rather
    # than only the one that needs it, because a list of which application
    # needs which header is a list that goes stale.
    cp "$main" mizu.h window.h window.c minimp3.h mp3shim.h sdk/
    ( cd sdk && ./koicc -m "$(basename "$main")" "$@" -o "$name" )
    mv "sdk/$(echo "$name" | tr 'a-z' 'A-Z').APP" .
    rm -f "sdk/$(basename "$main")" sdk/mizu.h sdk/window.h sdk/window.c \
          sdk/minimp3.h sdk/mp3shim.h
}

build mizu.c    mizu    window.c language.c settings.c wav.c
build mizucfg.c mizucfg dialog.c settings.c language.c

application apps/noteedit.c noteedit editcore.c language.c settings.c
application apps/player.c   player   wav.c
# A test rather than a part of the desktop: it proves that yielding works and
# shows what not yielding costs. publish.sh does not ship it.
application apps/files.c    files
application apps/term.c     term
application apps/image.c    image    png.c inflate.c gif.c
# apps/control.c and not apps/settings.c: build() copies the source into sdk/
# beside the library it is built against, and a file called settings.c would
# land on top of the SDK's own settings.c - which is a build that silently
# links the wrong file and then deletes the right one.
application apps/control.c  control  settings.c
application apps/nami.c     nami     language.c png.c inflate.c gif.c
application apps/spin.c     spin

ABI=$(sed -n 's/^#define KOI_ABI_VERSION \([0-9]*\).*/\1/p' sdk/syscall.h)
echo
echo "MIZU.EXE $(stat -c %s MIZU.EXE) bytes, MIZUCFG.EXE $(stat -c %s MIZUCFG.EXE) bytes"
echo "NAMI.APP $(stat -c %s NAMI.APP) bytes"
echo "NOTEEDIT.APP $(stat -c %s NOTEEDIT.APP) bytes, PLAYER.APP $(stat -c %s PLAYER.APP) bytes, FILES.APP $(stat -c %s FILES.APP) bytes, TERM.APP $(stat -c %s TERM.APP) bytes, IMAGE.APP $(stat -c %s IMAGE.APP) bytes, CONTROL.APP $(stat -c %s CONTROL.APP) bytes"
echo "(interface version $ABI)"
echo
echo "Mizu is a package. It installs into a directory of its own:"
echo
echo "    dosget install mizu"
echo
echo "or by hand, onto a Koi-DOS disk image:"
echo
echo "    mmd   -i esp.img ::/MIZU"
echo "    mcopy -o -i esp.img MIZU.EXE MIZUCFG.EXE NOTEEDIT.APP PLAYER.APP FILES.APP TERM.APP IMAGE.APP CONTROL.APP NAMI.APP WALLPAPER.BMP ::/MIZU/"
echo "    mcopy -o -i esp.img ICONS/*.BMP start.wav ::/MIZU/"
