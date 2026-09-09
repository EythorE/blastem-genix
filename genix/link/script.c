/*
 * script.c - the frame script's byte-stream reader and text parser.
 * Pure C99, no stdio, no allocation.  See script.h.
 */
#include "script.h"

int script_type_len(uint8_t type)
{
    switch (type) {
    case SCRIPT_AT:     return 4;
    case SCRIPT_STALL:  return 4;
    case SCRIPT_DETACH: return 2;
    default:            return frame_type_len(type);
    }
}

int script_reader_init(script_reader_t *r, const uint8_t *buf, uint32_t len, int with_header)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    if (with_header) {
        if (len < SCRIPT_MAGIC_LEN)
            return -1;
        for (int i = 0; i < SCRIPT_MAGIC_LEN; i++)
            if (buf[i] != (uint8_t)SCRIPT_MAGIC[i])
                return -1;
        r->pos = SCRIPT_MAGIC_LEN;
    }
    return 0;
}

int script_reader_next(script_reader_t *r, script_rec_t *rec)
{
    if (r->pos >= r->len)
        return 0;
    if (r->len - r->pos < 2)
        return -1;
    uint8_t type = r->buf[r->pos];
    uint8_t len = r->buf[r->pos + 1];
    int want = script_type_len(type);
    if (want < 0 || len != want || r->len - r->pos < 2u + len)
        return -1;
    rec->type = type;
    rec->len = len;
    for (int i = 0; i < len; i++)
        rec->payload[i] = r->buf[r->pos + 2 + i];
    r->pos += 2u + len;
    return 1;
}

void script_parser_reset(script_parser_t *p)
{
    p->pos = 0;
}

int script_parser_byte(script_parser_t *p, uint8_t b, script_rec_t *out)
{
    if (p->pos == 0) {
        if (script_type_len(b) < 0)
            return -1;
        p->cur.type = b;
        p->pos = 1;
        return 0;
    }
    if (p->pos == 1) {
        if ((int)b != script_type_len(p->cur.type)) {
            p->pos = 0;
            return -1;
        }
        p->cur.len = b;
        p->pos = 2;
        if (b == 0) {
            *out = p->cur;
            p->pos = 0;
            return 1;
        }
        return 0;
    }
    p->cur.payload[p->pos - 2] = b;
    p->pos++;
    if (p->pos == 2 + p->cur.len) {
        *out = p->cur;
        p->pos = 0;
        return 1;
    }
    return 0;
}

/* ---- the text parser ---- */

/* Set-2 (Saturn keyboard) scancodes, the single-byte block the console
 * indexes its keymap with (pal/megadrive/keyboard.c saturn_keymap). */
static const struct { char c; uint8_t sc; uint8_t shift; } ascii_table[] = {
    {'a',0x1C,0},{'b',0x32,0},{'c',0x21,0},{'d',0x23,0},{'e',0x24,0},
    {'f',0x2B,0},{'g',0x34,0},{'h',0x33,0},{'i',0x43,0},{'j',0x3B,0},
    {'k',0x42,0},{'l',0x4B,0},{'m',0x3A,0},{'n',0x31,0},{'o',0x44,0},
    {'p',0x4D,0},{'q',0x15,0},{'r',0x2D,0},{'s',0x1B,0},{'t',0x2C,0},
    {'u',0x3C,0},{'v',0x2A,0},{'w',0x1D,0},{'x',0x22,0},{'y',0x35,0},
    {'z',0x1A,0},
    {'0',0x45,0},{'1',0x16,0},{'2',0x1E,0},{'3',0x26,0},{'4',0x25,0},
    {'5',0x2E,0},{'6',0x36,0},{'7',0x3D,0},{'8',0x3E,0},{'9',0x46,0},
    {' ',0x29,0},{'\n',0x5A,0},{'\t',0x0D,0},{27,0x76,0},{8,0x66,0},
    {'-',0x4E,0},{'=',0x55,0},{'[',0x54,0},{']',0x5B,0},{';',0x4C,0},
    {'\'',0x52,0},{'`',0x0E,0},{',',0x41,0},{'.',0x49,0},{'/',0x4A,0},
    {'\\',0x5D,0},
    {')',0x45,1},{'!',0x16,1},{'@',0x1E,1},{'#',0x26,1},{'$',0x25,1},
    {'%',0x2E,1},{'^',0x36,1},{'&',0x3D,1},{'*',0x3E,1},{'(',0x46,1},
    {'_',0x4E,1},{'+',0x55,1},{'{',0x54,1},{'}',0x5B,1},{':',0x4C,1},
    {'"',0x52,1},{'~',0x0E,1},{'<',0x41,1},{'>',0x49,1},{'?',0x4A,1},
    {'|',0x5D,1},
};
#define SC_LSHIFT 0x12

uint8_t script_ascii_scancode(char c, int *shift)
{
    if (c >= 'A' && c <= 'Z') {
        *shift = 1;
        return script_ascii_scancode((char)(c + 32), &(int){0});
    }
    for (unsigned i = 0; i < sizeof ascii_table / sizeof ascii_table[0]; i++) {
        if (ascii_table[i].c == c) {
            *shift = ascii_table[i].shift;
            return ascii_table[i].sc;
        }
    }
    *shift = 0;
    return 0;
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r')
        s++;
    return s;
}

static int is_end(const char *s)
{
    s = skip_ws(s);
    return *s == 0 || *s == '\n' || *s == '#';
}

/* 0x-prefixed hex or decimal; returns 0 on a bad number. */
static int parse_num(const char **sp, uint32_t *out)
{
    const char *s = skip_ws(*sp);
    uint32_t v = 0;
    int digits = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (;; s++) {
            int d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            v = (v << 4) | (uint32_t)d;
            digits++;
        }
    } else {
        for (; *s >= '0' && *s <= '9'; s++) {
            v = v * 10 + (uint32_t)(*s - '0');
            digits++;
        }
    }
    if (!digits)
        return 0;
    *sp = s;
    *out = v;
    return 1;
}

static int word_is(const char *s, const char *w, const char **rest)
{
    s = skip_ws(s);
    while (*w) {
        if (*s != *w)
            return 0;
        s++;
        w++;
    }
    if (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' && *s != '#')
        return 0;
    *rest = s;
    return 1;
}

typedef struct { uint8_t *out; int cap; int n; } sink_t;

static int emit(sink_t *k, const uint8_t *b, int len)
{
    if (k->n + len > k->cap)
        return -1;
    for (int i = 0; i < len; i++)
        k->out[k->n + i] = b[i];
    k->n += len;
    return 0;
}

static int emit_frame(sink_t *k, const frame_t *f)
{
    uint8_t b[FRAME_MAX_BYTES];
    int n = frame_encode(f, b);
    if (n < 0)
        return -1;
    return emit(k, b, n);
}

static int emit_at(sink_t *k, uint32_t ms)
{
    uint8_t b[6] = { SCRIPT_AT, 4, (uint8_t)(ms >> 24), (uint8_t)(ms >> 16),
                     (uint8_t)(ms >> 8), (uint8_t)ms };
    return emit(k, b, 6);
}

static int emit_key(sink_t *k, uint8_t sc, int make)
{
    frame_t f;
    frame_make_key(&f, sc, (uint8_t)make);
    return emit_frame(k, &f);
}

void script_text_init(script_text_state_t *st)
{
    st->now_ms = 0;
}

int script_text_line(script_text_state_t *st, const char *line,
                     uint8_t *out, int cap, const char **err)
{
    sink_t k = { out, cap, 0 };
    const char *s = skip_ws(line);
    const char *rest;
    uint32_t a, b, c;
    frame_t f;

    *err = 0;
    if (is_end(s))
        return 0;

    if (*s == '@') {
        s++;
        int rel = 0;
        if (*s == '+') { rel = 1; s++; }
        if (!parse_num(&s, &a) || !is_end(s)) { *err = "bad @ time"; return -1; }
        st->now_ms = rel ? st->now_ms + a : a;
        if (emit_at(&k, st->now_ms) < 0) { *err = "out of space"; return -1; }
        return k.n;
    }
    if (word_is(s, "key", &rest)) {
        s = rest;
        if (!parse_num(&s, &a) || a > 255) { *err = "bad scancode"; return -1; }
        int make;
        if (word_is(s, "make", &rest)) make = 1;
        else if (word_is(s, "break", &rest)) make = 0;
        else { *err = "key needs make|break"; return -1; }
        if (!is_end(rest)) { *err = "trailing text"; return -1; }
        if (emit_key(&k, (uint8_t)a, make) < 0) { *err = "out of space"; return -1; }
        return k.n;
    }
    if (word_is(s, "midi", &rest)) {
        s = rest;
        if (!parse_num(&s, &a) || a > 255 || !parse_num(&s, &b) || b > 255 ||
            !parse_num(&s, &c) || c > 255 || !is_end(s)) {
            *err = "midi needs status d1 d2"; return -1;
        }
        /* The delta is the emulator's to fill from its clock; the
         * script carries 0 here (an @ record sets the time). */
        frame_make_midi(&f, 0, (uint8_t)a, (uint8_t)b, (uint8_t)c);
        if (emit_frame(&k, &f) < 0) { *err = "out of space"; return -1; }
        return k.n;
    }
    if (word_is(s, "status", &rest)) {
        s = rest;
        if (!parse_num(&s, &a) || a > 255 || !parse_num(&s, &b) || b > 255 ||
            !parse_num(&s, &c) || c > 255 || !is_end(s)) {
            *err = "status needs flags major minor"; return -1;
        }
        frame_make_status(&f, (uint8_t)a, (uint8_t)b, (uint8_t)c);
        if (emit_frame(&k, &f) < 0) { *err = "out of space"; return -1; }
        return k.n;
    }
    if (word_is(s, "overflow", &rest)) {
        s = rest;
        if (!parse_num(&s, &a) || a > 255 || !is_end(s)) { *err = "bad overflow count"; return -1; }
        frame_make_overflow(&f, (uint8_t)a);
        if (emit_frame(&k, &f) < 0) { *err = "out of space"; return -1; }
        return k.n;
    }
    if (word_is(s, "marker", &rest)) {
        if (!is_end(rest)) { *err = "trailing text"; return -1; }
        frame_make_time_marker(&f);
        if (emit_frame(&k, &f) < 0) { *err = "out of space"; return -1; }
        return k.n;
    }
    if (word_is(s, "stall", &rest)) {
        s = rest;
        b = 300;
        if (!parse_num(&s, &a) || a > 65535) { *err = "stall needs a nibble count"; return -1; }
        if (!is_end(s) && (!parse_num(&s, &b) || b > 65535)) { *err = "bad stall ms"; return -1; }
        if (!is_end(s)) { *err = "trailing text"; return -1; }
        uint8_t r[6] = { SCRIPT_STALL, 4, (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(b >> 8), (uint8_t)b };
        if (emit(&k, r, 6) < 0) { *err = "out of space"; return -1; }
        return k.n;
    }
    if (word_is(s, "detach", &rest)) {
        s = rest;
        a = 0;
        if (!is_end(s) && (!parse_num(&s, &a) || a > 65535)) { *err = "bad detach ms"; return -1; }
        if (!is_end(s)) { *err = "trailing text"; return -1; }
        uint8_t r[4] = { SCRIPT_DETACH, 2, (uint8_t)(a >> 8), (uint8_t)a };
        if (emit(&k, r, 4) < 0) { *err = "out of space"; return -1; }
        return k.n;
    }
    if (word_is(s, "type", &rest)) {
        s = skip_ws(rest);
        uint32_t gap = 0;
        if (*s != '"') {
            if (!parse_num(&s, &gap)) { *err = "type needs [ms] \"text\""; return -1; }
            s = skip_ws(s);
        }
        if (*s != '"') { *err = "type needs a quoted string"; return -1; }
        s++;
        int first = 1;
        while (*s && *s != '"') {
            char ch = *s++;
            if (ch == '\\') {
                if (*s == 'n') ch = '\n';
                else if (*s == 't') ch = '\t';
                else if (*s == 'e') ch = 27;
                else if (*s == '"') ch = '"';
                else if (*s == '\\') ch = '\\';
                else { *err = "bad escape"; return -1; }
                s++;
            }
            int shift;
            uint8_t sc = script_ascii_scancode(ch, &shift);
            if (!sc) { *err = "character not on the scancode table"; return -1; }
            if (gap && !first) {
                st->now_ms += gap;
                if (emit_at(&k, st->now_ms) < 0) { *err = "out of space"; return -1; }
            }
            first = 0;
            if (shift && emit_key(&k, SC_LSHIFT, 1) < 0) { *err = "out of space"; return -1; }
            if (emit_key(&k, sc, 1) < 0 || emit_key(&k, sc, 0) < 0) { *err = "out of space"; return -1; }
            if (shift && emit_key(&k, SC_LSHIFT, 0) < 0) { *err = "out of space"; return -1; }
        }
        if (*s != '"') { *err = "unterminated string"; return -1; }
        if (!is_end(s + 1)) { *err = "trailing text"; return -1; }
        return k.n;
    }
    *err = "unknown directive";
    return -1;
}
