# Mizu

A desktop with windows for [Koi-DOS](../Koi-DOS), in the shape Windows 3.0 had:
a menu bar, windows that overlap and can be dragged and resized, a bar along
the bottom naming the ones you cannot see, and a control panel you start things
from.

*Mizu* is water.

## What it is, and what it is not

**It is a package.** Koi-DOS does not contain it and is complete without it.
Installing it puts it in `\MIZU` and on the program search path; removing it
leaves the same system that was there before:

    dosget install mizu
    dosget remove mizu

That is why this is a separate repository. While Mizu lived in the kernel tree
it was true that it was a package and hard to believe, because everything was
built by one `make` and shipped by one image. Here the claim is checked by the
arrangement: the kernel builds and boots without this directory existing.

**It is not a system yet.** Every window belongs to one program, because
Koi-DOS runs one program at a time. That is not a stopgap standing in for a
missing feature - it is what Windows 1.0 through 3.0 in real mode actually
were, and their bundled applications were parts of one image for the same
reason.

Where it goes next is the interesting part, and it is the road Windows took:
a protected-mode kernel that starts from DOS, takes the machine, and runs DOS
underneath it rather than on top. See `PLAN.md` in the Koi-DOS tree.

## Building

    cp ../Koi-DOS/sdk/* sdk/     # once, and after the SDK changes
    ./build.sh

It builds with the Koi-DOS SDK and nothing else - the same `koicc` any
third-party program uses. The windowing library, the dialogues, the settings
and the WAV reader come from the SDK rather than being copied in here, so there
is one of each rather than one per project.

`../DOSGET/publish.sh` builds this automatically and publishes it as the MIZU
package.

## Licence

MIT, the same as Koi-DOS. See [LICENSE](LICENSE).

`WALLPAPER.BMP` is part of this package.
