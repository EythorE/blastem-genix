/*
 * frame.h - the MIDI dongle link frame codec (usb-midi-dongle.md sec 4.2)
 *
 * ONE reading of the wire format, compiled into three places: the
 * dongle firmware (hardware/midi-dongle/firmware/), the BlastEm port
 * device (blastem-genix, which carries a checksummed copy of this
 * directory) and the Genix console driver (pal/megadrive/dongle.c).
 * No dependencies beyond stdint; no allocation; no globals.
 *
 * A frame is [TYPE][LENGTH][PAYLOAD], sent as nibbles low nibble
 * first.  Every type has ONE fixed length; the decoder rejects any
 * other, so a misaligned nibble stream fails at its header instead of
 * being read as data.  The whole frame is at most FRAME_MAX_BYTES.
 */
#ifndef LINK_FRAME_H
#define LINK_FRAME_H

#include <stdint.h>

#define FRAME_KEY        0x01  /* [scancode][make: 1 make, 0 break]           */
#define FRAME_MIDI       0x03  /* [delta hi][delta lo][status][data1][data2]  */
#define FRAME_STATUS     0x04  /* [flags][fw major][fw minor]                 */
#define FRAME_OVERFLOW   0x05  /* [count, saturating at 255]                  */

#define FRAME_KEY_LEN       2
#define FRAME_MIDI_LEN      5
#define FRAME_STATUS_LEN    3
#define FRAME_OVERFLOW_LEN  1

#define FRAME_MAX_PAYLOAD   5
#define FRAME_MAX_BYTES     (2 + FRAME_MAX_PAYLOAD)   /* 7 bytes, 14 nibbles */
#define FRAME_MAX_NIBBLES   (2 * FRAME_MAX_BYTES)

/* Status frame flags (decided 2026-09-09, midi-dongle-build.md sec 2). */
#define STATUS_USB_KEYBOARD  0x01  /* a HID boot keyboard is enumerated  */
#define STATUS_USB_MIDI      0x02  /* a USB-MIDI class device is enumerated */
#define STATUS_VBUS_ON       0x04  /* the TPS2553 is enabled             */
#define STATUS_USB_FAULT     0x08  /* the TPS2553 reported a fault       */
#define STATUS_DIN_ACTIVE    0x10  /* a byte arrived on the DIN input    */

/* The time marker: 65.535 s of MIDI silence, status 0xF9 (a byte that
 * never appears on the wire), zero data.  A recorder adds the delta
 * and stores nothing; live mode drops it. */
#define MIDI_TIME_MARKER_STATUS  0xF9
#define MIDI_TIME_MARKER_DELTA   0xFFFF

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t payload[FRAME_MAX_PAYLOAD];
} frame_t;

/* Fixed payload length of a type, or -1 for an unknown type. */
int frame_type_len(uint8_t type);

/* Encode f into out (FRAME_MAX_BYTES).  Returns the byte count, or -1
 * when the type is unknown or the length is not the type's. */
int frame_encode(const frame_t *f, uint8_t *out);

/* Nibble i (0 = low nibble of the type byte) of an encoded frame. */
static inline uint8_t frame_nibble(const uint8_t *bytes, unsigned i)
{
    return (uint8_t)((i & 1) ? (bytes[i >> 1] >> 4) : (bytes[i >> 1] & 0x0F));
}

/* Byte-at-a-time decoder: feed bytes, get a frame. */
typedef struct {
    uint8_t pos;      /* bytes consumed of the current frame */
    frame_t cur;
} frame_decoder_t;

#define FRAME_DECODE_MORE   0
#define FRAME_DECODE_DONE   1
#define FRAME_DECODE_ERROR  (-1)

void frame_decoder_reset(frame_decoder_t *d);
/* Returns FRAME_DECODE_DONE with *out filled, FRAME_DECODE_MORE, or
 * FRAME_DECODE_ERROR (bad type or length; the decoder is reset). */
int frame_decode_byte(frame_decoder_t *d, uint8_t b, frame_t *out);

/* Convenience constructors. */
void frame_make_key(frame_t *f, uint8_t scancode, uint8_t make);
void frame_make_midi(frame_t *f, uint16_t delta, uint8_t status, uint8_t d1, uint8_t d2);
void frame_make_status(frame_t *f, uint8_t flags, uint8_t major, uint8_t minor);
void frame_make_overflow(frame_t *f, uint8_t count);
void frame_make_time_marker(frame_t *f);

/* Payload accessors for FRAME_MIDI. */
static inline uint16_t frame_midi_delta(const frame_t *f)
{
    return (uint16_t)((f->payload[0] << 8) | f->payload[1]);
}

/*
 * The MIDI normaliser (sec 4.2 "MIDI is normalized on the Pico"):
 * running status resolved, 2-byte messages zero-padded, the real-time
 * table applied, SysEx dropped whole.  Feed raw wire bytes; a complete
 * 3-byte message comes out at most once per byte.
 */
typedef struct {
    uint8_t status;    /* running status, 0 when none */
    uint8_t need;      /* data bytes still wanted for the message in flight */
    uint8_t have;      /* data bytes collected */
    uint8_t data[2];
    uint8_t in_sysex;
} midi_norm_t;

void midi_norm_reset(midi_norm_t *n);
/* Returns 1 with out[3] = {status, data1, data2} when a message
 * completes, else 0.  Dropped bytes (0xF8, 0xFE, 0xF9, 0xFD, SysEx)
 * return 0. */
int midi_norm_byte(midi_norm_t *n, uint8_t b, uint8_t out[3]);

/* Number of data bytes a status byte carries (0..2), -1 for bytes the
 * normaliser drops (0xF8, 0xF9, 0xFD, 0xFE, 0xF0, 0xF7 and the
 * undefined 0xF4/0xF5/0xF6-as-data cases handled inside). */
int midi_data_bytes(uint8_t status);

/* True for a note-off: 0x8n, or 0x9n with velocity 0.  The overflow
 * reserve (sec 4.5) admits these when everything else is dropped. */
static inline int midi_is_note_off(uint8_t status, uint8_t d2)
{
    return (status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && d2 == 0);
}

#endif /* LINK_FRAME_H */
