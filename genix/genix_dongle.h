/*
 * genix_dongle.h - the Genix MIDI dongle as a controller-port device.
 *
 * The dongle (genix/docs/plans/usb-midi-dongle.md sec 4, build plan
 * genix/docs/plans/midi-dongle-build.md sec 4) speaks a nibble
 * handshake on D0-D3 / TR / TH with TL as the console's ack.  This
 * device runs the dongle's own handshake code (genix/link/link_tx.c,
 * a checksummed copy of the firmware's) behind a response-latency
 * model, fed by a frame script or a raw MIDI byte stream.
 *
 * Selected with `-M <spec>` on the command line, which puts the
 * device on controller port 2:
 *
 *   -M keys.fst                 a GXF1 frame stream (mkframes output)
 *   -M midi:/dev/snd/midiC1D0   raw MIDI bytes from a device node,
 *                               FIFO or file, normalised like the
 *                               firmware does
 *   ...,latency=<us>            the dongle's response latency after a
 *                               TL edge (default GENIX_DONGLE_LATENCY_US)
 */
#ifndef GENIX_DONGLE_H_
#define GENIX_DONGLE_H_

#include <stdint.h>
#include "../io.h"

#define GENIX_DONGLE_LATENCY_US   3

extern char *genix_dongle_spec;

void    genix_dongle_attach(io_port *port);
/* The console's effective pin vector changed (data or control write). */
void    genix_dongle_pins(io_port *port, uint8_t output, uint32_t current_cycle);
/* The bits the dongle drives: D0-D3, TR, TH. */
uint8_t genix_dongle_read(io_port *port, uint32_t current_cycle);
void    genix_dongle_run(io_port *port, uint32_t current_cycle);
void    genix_dongle_adjust_cycles(io_port *port, uint32_t deduction);

#endif
