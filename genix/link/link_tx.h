/*
 * link_tx.h - the DONGLE'S side of the nibble handshake, as pure C.
 *
 * One implementation of usb-midi-dongle.md sec 4.3-4.5 and 4.7, called
 * by the firmware's link core (core 1) and by the BlastEm port device,
 * and driven on the host by tests/test_link.c against the console-side
 * reader of link_rx.h.  No hardware, no clock: the caller reports TL
 * as it sees it and passes time in milliseconds.
 *
 * THE RULES (the normative statement; the plan's prose is refined
 * here, 2026-09-09, so that a console attaching at any moment finds
 * the link in a state it can start from):
 *
 *  1. At reset TH, TR and D0-D3 are low.  The first frame queued is
 *     the 0x04 status frame.
 *  2. TH is the level "a frame is queued or in flight".
 *  3. The link OPENS on the first TL change the dongle sees (the
 *     console's attach); until then nothing is offered, whatever is
 *     queued ("waits for the console", sec 4.7).  Once open, the
 *     FIRST nibble of a frame is offered as soon as the frame is at
 *     the head and nothing is in flight: D0-D3 take the nibble and TR
 *     toggles.  No TL change is needed for it: the console keeps the
 *     TR level of its last latch (or of its attach) as "last VALID",
 *     so a fresh toggle is always seen.
 *  4. Every LATER nibble is offered only after TL has changed (the
 *     console's ack of the one before).  D0-D3 and TR change together
 *     (one GPIO write on the board, one output vector in the model).
 *  5. RESEND.  If TL has not changed for LINK_RESEND_MS (100) while a
 *     nibble is offered, the transfer is abandoned: the next TL change
 *     re-offers the frame from its first nibble with a fresh TR
 *     toggle.  The console's hold-off (200 ms) is longer than this on
 *     purpose: by the time it re-attaches (samples TR, toggles TL) the
 *     dongle is waiting for exactly that toggle.
 *  6. Key frames enter at the FRONT of the queue, behind the frame in
 *     flight and behind earlier key frames (order within a type is
 *     kept).  So does the overflow notice.  Implemented as a second,
 *     small ring popped before the main one: one FIFO to the console,
 *     two rings in memory (the plan's "one interleaved FIFO" without
 *     an insert-in-the-middle on a byte ring).
 *  7. OVERFLOW.  The last `reserve` bytes admit only note-offs, key,
 *     status and overflow frames; everything else is dropped and
 *     counted, and the count goes out as an 0x05 frame at the front
 *     when there is room for it.  Nothing already queued is ever
 *     dropped.
 */
#ifndef LINK_TX_H
#define LINK_TX_H

#include <stdint.h>
#include "frame.h"

#define LINK_RESEND_MS   100

/* Port data register bits, as the console sees them (0xA10005). */
#define LINK_D_MASK  0x0F
#define LINK_TL      0x10
#define LINK_TR      0x20
#define LINK_TH      0x40

typedef struct {
    uint8_t th;       /* 0/1 */
    uint8_t tr;       /* 0/1 */
    uint8_t d;        /* 0..15 */
} link_out_t;

/* A byte ring of [len][frame bytes] records. */
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t head;    /* oldest record */
    uint32_t tail;    /* next free byte */
    uint32_t used;
} link_ring_t;

typedef struct {
    link_ring_t main;   /* MIDI and status, with the reserve rule */
    link_ring_t pri;    /* keys and overflow notices: popped first */
    uint32_t reserve;
    uint8_t  dropped; /* saturating count of frames dropped since the last notice */

    /* the frame in flight */
    uint8_t  cur[FRAME_MAX_BYTES];
    uint8_t  cur_nibbles;
    uint8_t  cur_valid;
    uint8_t  nib;       /* index of the offered nibble */
    uint8_t  pending;   /* a nibble is offered and unacked */
    uint32_t offer_ms;

    uint8_t  tl_last;
    uint8_t  tl_seen;
    uint8_t  opened;    /* the console has attached once (rule 3) */

    link_out_t out;

    /* counters, for the injector's echo and the tests */
    uint32_t n_offered, n_acked, n_frames, n_resent, n_dropped;
} link_tx_t;

/* main/main_cap: the FIFO storage (31 KB on the board); reserve: sec
 * 4.5's 1 KB; pri/pri_cap: the key queue (1 KB).  Queues nothing: the
 * caller pushes the status frame, then reports TL's boot level. */
void link_tx_init(link_tx_t *t, uint8_t *main, uint32_t main_cap, uint32_t reserve,
                  uint8_t *pri, uint32_t pri_cap);

/* Queue a frame at the back (MIDI, status) or the front (keys).  The
 * overflow policy applies to both; returns 0 when queued, -1 when
 * dropped and counted. */
int link_tx_push(link_tx_t *t, const frame_t *f, uint32_t now_ms);
int link_tx_push_front(link_tx_t *t, const frame_t *f, uint32_t now_ms);

/* Report TL's level as sampled now (call on every edge, or every
 * poll; only changes matter).  The FIRST call after link_tx_init is
 * the boot sample and reports no change: make it at init, from the
 * pin, so the console's attach toggle is the first change seen. */
void link_tx_tl(link_tx_t *t, uint8_t tl_level, uint32_t now_ms);

/* Advance time: offer a first nibble, apply the resend rule, queue an
 * overflow notice.  Call after every push and TL report, and at least
 * every few milliseconds. */
void link_tx_run(link_tx_t *t, uint32_t now_ms);

static inline const link_out_t *link_tx_out(const link_tx_t *t) { return &t->out; }
static inline uint8_t link_tx_out_byte(const link_tx_t *t)
{
    return (uint8_t)(t->out.d | (t->out.tr ? LINK_TR : 0) | (t->out.th ? LINK_TH : 0));
}
static inline int link_tx_empty(const link_tx_t *t)
{
    return !t->cur_valid && t->main.used == 0 && t->pri.used == 0;
}
uint32_t link_tx_queued_frames(const link_tx_t *t);

#endif /* LINK_TX_H */
