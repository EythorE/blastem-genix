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
`io.c` (the device string, six call sites, plus the port choice and
the keyboard passthrough), `blastem.c` (the `-M` option and its help
line), `Makefile` (the objects, the checksum, two mkdirs).

Use:

    blastem -M keys.fst ROM              # a frame stream (mkframes output) on port 2
    blastem -M midi:/dev/snd/midiC1D0 ROM  # raw MIDI bytes from a device node or FIFO
    blastem -M keys.fst,port=1 ROM       # port 1, Saturn keyboard left on port 2
    blastem -M keys.fst,latency=3,trace=200 ROM

`port=1` puts the device on controller port 1 instead of port 2
(added 2026-09-09 with the Genix port model, which finds a device on
either port; `io.c`'s `setup_io_devices` reads it straight off the
spec).  `latency=<us>` is the dongle's response latency after a TL
edge (default 3; the board's number replaces it).  `trace=<n>` logs
the first n port events (TL edges, offers, reads) to stderr with the
model's microsecond clock: the handshake nibble by nibble.  The
frame stream's `stall` and `detach` directives freeze the dongle
mid-frame and unplug it for a while; the device logs each one.

HOST KEYBOARD PASSTHROUGH (2026-09-09): while the dongle holds a
port, BlastEm's own key events are pushed into it as 0x01 key frames
- the same priority ring the frame script's `type` directive feeds,
and the emulated form of the product's USB keyboard path.  BlastEm's
scancodes are already set 2, which is what the frame format and the
Genix keyboard driver want.  It is ON BY DEFAULT whenever no other
port has a keyboard, which is the plain `-M` case, and off when a
Saturn keyboard is there to type on instead; `keys=off` disables it,
`keys=on` forces it alongside a keyboard.  `io.c` gains
`find_genix_dongle` / `genix_dongle_for_keys`, two lines each in
`io_keyboard_down` / `io_keyboard_up`, and one in `io_has_keyboard`
(without which BlastEm would not offer keyboard capture at all).

A hand session: `-M midi:/tmp/midi` with a FIFO (`mkfifo /tmp/midi`),
`mididump` on the console, then `printf '\x90\x3c\x64' > /tmp/midi`.
The dongle takes port 2 from the Saturn keyboard there, but the
passthrough above hands the host keyboard back, so there IS something
to type `mididump` with - press right ctrl to capture the keyboard
first.  `-M ...,port=1` is the other way: the Saturn keyboard keeps
port 2 and the console finds the dongle on port 1.

The model's clock is the master clock of the running context
(53.69 MHz NTSC, 53.20 MHz PAL), kept as a 64-bit count across
BlastEm's cycle deductions.  Two BlastEm facts the Genix side had
to learn: `-b N` counts at about 120 a second (NTSC), not 60; and
`io.c` restarts its slow-rise model on every control-register
write, so an input pin whose latch holds 0 reads 0 for ~4 us after
any direction change.  The device therefore takes TL as the
console's latch while the console drives the pin and as the pull-up
(high) otherwise, which is what the wire does.

### The production cart mapper (2026-09-09)

`genix/genix_cart.c` is the Genix production cartridge
(genix/hardware/production-cart/design.md secs 4-6, plan
genix/docs/plans/production-cart.md sec 8): two 2 MB slots over the
flash image and 8 MB of PSRAM in four banks (RAM_ON and the BANK
fields steer two pointer chunks; writes go through functions that
write PSRAM or count a flash write), the CTRL/BANK/DOUT/STATUS/DATA/
SRM_ON registers in the /TIME window, and the SD engine with a card
behind it: `genix/cart/`, a checksummed copy of the Genix tree's
`hardware/production-cart/model/` (sync it like the dongle's link/;
`scripts/check-cart-model.sh` there is the gate).  Selected when the
ROM header's name contains "GENIX CART", or by `-C`:

    blastem -C card.img probe.bin            # a card image (mkcard.py output)
    blastem -C none probe.bin                # no card in the socket
    blastem -C card.img,dip=1,dump=out.bin:8192,trace=40 probe.bin

`dip=` is the flash image switch, `dump=<path>[:<bytes>]` writes
PSRAM bank 0's first bytes big-endian at exit (the ladder reads the
probe's record there), `trace=<n>` logs the first n register
accesses with the 68000's cycle count.  The pulse train's BUSY is
carried across BlastEm's cycle deductions.  Upstream files touched:
`romdb.h` (the mapper enum), `romdb.c` (one call in configure_rom),
`genesis.c` (init reset, soft reset, the deduction), `blastem.c`
(`-C`), the Makefile.

SAVE (2026-09-12): the card image is malloc'd at attach and the
model's CMD24 writes land in that copy, so without more a write
persists within one session and not across runs (Genix plan
production-cart.md sec 8.10 item 5).  `-C card.img,save` writes the
whole image back to `card.img` at exit, `save=<path>` to another
file; same atexit shape as `dump=`, and the log line says how many
card writes it carries (`genix cart: saved N bytes of card image to
... (card writes M)`).  Without the option the file is never
touched, so a ladder leg does not mutate its build artifacts by
default.  `save` with `-C none` is an error.

    blastem -C card.img,save boot.bin          # write then reboot then verify
    blastem -C card.img,save=after.img boot.bin

## Building here

`make` needs SDL2 and GLEW development files (`pkg-config sdl2 glew
gl`).  Without a system GLEW, build 2.2.0 into a prefix and point
pkg-config at it:

    make -C glew-2.2.0 GLEW_DEST=$PWD/glew-2.2.0/dist install
    rm glew-2.2.0/dist/lib*/libGLEW.so*     # link it statically
    # glew.pc lands with prefix=/usr: point includedir= at dist/include
    # and libdir= at the dist lib dir the install actually used (lib64
    # on x86-64), or pkg-config hands the compiler /usr's missing headers
    PKG_CONFIG_PATH=$PWD/glew-2.2.0/dist/lib/pkgconfig make -j8 blastem

The Genix ladder runs the dongle legs with
`BLASTEM=~/github/blastem-genix/blastem` until a release is tagged.
