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

Each patch is a separate commit against `main`, touching as few
upstream files as possible so upstream merges stay clean.

### The MIDI dongle port device (2026-09-09)

`genix/genix_dongle.c` models the Genix MIDI dongle
(genix/docs/plans/usb-midi-dongle.md sec 4; build plan
genix/docs/plans/midi-dongle-build.md sec 4 and sec 12) as a
controller-port device: the nibble handshake on D0-D3 / TR / TH with
TL as the console's ack, run by `genix/link/link_tx.c`, the SAME
handshake code the dongle firmware runs.  `genix/link/` is a copy of
`genix/hardware/midi-dongle/link/`; the Makefile bakes a checksum of
it into the binary and the device prints it when it attaches, so the
Genix ladder can fail loudly on drift (`scripts/check-dongle-link.sh`
there).  Sync the copy whenever the protocol changes:

    cp ../genix/hardware/midi-dongle/link/{frame.c,frame.h,script.c,script.h,link_tx.c,link_tx.h,link_rx.h} genix/link/

Upstream files touched: `io.h` (one enum value, one union member),
`io.c` (the device string, six call sites), `blastem.c` (the `-M`
option and its help line), `Makefile` (the objects, the checksum,
two mkdirs).

Use:

    blastem -M keys.fst ROM              # a frame stream (mkframes output) on port 2
    blastem -M midi:/dev/snd/midiC1D0 ROM  # raw MIDI bytes from a device node or FIFO
    blastem -M keys.fst,latency=3,trace=200 ROM

`latency=<us>` is the dongle's response latency after a TL edge
(default 3; the board's number replaces it).  `trace=<n>` logs the
first n port events (TL edges, offers, reads) to stderr with the
model's microsecond clock: the handshake nibble by nibble.  The
frame stream's `stall` and `detach` directives freeze the dongle
mid-frame and unplug it for a while; the device logs each one.

The model's clock is the master clock of the running context
(53.69 MHz NTSC, 53.20 MHz PAL), kept as a 64-bit count across
BlastEm's cycle deductions.  Two BlastEm facts the Genix side had
to learn: `-b N` counts at about 120 a second (NTSC), not 60; and
`io.c` restarts its slow-rise model on every control-register
write, so an input pin whose latch holds 0 reads 0 for ~4 us after
any direction change.  The device therefore takes TL as the
console's latch while the console drives the pin and as the pull-up
(high) otherwise, which is what the wire does.

### Planned

- The production cartridge mapper and SD card model:
  genix/hardware/production-cart/design.md secs 4-6. Plan:
  genix/docs/plans/production-cart.md sec 8.

## Building here

`make` needs SDL2 and GLEW development files (`pkg-config sdl2 glew
gl`).  Without a system GLEW, build 2.2.0 into a prefix and point
pkg-config at it:

    make -C glew-2.2.0 GLEW_DEST=$PWD/glew-2.2.0/dist install
    rm glew-2.2.0/dist/lib*/libGLEW.so*     # link it statically
    # fix includedir= in dist/lib/pkgconfig/glew.pc to dist/include
    PKG_CONFIG_PATH=$PWD/glew-2.2.0/dist/lib/pkgconfig make -j8 blastem

The Genix ladder runs the dongle legs with
`BLASTEM=~/github/blastem-genix/blastem` until a release is tagged.
