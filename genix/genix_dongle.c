/*
 * genix_dongle.c - the Genix MIDI dongle port device.  See genix_dongle.h.
 *
 * Time: BlastEm's cycle stamps are master-clock cycles that are
 * periodically deducted (io_adjust_cycles); the model keeps its own
 * 64-bit elapsed count and turns it into the milliseconds link_tx.c
 * wants (the 100 ms resend rule, the script's @ times, MIDI deltas).
 * The master clock comes from the running genesis context, so PAL
 * runs (-r E) keep real milliseconds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "genix_dongle.h"
#include "../blastem.h"
#include "../genesis.h"
#include "../util.h"
#include "link/frame.h"
#include "link/script.h"
#include "link/link_tx.h"

#ifndef GENIX_LINK_SHA
#define GENIX_LINK_SHA "unknown"
#endif

#define FIFO_MAIN   (31 * 1024)
#define FIFO_PRI    (1 * 1024)
#define FIFO_RESERVE 1024
#define FW_MAJOR 0
#define FW_MINOR 1
#define MARKER_SILENCE_MS 65535u

char *genix_dongle_spec;
int   genix_dongle_keys = -1;      /* auto; io.c resolves it */

/* Read one ",name=<int>" out of the spec without disturbing it.  The
 * port is wanted before any device is created (io.c's
 * setup_io_devices), so it cannot come from the parsed options. */
static int spec_opt(const char *name, int dflt)
{
    const char *p = genix_dongle_spec;
    size_t n = strlen(name);

    if (!p)
        return dflt;
    while ((p = strchr(p, ',')) != NULL) {
        p++;
        if (!strncmp(p, name, n) && p[n] == '=')
            return atoi(p + n + 1);
    }
    return dflt;
}

int genix_dongle_port(void)
{
    return spec_opt("port", 2) == 1 ? 1 : 2;
}

typedef struct {
    link_tx_t tx;
    uint8_t main_ring[FIFO_MAIN];
    uint8_t pri_ring[FIFO_PRI];

    /* time */
    uint64_t elapsed;          /* master clock cycles since creation */
    uint32_t last_cycle;
    uint32_t cycles_per_ms;
    uint32_t latency_cycles;
    uint32_t latency_us;

    /* the visible outputs, behind the response latency */
    link_out_t vis, nxt;
    uint32_t apply_cycle;      /* absolute (elapsed-based) */
    uint64_t apply_at;
    int nxt_pending;

    /* the console's TL as last seen */
    uint8_t tl;

    /* fault injection */
    int frozen;
    uint64_t thaw_ms;
    int stall_armed;
    uint32_t stall_after, stall_acks_seen, stall_ms;
    uint32_t acks_at_arm;
    int floating;
    uint64_t reattach_ms;      /* 0 = never */

    /* sources */
    uint8_t *script;
    uint32_t script_len;
    script_reader_t reader;
    script_rec_t rec;
    int rec_valid;
    uint64_t rec_at_ms;        /* the @ time the pending record waits for */
    int midi_fd;
    midi_norm_t norm;
    uint64_t last_midi_ms;
    uint64_t last_midi_poll_ms;
    int midi_any;

    /* counters for the attach line */
    uint32_t n_key, n_midi;
    int announced;

    /* ,trace=N: log the first N port events to stderr */
    uint32_t trace_left;
    uint32_t n_adjust;
} dongle_t;

static uint64_t now_ms(dongle_t *d)
{
    return d->elapsed / d->cycles_per_ms;
}

static void trace(dongle_t *d, const char *what, unsigned a, unsigned b)
{
    if (!d->trace_left)
        return;
    d->trace_left--;
    fprintf(stderr, "genix dongle: %9llu us %s %x %x | tl=%u nib=%u pend=%u open=%u off=%lu ack=%lu\n",
            (unsigned long long)(d->elapsed * 1000 / d->cycles_per_ms), what, a, b,
            d->tl, d->tx.nib, d->tx.pending, d->tx.opened,
            (unsigned long)d->tx.n_offered, (unsigned long)d->tx.n_acked);
}

static void sched(dongle_t *d)
{
    if (memcmp(&d->tx.out, &d->nxt, sizeof d->nxt) != 0) {
        d->nxt = d->tx.out;
        d->apply_at = d->elapsed + d->latency_cycles;
        d->nxt_pending = 1;
        trace(d, "out d/tr/th", d->nxt.d, (unsigned)(d->nxt.tr | d->nxt.th << 1));
    }
}

static void apply(dongle_t *d)
{
    if (d->nxt_pending && d->elapsed >= d->apply_at) {
        d->vis = d->nxt;
        d->nxt_pending = 0;
    }
}

static void power_on(dongle_t *d)
{
    link_tx_init(&d->tx, d->main_ring, sizeof d->main_ring, FIFO_RESERVE,
                 d->pri_ring, sizeof d->pri_ring);
    link_tx_tl(&d->tx, d->tl, (uint32_t)now_ms(d));
    frame_t f;
    frame_make_status(&f, 0, FW_MAJOR, FW_MINOR);
    link_tx_push(&d->tx, &f, (uint32_t)now_ms(d));
    memset(&d->vis, 0, sizeof d->vis);
    memset(&d->nxt, 0, sizeof d->nxt);
    d->nxt_pending = 0;
    sched(d);
}

static void push_frame(dongle_t *d, const frame_t *f)
{
    uint32_t ms = (uint32_t)now_ms(d);
    if (f->type == FRAME_MIDI) {
        frame_t m = *f;
        if (m.payload[2] != MIDI_TIME_MARKER_STATUS) {
            /* the delta is the dongle's: ms since the previous MIDI frame */
            uint64_t delta = d->midi_any ? now_ms(d) - d->last_midi_ms : 0;
            if (delta > 0xFFFE)
                delta = 0xFFFE;
            m.payload[0] = (uint8_t)(delta >> 8);
            m.payload[1] = (uint8_t)delta;
        }
        d->last_midi_ms = now_ms(d);
        d->midi_any = 1;
        d->n_midi++;
        link_tx_push(&d->tx, &m, ms);
    } else {
        if (f->type == FRAME_KEY)
            d->n_key++;
        link_tx_push(&d->tx, f, ms);
    }
}

static uint8_t *read_file(const char *path, uint32_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) { fclose(fp); return NULL; }
    uint8_t *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); fclose(fp); return NULL; }
    fclose(fp);
    *len = (uint32_t)n;
    return buf;
}

static void announce(dongle_t *d, const char *what)
{
    fprintf(stderr, "genix dongle: link %s port %d source %s latency %u us keys %s\n",
            GENIX_LINK_SHA, genix_dongle_port(), what, d->latency_us,
            genix_dongle_keys < 0 ? "auto" : (genix_dongle_keys ? "on" : "off"));
    fflush(stderr);
}

void genix_dongle_attach(io_port *port)
{
    dongle_t *d = calloc(1, sizeof *d);
    port->device.genix.ctx = d;
    d->cycles_per_ms = 53693175 / 1000;   /* refined from the context on first run */
    d->latency_us = GENIX_DONGLE_LATENCY_US;
    d->midi_fd = -1;
    d->tl = 0;                              /* an undriven TL reads LOW at the dongle (see the input model below) */

    char *spec = genix_dongle_spec ? strdup(genix_dongle_spec) : NULL;
    char *source = spec, *opts = NULL;
    if (spec) {
        opts = strchr(spec, ',');
        if (opts)
            *opts++ = 0;
    }
    while (opts && *opts) {
        char *next = strchr(opts, ',');
        if (next)
            *next++ = 0;
        if (!strncmp(opts, "latency=", 8)) {
            d->latency_us = (uint32_t)atoi(opts + 8);
        } else if (!strncmp(opts, "trace=", 6)) {
            d->trace_left = (uint32_t)atoi(opts + 6);
        } else if (!strncmp(opts, "port=", 5)) {
            /* read by genix_dongle_port() straight off the spec */
        } else if (!strncmp(opts, "keys=", 5)) {
            const char *v = opts + 5;
            genix_dongle_keys = (!strcmp(v, "on") || !strcmp(v, "1")) ? 1 :
                                (!strcmp(v, "off") || !strcmp(v, "0")) ? 0 : -1;
        } else {
            fatal_error("genix dongle: unknown option '%s'\n", opts);
        }
        opts = next;
    }
    if (!source || !*source) {
        fatal_error("genix dongle: -M needs a frame stream path or midi:<path>\n");
    } else if (!strncmp(source, "midi:", 5)) {
        d->midi_fd = open(source + 5, O_RDONLY | O_NONBLOCK);
        if (d->midi_fd < 0)
            fatal_error("genix dongle: %s: %s\n", source + 5, strerror(errno));
        midi_norm_reset(&d->norm);
        announce(d, source);
    } else {
        d->script = read_file(source, &d->script_len);
        if (!d->script)
            fatal_error("genix dongle: %s: %s\n", source, strerror(errno));
        if (script_reader_init(&d->reader, d->script, d->script_len, 1) < 0)
            fatal_error("genix dongle: %s: not a GXF1 frame stream (mkframes output)\n", source);
        announce(d, source);
    }
    d->latency_cycles = d->latency_us * d->cycles_per_ms / 1000;
    free(spec);
    power_on(d);
}

static void refine_clock(dongle_t *d)
{
    if (current_system && current_system->type == SYSTEM_GENESIS) {
        genesis_context *gen = (genesis_context *)current_system;
        if (gen->master_clock) {
            d->cycles_per_ms = gen->master_clock / 1000;
            d->latency_cycles = d->latency_us * d->cycles_per_ms / 1000;
        }
    }
}

/* Advance the model's clock to current_cycle. */
static void advance(dongle_t *d, uint32_t current_cycle)
{
    if (!d->announced) {
        refine_clock(d);
        d->announced = 1;
        d->last_cycle = current_cycle;
    }
    if (current_cycle > d->last_cycle)
        d->elapsed += current_cycle - d->last_cycle;
    d->last_cycle = current_cycle;
}

static void handle_stall_arm(dongle_t *d)
{
    if (d->stall_armed && !d->frozen &&
        d->tx.n_acked - d->acks_at_arm >= d->stall_after) {
        d->frozen = 1;
        d->thaw_ms = now_ms(d) + d->stall_ms;
        d->stall_armed = 0;
        fprintf(stderr, "genix dongle: stall for %u ms at %llu ms\n",
                d->stall_ms, (unsigned long long)now_ms(d));
    }
}

static void feed_script(dongle_t *d)
{
    for (;;) {
        if (!d->rec_valid) {
            int rc = script_reader_next(&d->reader, &d->rec);
            if (rc <= 0) {
                if (rc < 0) {
                    fprintf(stderr, "genix dongle: malformed frame stream at byte %u\n", d->reader.pos);
                    d->reader.pos = d->reader.len;
                }
                return;
            }
            d->rec_valid = 1;
        }
        if (d->rec.type == SCRIPT_AT) {
            d->rec_at_ms = script_at_ms(&d->rec);
            d->rec_valid = 0;
            continue;
        }
        if (now_ms(d) < d->rec_at_ms)
            return;
        d->rec_valid = 0;
        switch (d->rec.type) {
        case SCRIPT_STALL:
            d->stall_armed = 1;
            d->stall_after = script_u16(&d->rec, 0);
            d->stall_ms = script_u16(&d->rec, 2);
            d->acks_at_arm = d->tx.n_acked;
            break;
        case SCRIPT_DETACH: {
            uint16_t ms = script_u16(&d->rec, 0);
            d->floating = 1;
            d->reattach_ms = ms ? now_ms(d) + ms : 0;
            fprintf(stderr, "genix dongle: detach at %llu ms%s\n",
                    (unsigned long long)now_ms(d), ms ? "" : " (for good)");
            break;
        }
        default: {
            frame_t f;
            script_rec_frame(&d->rec, &f);
            push_frame(d, &f);
            break;
        }
        }
        if (d->floating)
            return;            /* nothing is queued into an unplugged dongle */
    }
}

static void feed_midi(dongle_t *d)
{
    if (d->midi_fd < 0 || now_ms(d) == d->last_midi_poll_ms)
        return;
    d->last_midi_poll_ms = now_ms(d);
    uint8_t buf[64];
    ssize_t n = read(d->midi_fd, buf, sizeof buf);
    if (n <= 0)
        return;
    for (ssize_t i = 0; i < n; i++) {
        uint8_t msg[3];
        if (midi_norm_byte(&d->norm, buf[i], msg)) {
            frame_t f;
            frame_make_midi(&f, 0, msg[0], msg[1], msg[2]);
            push_frame(d, &f);
        }
    }
}

static void time_marker(dongle_t *d)
{
    if (d->midi_any && now_ms(d) - d->last_midi_ms >= MARKER_SILENCE_MS) {
        frame_t f;
        frame_make_time_marker(&f);
        link_tx_push(&d->tx, &f, (uint32_t)now_ms(d));
        d->last_midi_ms = now_ms(d);
    }
}

static void step(dongle_t *d)
{
    uint32_t ms = (uint32_t)now_ms(d);
    if (d->floating) {
        if (d->reattach_ms && now_ms(d) >= d->reattach_ms) {
            d->floating = 0;
            d->reattach_ms = 0;
            d->frozen = 0;
            fprintf(stderr, "genix dongle: reattach at %llu ms\n", (unsigned long long)ms);
            power_on(d);
        } else {
            return;
        }
    }
    if (d->frozen) {
        if (now_ms(d) >= d->thaw_ms) {
            d->frozen = 0;
            /* the firmware wakes and sees TL where it is now */
            link_tx_tl(&d->tx, d->tl, ms);
        } else {
            return;
        }
    }
    feed_script(d);
    feed_midi(d);
    time_marker(d);
    link_tx_run(&d->tx, ms);
    handle_stall_arm(d);
    sched(d);
    apply(d);
}

/* HOST KEYBOARD PASSTHROUGH (midi-dongle-build.md sec 4, the corrected
 * note): BlastEm's own key events pushed in as 0x01 key frames.  Its
 * scancodes are already set 2 (render_sdl.c's scancode_map), which is
 * what the frame format and the console's keyboard.c want, so this is
 * the script `type` directive's path with a live hand on the other
 * end.  Keys ride the priority ring, so they jump ahead of queued MIDI
 * exactly as the product's USB keyboard does. */
void genix_dongle_key(io_port *port, uint8_t scancode, int make)
{
    dongle_t *d = port->device.genix.ctx;
    frame_t f;

    if (!d || !scancode || d->floating)
        return;
    frame_make_key(&f, scancode, make ? 1 : 0);
    push_frame(d, &f);
}

void genix_dongle_pins(io_port *port, uint8_t output, uint32_t current_cycle)
{
    dongle_t *d = port->device.genix.ctx;
    if (!d)
        return;
    advance(d, current_cycle);
    /* TL as the dongle's input sees it: the console's latch while the
     * console drives the pin, else LOW.  The board reads TL through a
     * 1k/1.8k divider (its unpowered-pin protection), and that 2.8k
     * load pulls the console's weak pull-up down to 0.27 V at the
     * port (measured 2026-09-14 on the first assembled board), below
     * the RP2350's input threshold: an undriven TL is a 0 at the
     * dongle, steady.  This model said "pull-up, steady high" until
     * then, and the kernel claimed the port with TL high on that
     * word, which on hardware was an edge (and the attach toggle a
     * second one, taken as an ack).  Not the effective vector BlastEm
     * computes either: that restarts its slow-rise model on every
     * control write, so an input pin with a 0 in the latch reads 0
     * for ~4 us after any direction change - the kernel's port
     * initialisation (CTRL2 = 0x60) glitched TL low and back, two
     * edges that opened the link and ate a nibble before the real
     * attach (found 2026-09-09). */
    uint8_t tl = (port->control & LINK_TL) ? ((output & LINK_TL) ? 1 : 0) : 0;
    trace(d, tl != d->tl ? "tl edge ->" : "pins", tl, output);
    d->tl = tl;
    if (!d->floating && !d->frozen) {
        link_tx_tl(&d->tx, tl, (uint32_t)now_ms(d));
        handle_stall_arm(d);
        sched(d);
    }
    step(d);
}

uint8_t genix_dongle_read(io_port *port, uint32_t current_cycle)
{
    dongle_t *d = port->device.genix.ctx;
    if (!d)
        return 0x7F;
    advance(d, current_cycle);
    step(d);
    apply(d);
    if (d->floating)
        return 0x7F;                   /* pull-ups: TH looks ready, TR never moves */
    uint8_t v = (uint8_t)(d->vis.d | (d->vis.tr ? LINK_TR : 0) | (d->vis.th ? LINK_TH : 0));
    trace(d, "read", v, 0);
    return v;
}

void genix_dongle_run(io_port *port, uint32_t current_cycle)
{
    dongle_t *d = port->device.genix.ctx;
    if (!d)
        return;
    advance(d, current_cycle);
    if (d->trace_left) {
        static uint64_t next_s;
        if (now_ms(d) >= next_s) {
            fprintf(stderr, "genix dongle: clock %llu ms (cycle %u, adjusts %u, cpm %u, frames left %d)\n",
                    (unsigned long long)now_ms(d), current_cycle, d->n_adjust, d->cycles_per_ms, exit_after);
            next_s = now_ms(d) + 1000;
        }
    }
    step(d);
}

void genix_dongle_adjust_cycles(io_port *port, uint32_t deduction)
{
    dongle_t *d = port->device.genix.ctx;
    if (!d)
        return;
    d->n_adjust++;
    if (d->last_cycle >= deduction)
        d->last_cycle -= deduction;
    else
        d->last_cycle = 0;
}
