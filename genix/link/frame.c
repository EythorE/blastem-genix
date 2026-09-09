/*
 * frame.c - the link frame codec and the MIDI normaliser.
 * See frame.h.  Pure C99, no allocation, no globals.
 */
#include "frame.h"

int frame_type_len(uint8_t type)
{
    switch (type) {
    case FRAME_KEY:      return FRAME_KEY_LEN;
    case FRAME_MIDI:     return FRAME_MIDI_LEN;
    case FRAME_STATUS:   return FRAME_STATUS_LEN;
    case FRAME_OVERFLOW: return FRAME_OVERFLOW_LEN;
    default:             return -1;
    }
}

int frame_encode(const frame_t *f, uint8_t *out)
{
    int len = frame_type_len(f->type);
    if (len < 0 || f->len != len)
        return -1;
    out[0] = f->type;
    out[1] = f->len;
    for (int i = 0; i < len; i++)
        out[2 + i] = f->payload[i];
    return 2 + len;
}

void frame_decoder_reset(frame_decoder_t *d)
{
    d->pos = 0;
}

int frame_decode_byte(frame_decoder_t *d, uint8_t b, frame_t *out)
{
    if (d->pos == 0) {
        if (frame_type_len(b) < 0)
            return FRAME_DECODE_ERROR;
        d->cur.type = b;
        d->pos = 1;
        return FRAME_DECODE_MORE;
    }
    if (d->pos == 1) {
        if ((int)b != frame_type_len(d->cur.type)) {
            d->pos = 0;
            return FRAME_DECODE_ERROR;
        }
        d->cur.len = b;
        d->pos = 2;
        return FRAME_DECODE_MORE;
    }
    d->cur.payload[d->pos - 2] = b;
    d->pos++;
    if (d->pos == 2 + d->cur.len) {
        *out = d->cur;
        d->pos = 0;
        return FRAME_DECODE_DONE;
    }
    return FRAME_DECODE_MORE;
}

void frame_make_key(frame_t *f, uint8_t scancode, uint8_t make)
{
    f->type = FRAME_KEY;
    f->len = FRAME_KEY_LEN;
    f->payload[0] = scancode;
    f->payload[1] = make ? 1 : 0;
}

void frame_make_midi(frame_t *f, uint16_t delta, uint8_t status, uint8_t d1, uint8_t d2)
{
    f->type = FRAME_MIDI;
    f->len = FRAME_MIDI_LEN;
    f->payload[0] = (uint8_t)(delta >> 8);
    f->payload[1] = (uint8_t)delta;
    f->payload[2] = status;
    f->payload[3] = d1;
    f->payload[4] = d2;
}

void frame_make_status(frame_t *f, uint8_t flags, uint8_t major, uint8_t minor)
{
    f->type = FRAME_STATUS;
    f->len = FRAME_STATUS_LEN;
    f->payload[0] = flags;
    f->payload[1] = major;
    f->payload[2] = minor;
}

void frame_make_overflow(frame_t *f, uint8_t count)
{
    f->type = FRAME_OVERFLOW;
    f->len = FRAME_OVERFLOW_LEN;
    f->payload[0] = count;
}

void frame_make_time_marker(frame_t *f)
{
    frame_make_midi(f, MIDI_TIME_MARKER_DELTA, MIDI_TIME_MARKER_STATUS, 0, 0);
}

/* ---- the MIDI normaliser ---- */

/* The real-time and system-common table (usb-midi-dongle.md sec 4.2,
 * rev 1.1).  Revisable here and nowhere else. */
int midi_data_bytes(uint8_t status)
{
    if (status < 0x80)
        return -1;                       /* not a status byte */
    if (status < 0xF0) {
        switch (status & 0xF0) {
        case 0xC0:                       /* program change */
        case 0xD0:                       /* channel pressure */
            return 1;
        default:
            return 2;
        }
    }
    switch (status) {
    case 0xF1: return 1;                 /* MTC quarter frame: forwarded */
    case 0xF2: return 2;                 /* song position: forwarded */
    case 0xF3: return 1;                 /* song select: forwarded */
    case 0xF6: return 0;                 /* tune request: forwarded, zero data */
    case 0xFA:                           /* start */
    case 0xFB:                           /* continue */
    case 0xFC:                           /* stop */
    case 0xFF:                           /* reset */
        return 0;
    case 0xF8:                           /* clock: dropped */
    case 0xF9:                           /* undefined (our time marker): dropped */
    case 0xFD:                           /* undefined: dropped */
    case 0xFE:                           /* active sensing: dropped */
    case 0xF0:                           /* SysEx start: dropped whole */
    case 0xF7:                           /* SysEx end */
    case 0xF4:                           /* undefined system common */
    case 0xF5:
    default:
        return -1;
    }
}

void midi_norm_reset(midi_norm_t *n)
{
    n->status = 0;
    n->need = 0;
    n->have = 0;
    n->data[0] = n->data[1] = 0;
    n->in_sysex = 0;
}

int midi_norm_byte(midi_norm_t *n, uint8_t b, uint8_t out[3])
{
    if (b >= 0xF8) {
        /* Real-time: may interleave anywhere, never disturbs running
         * status or a message in flight. */
        if (midi_data_bytes(b) < 0)
            return 0;
        out[0] = b;
        out[1] = 0;
        out[2] = 0;
        return 1;
    }
    if (b & 0x80) {
        /* A status byte ends any SysEx and any partial message. */
        n->in_sysex = 0;
        n->have = 0;
        if (b == 0xF0) {
            n->in_sysex = 1;
            n->status = 0;               /* SysEx cancels running status */
            n->need = 0;
            return 0;
        }
        int nd = midi_data_bytes(b);
        if (nd < 0) {                    /* 0xF7, 0xF4, 0xF5 */
            n->status = 0;
            n->need = 0;
            return 0;
        }
        if (b >= 0xF0) {
            /* System common: no running status afterwards. */
            n->status = 0;
            if (nd == 0) {
                out[0] = b; out[1] = 0; out[2] = 0;
                return 1;
            }
            /* Collect its data bytes under a temporary status. */
            n->status = b;
            n->need = (uint8_t)nd;
            return 0;
        }
        n->status = b;
        n->need = (uint8_t)nd;
        return 0;
    }
    /* Data byte. */
    if (n->in_sysex || n->status == 0)
        return 0;
    n->data[n->have++] = b;
    if (n->have < n->need)
        return 0;
    out[0] = n->status;
    out[1] = n->data[0];
    out[2] = n->need == 2 ? n->data[1] : 0;
    n->have = 0;
    if (n->status >= 0xF0)
        n->status = 0;                   /* system common: one-shot */
    return 1;
}
