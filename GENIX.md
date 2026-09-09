# blastem-genix

BlastEm (Mike Pavone's Sega Genesis / Mega Drive emulator, GPL-3.0,
https://www.retrodev.com/repos/blastem) plus the device models the
Genix operating system (https://github.com/EythorE/genix) needs for
its test ladder. Upstream is Mercurial; this repo takes upstream
through the git conversion at https://github.com/EythorE/blastem-mirror.

## Layout of history

- `main` is the working branch: an upstream commit plus the Genix
  patches on top. It is never force-pushed.
- The mirror's `main` is rewritten daily by its bot (an upstream
  commit with the mirror's workflow files re-applied on top). Its
  upstream commit SHAs are stable across runs; only the bot's tip
  commit changes.
- Bringing in a newer upstream: fetch the mirror, then MERGE the
  upstream commit just below the bot's tip (never the tip itself,
  which carries the mirror's own workflow files):

      git fetch upstream
      git merge upstream/main~1

  Merge, never rebase, so published SHAs stay put.

Base of this repo: upstream 2dd605c (2026-08-20, "Fix a warning
when building for non-x86 ...").

## Building

    make

The `Build (Linux)` workflow (`.github/workflows/build.yml`, from
the mirror) builds any commit on demand and publishes a
self-contained Linux tarball as a GitHub release:

    gh workflow run build.yml --repo EythorE/blastem-genix \
        -f commit=<sha-or-ref> -f tag=v$(date +%Y.%m.%d)

The Genix ladder and its CI install from those releases.

## Genix patches

None yet. Planned, in the order the Genix plans give them:

- A MIDI dongle port device (io.c): the nibble handshake of
  genix/docs/plans/usb-midi-dongle.md sec 4, with a frame-script
  source and fault injection. Plan: genix/docs/plans/midi-dongle-build.md.
- The production cartridge mapper and SD card model:
  genix/hardware/production-cart/design.md secs 4-6. Plan:
  genix/docs/plans/production-cart.md sec 8.

Each patch is a separate commit or PR against `main`, touching as
few upstream files as possible so upstream merges stay clean.
