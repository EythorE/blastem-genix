/*
 * link_tx.c - the dongle's side of the handshake.  See link_tx.h.
 */
#include "link_tx.h"

static void ring_init(link_ring_t *r, uint8_t *buf, uint32_t cap)
{
    r->buf = buf;
    r->cap = cap;
    r->head = r->tail = r->used = 0;
}

void link_tx_init(link_tx_t *t, uint8_t *main, uint32_t main_cap, uint32_t reserve,
                  uint8_t *pri, uint32_t pri_cap)
{
    ring_init(&t->main, main, main_cap);
    ring_init(&t->pri, pri, pri_cap);
    t->reserve = reserve;
    t->dropped = 0;
    t->cur_valid = 0;
    t->cur_nibbles = 0;
    t->nib = 0;
    t->pending = 0;
    t->offer_ms = 0;
    t->tl_last = 0;
    t->tl_seen = 0;
    t->opened = 0;
    t->out.th = t->out.tr = t->out.d = 0;
    t->n_offered = t->n_acked = t->n_frames = t->n_resent = t->n_dropped = 0;
}

static uint32_t wrap(const link_ring_t *r, uint32_t i)
{
    return i >= r->cap ? i - r->cap : i;
}

static int ring_put(link_ring_t *r, const uint8_t *bytes, uint32_t n, uint32_t limit)
{
    uint32_t need = n + 1;
    if (limit > r->cap)
        limit = r->cap;
    if (r->used + need > limit)
        return -1;
    uint32_t pos = r->tail;
    r->buf[pos] = (uint8_t)n;
    pos = wrap(r, pos + 1);
    for (uint32_t i = 0; i < n; i++) {
        r->buf[pos] = bytes[i];
        pos = wrap(r, pos + 1);
    }
    r->tail = pos;
    r->used += need;
    return 0;
}

static uint32_t ring_get(link_ring_t *r, uint8_t *out)
{
    uint32_t pos = r->head;
    uint8_t n = r->buf[pos];
    pos = wrap(r, pos + 1);
    for (uint32_t i = 0; i < n; i++) {
        out[i] = r->buf[pos];
        pos = wrap(r, pos + 1);
    }
    r->head = pos;
    r->used -= (uint32_t)n + 1;
    return n;
}

static int is_priority(const frame_t *f)
{
    return f->type == FRAME_KEY || f->type == FRAME_OVERFLOW;
}

/* Sec 4.5: below the reserve only note-offs, keys, status and the
 * notice itself are admitted. */
static int admit_in_reserve(const frame_t *f)
{
    switch (f->type) {
    case FRAME_KEY:
    case FRAME_STATUS:
    case FRAME_OVERFLOW:
        return 1;
    case FRAME_MIDI:
        return midi_is_note_off(f->payload[2], f->payload[4]);
    default:
        return 0;
    }
}

static int enqueue(link_tx_t *t, const frame_t *f)
{
    uint8_t bytes[FRAME_MAX_BYTES];
    int n = frame_encode(f, bytes);
    if (n < 0)
        return -1;
    int rc;
    if (is_priority(f)) {
        rc = ring_put(&t->pri, bytes, (uint32_t)n, t->pri.cap);
    } else {
        uint32_t limit = t->main.cap;
        if (!admit_in_reserve(f))
            limit = t->main.cap > t->reserve ? t->main.cap - t->reserve : 0;
        rc = ring_put(&t->main, bytes, (uint32_t)n, limit);
    }
    if (rc < 0) {
        if (t->dropped < 255)
            t->dropped++;
        t->n_dropped++;
    }
    return rc;
}

static void dequeue(link_tx_t *t)
{
    uint32_t n;
    if (t->pri.used)
        n = ring_get(&t->pri, t->cur);
    else
        n = ring_get(&t->main, t->cur);
    t->cur_nibbles = (uint8_t)(2 * n);
    t->cur_valid = 1;
    t->nib = 0;
}

static void update_th(link_tx_t *t)
{
    t->out.th = (t->cur_valid || t->main.used || t->pri.used) ? 1 : 0;
}

static void offer(link_tx_t *t, uint32_t now_ms)
{
    t->out.d = frame_nibble(t->cur, t->nib);
    t->out.tr ^= 1;
    t->offer_ms = now_ms;
    t->pending = 1;
    t->n_offered++;
}

static void notice(link_tx_t *t)
{
    if (!t->dropped)
        return;
    frame_t f;
    frame_make_overflow(&f, t->dropped);
    uint8_t bytes[FRAME_MAX_BYTES];
    int n = frame_encode(&f, bytes);
    if (ring_put(&t->pri, bytes, (uint32_t)n, t->pri.cap) == 0)
        t->dropped = 0;      /* else: no room yet, try again next run */
}

/* Load the head frame and offer its first nibble (rule 3). */
static void pump(link_tx_t *t, uint32_t now_ms)
{
    notice(t);
    if (!t->cur_valid && (t->pri.used || t->main.used))
        dequeue(t);
    if (t->opened && t->cur_valid && !t->pending)
        offer(t, now_ms);
    update_th(t);
}

int link_tx_push(link_tx_t *t, const frame_t *f, uint32_t now_ms)
{
    int rc = enqueue(t, f);
    link_tx_run(t, now_ms);
    return rc;
}

int link_tx_push_front(link_tx_t *t, const frame_t *f, uint32_t now_ms)
{
    /* the same call: the type decides the ring (rule 6) */
    return link_tx_push(t, f, now_ms);
}

void link_tx_tl(link_tx_t *t, uint8_t tl_level, uint32_t now_ms)
{
    tl_level = tl_level ? 1 : 0;
    if (!t->tl_seen) {
        t->tl_seen = 1;
        t->tl_last = tl_level;
        return;
    }
    if (tl_level == t->tl_last)
        return;
    t->tl_last = tl_level;
    if (!t->opened) {
        t->opened = 1;               /* the console's attach: the link is open */
        pump(t, now_ms);
        return;
    }

    if (t->pending) {
        if (now_ms - t->offer_ms < LINK_RESEND_MS) {
            /* the ack of the offered nibble (rule 4) */
            t->n_acked++;
            t->nib++;
            if (t->nib >= t->cur_nibbles) {
                t->cur_valid = 0;
                t->pending = 0;
                t->n_frames++;
                pump(t, now_ms);
            } else {
                offer(t, now_ms);
            }
        } else {
            /* a stale offer: the console re-attached (rule 5) */
            t->nib = 0;
            t->n_resent++;
            offer(t, now_ms);
        }
    }
    update_th(t);
}

void link_tx_run(link_tx_t *t, uint32_t now_ms)
{
    if (t->pending && now_ms - t->offer_ms >= LINK_RESEND_MS && t->nib != 0) {
        /* abandon the transfer; the re-offer waits for TL (rule 5) */
        t->nib = 0;
        t->out.d = frame_nibble(t->cur, 0);
    }
    pump(t, now_ms);
}

static uint32_t ring_frames(const link_ring_t *r)
{
    uint32_t n = 0, pos = r->head, left = r->used;
    while (left) {
        uint8_t len = r->buf[pos];
        pos = wrap(r, pos + 1 + len);
        left -= (uint32_t)len + 1;
        n++;
    }
    return n;
}

uint32_t link_tx_queued_frames(const link_tx_t *t)
{
    return (t->cur_valid ? 1 : 0) + ring_frames(&t->pri) + ring_frames(&t->main);
}
