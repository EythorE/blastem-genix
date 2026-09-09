/*
 * script.h - the frame script: text in, a byte stream out, records back.
 *
 * The ladder, the BlastEm dongle model and the firmware's UART
 * injector all speak the BYTE STREAM; only the host tool (mkframes)
 * and the host tests read the TEXT.  midi-dongle-build.md sec 2.
 *
 * TEXT, one record per line, '#' comments:
 *   @<ms>                   absolute time (ms since power-on) the
 *                           following records are not queued before
 *   @+<ms>                  the same, relative to the previous @
 *   key <scancode> make|break
 *   midi <status> <d1> <d2>
 *   status <flags> <major> <minor>
 *   overflow <count>
 *   marker                  the 65.535 s time marker
 *   type [<ms>] "<text>"    ASCII typed as set-2 make/break pairs,
 *                           <ms> apart per character (0 = burst)
 *   stall <nibbles> [<ms>]  (emulator only) freeze the dongle after
 *                           <nibbles> nibbles of the next frame were
 *                           acked, for <ms> (default 300)
 *   detach [<ms>]           (emulator only) unplug; re-plug after
 *                           <ms> (0 = never) with a fresh power-on
 * Numbers: 0x-prefixed hex or decimal.
 *
 * BYTE STREAM: the header "GXF1", then records [type][len][payload].
 * Frame types are frame.h's, encoded by frame_encode; the directives
 * use the pseudo-types below, which the frame decoder rejects, so a
 * stream can never be mistaken for the wire.
 */
#ifndef LINK_SCRIPT_H
#define LINK_SCRIPT_H

#include <stdint.h>
#include "frame.h"

#define SCRIPT_MAGIC        "GXF1"
#define SCRIPT_MAGIC_LEN    4

#define SCRIPT_AT           0x10  /* [ms u32 BE]                        */
#define SCRIPT_STALL        0x11  /* [nibbles u16 BE][ms u16 BE]        */
#define SCRIPT_DETACH       0x12  /* [ms u16 BE]                        */
#define SCRIPT_MAX_PAYLOAD  5

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t payload[SCRIPT_MAX_PAYLOAD];
} script_rec_t;

/* Record reader over an in-memory stream.  with_header: expect and
 * check the magic (the emulator's files); without: raw records (the
 * firmware injector's UART stream). */
typedef struct {
    const uint8_t *buf;
    uint32_t len;
    uint32_t pos;
} script_reader_t;

/* Returns 0, or -1 when the header is missing or wrong. */
int script_reader_init(script_reader_t *r, const uint8_t *buf, uint32_t len, int with_header);
/* Returns 1 with *rec filled, 0 at the end, -1 on a malformed record. */
int script_reader_next(script_reader_t *r, script_rec_t *rec);

/* Byte-at-a-time record parser (the injector's UART path). */
typedef struct {
    uint8_t pos;
    script_rec_t cur;
} script_parser_t;

void script_parser_reset(script_parser_t *p);
/* 1 = record complete in *out, 0 = more, -1 = bad type or length (reset). */
int script_parser_byte(script_parser_t *p, uint8_t b, script_rec_t *out);

/* Payload length of a record type, -1 if unknown (both real frames and
 * directives). */
int script_type_len(uint8_t type);

/* Helpers to decode directive payloads. */
static inline uint32_t script_at_ms(const script_rec_t *r)
{
    return ((uint32_t)r->payload[0] << 24) | ((uint32_t)r->payload[1] << 16) |
           ((uint32_t)r->payload[2] << 8) | r->payload[3];
}
static inline uint16_t script_u16(const script_rec_t *r, int off)
{
    return (uint16_t)((r->payload[off] << 8) | r->payload[off + 1]);
}
/* A frame record as a frame_t (only for real frame types). */
static inline void script_rec_frame(const script_rec_t *r, frame_t *f)
{
    f->type = r->type;
    f->len = r->len;
    for (int i = 0; i < r->len && i < FRAME_MAX_PAYLOAD; i++)
        f->payload[i] = r->payload[i];
}

/*
 * The text parser.  Feed one line; it appends zero or more records to
 * out (up to cap bytes) and returns the byte count, or -1 with err set
 * to a short message.  st carries the @ clock between lines.
 */
typedef struct {
    uint32_t now_ms;      /* the last @ time */
} script_text_state_t;

void script_text_init(script_text_state_t *st);
int script_text_line(script_text_state_t *st, const char *line,
                     uint8_t *out, int cap, const char **err);

/* Set-2 scancode for an ASCII character; *shift set when the key needs
 * shift.  Returns 0 for characters not on the table. */
uint8_t script_ascii_scancode(char c, int *shift);

#endif /* LINK_SCRIPT_H */
