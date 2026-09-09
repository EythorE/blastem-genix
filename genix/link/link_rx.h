/*
 * link_rx.h - the CONSOLE'S side of the handshake: one frame reader,
 * instantiated twice.
 *
 * The 68000 reads the port register in a tight loop and cannot afford
 * a function pointer per nibble, so the reader is a header of static
 * functions bound to three macros by whoever includes it:
 *
 *   LRX_READ()       the port data register as a uint8_t (D0-D3 in
 *                    bits 0-3, TL bit 4, TR bit 5, TH bit 6)
 *   LRX_WRITE_TL(v)  drive TL to v (0/1); the other pins are inputs
 *   LRX_TIMEOUT      iterations of the TR wait loop before giving up
 *                    (~2,000 68000 cycles = 260 us on the console;
 *                    usb-midi-dongle.md sec 4.7 TIMEOUT)
 *
 * pal/megadrive/dongle.c binds them to 0xA10005; tests/test_link.c
 * binds them to a simulated dongle running link_tx.c.  Rules, the
 * counterpart of link_tx.h's:
 *
 *  a. ATTACH: TL becomes an output; sample TR as "last VALID"; then
 *     TOGGLE TL, so a dongle whose offer went stale re-offers its
 *     frame from the first nibble.
 *  b. A nibble is taken when TR differs from last VALID: latch D0-D3,
 *     record TR, toggle TL.
 *  c. A frame is [type][len][payload] as nibbles, low first; the
 *     header is checked against frame.h before the payload is read.
 *  d. TIMEOUT or a bad header: the frame is abandoned, the caller
 *     marks the port detached and holds off LINK_HOLDOFF_MS (200)
 *     before attaching again.
 *  e. After the last ack of a frame, TH is sampled again only after
 *     LRX_TH_SETTLE reads (the dongle's response latency: it drops
 *     TH after the ack it pops the frame on).
 */
#ifndef LINK_RX_H
#define LINK_RX_H

#include <stdint.h>
#include "frame.h"
#include "link_tx.h"   /* the pin bit definitions */

#define LINK_HOLDOFF_MS   200

#ifndef LRX_TH_SETTLE
#define LRX_TH_SETTLE 8   /* port reads (~8 us on the console) */
#endif

typedef struct {
    uint8_t tr_last;   /* LINK_TR or 0 */
    uint8_t tl;        /* 0/1, the level we drive */
} link_rx_t;

#define LINK_RX_OK        0
#define LINK_RX_TIMEOUT  (-1)
#define LINK_RX_BADHDR   (-2)

#endif /* LINK_RX_H */

/* The instantiation, guarded separately so a translation unit can
 * include the header for the types alone. */
#if defined(LRX_READ) && !defined(LRX_INSTANTIATED)
#define LRX_INSTANTIATED

static void lrx_attach(link_rx_t *rx)
{
    rx->tr_last = (uint8_t)(LRX_READ() & LINK_TR);
    rx->tl ^= 1;
    LRX_WRITE_TL(rx->tl);
}

/* TH as a level: 1 when the dongle says a frame is queued. */
static int lrx_avail(void)
{
    return (LRX_READ() & LINK_TH) != 0;
}

static int lrx_nibble(link_rx_t *rx, uint8_t *nib)
{
    uint8_t v;
    unsigned i;
    for (i = 0; i < LRX_TIMEOUT; i++) {
        v = LRX_READ();
        if ((uint8_t)(v & LINK_TR) != rx->tr_last) {
            rx->tr_last = (uint8_t)(v & LINK_TR);
            *nib = (uint8_t)(v & LINK_D_MASK);
            rx->tl ^= 1;
            LRX_WRITE_TL(rx->tl);
            return LINK_RX_OK;
        }
    }
    return LINK_RX_TIMEOUT;
}

/* Read one frame into out (FRAME_MAX_BYTES).  Returns the byte count
 * (>= 2), LINK_RX_TIMEOUT or LINK_RX_BADHDR. */
static int lrx_frame(link_rx_t *rx, uint8_t *out)
{
    uint8_t lo, hi;
    int len, i;

    if (lrx_nibble(rx, &lo) || lrx_nibble(rx, &hi))
        return LINK_RX_TIMEOUT;
    out[0] = (uint8_t)(lo | (hi << 4));
    len = frame_type_len(out[0]);
    if (len < 0)
        return LINK_RX_BADHDR;
    if (lrx_nibble(rx, &lo) || lrx_nibble(rx, &hi))
        return LINK_RX_TIMEOUT;
    out[1] = (uint8_t)(lo | (hi << 4));
    if (out[1] != len)
        return LINK_RX_BADHDR;
    for (i = 0; i < len; i++) {
        if (lrx_nibble(rx, &lo) || lrx_nibble(rx, &hi))
            return LINK_RX_TIMEOUT;
        out[2 + i] = (uint8_t)(lo | (hi << 4));
    }
    /* rule e: let TH settle before the caller looks at it */
    for (i = 0; i < LRX_TH_SETTLE; i++)
        (void)LRX_READ();
    return 2 + len;
}

#endif /* LRX_READ */
