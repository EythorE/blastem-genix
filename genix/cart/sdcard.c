/*
 * sdcard.c - the SD card model.  See sdcard.h.
 */
#include <string.h>
#include "sdcard.h"

#define NCR   4      /* clocks from a command's end bit to the response's start bit */
#define NAC   8      /* clocks from a read command's response to the data start */
#define NWR   2      /* clocks from a write's end bit to the CRC status token */
#define NBUSY 16     /* busy clocks after a write */
#define ACMD41_POLLS 2

/* R1 status bits */
#define ST_OUT_OF_RANGE   (1u << 31)
#define ST_ADDRESS_ERROR  (1u << 30)
#define ST_COM_CRC_ERROR  (1u << 23)
#define ST_ILLEGAL        (1u << 22)
#define ST_READY_FOR_DATA (1u << 8)
#define ST_APP_CMD        (1u << 5)

uint8_t sd_crc7(const uint8_t *bytes, int nbytes)
{
    uint8_t crc = 0;
    for (int i = 0; i < nbytes; i++) {
        uint8_t b = bytes[i];
        for (int k = 0; k < 8; k++) {
            uint8_t bit = (uint8_t)(((crc >> 6) ^ (b >> 7)) & 1);
            crc = (uint8_t)((crc << 1) & 0x7F);
            if (bit)
                crc ^= 0x09;
            b = (uint8_t)(b << 1);
        }
    }
    return crc;
}

uint16_t sd_crc16_bit(uint16_t crc, int bit)
{
    int top = (crc >> 15) & 1;
    crc = (uint16_t)(crc << 1);
    if (top ^ (bit & 1))
        crc ^= 0x1021;
    return crc;
}

void sdcard_init(sdcard_t *c, uint8_t *image, uint32_t size)
{
    memset(c, 0, sizeof *c);
    c->image = image;
    c->size = size;
    c->cmd_level = 1;
    c->dat_level = 0xF;
    c->state = SD_IDLE;
}

static void put_bit(sdcard_t *c, int bit)
{
    if (bit)
        c->out[c->out_bits >> 3] |= (uint8_t)(0x80 >> (c->out_bits & 7));
    else
        c->out[c->out_bits >> 3] &= (uint8_t)~(0x80 >> (c->out_bits & 7));
    c->out_bits++;
}

static void put_bits(sdcard_t *c, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--)
        put_bit(c, (v >> i) & 1);
}

static uint32_t card_status(const sdcard_t *c)
{
    uint32_t st = (uint32_t)(c->state & 0xF) << 9;
    if (c->state == SD_TRAN)
        st |= ST_READY_FOR_DATA;
    if (c->app_cmd)
        st |= ST_APP_CMD;
    return st;
}

/* Start a response: 48 bits (R1/R3/R6/R7) or 136 (R2), MSB first. */
static void begin_response(sdcard_t *c)
{
    c->out_pos = 0;
    c->out_wait = NCR;
}

static void resp_r1(sdcard_t *c, uint8_t idx, uint32_t status)
{
    memset(c->out, 0, sizeof c->out);
    c->out_bits = 0;
    put_bits(c, 0, 2);
    put_bits(c, idx, 6);
    put_bits(c, status, 32);
    put_bits(c, sd_crc7(c->out, 5), 7);
    put_bit(c, 1);
    begin_response(c);
}

static void resp_r3(sdcard_t *c, uint32_t ocr)
{
    memset(c->out, 0, sizeof c->out);
    c->out_bits = 0;
    put_bits(c, 0, 2);
    put_bits(c, 0x3F, 6);
    put_bits(c, ocr, 32);
    put_bits(c, 0x7F, 7);
    put_bit(c, 1);
    begin_response(c);
}

static void resp_r6(sdcard_t *c, uint16_t rca, uint16_t status)
{
    memset(c->out, 0, sizeof c->out);
    c->out_bits = 0;
    put_bits(c, 0, 2);
    put_bits(c, 3, 6);
    put_bits(c, rca, 16);
    put_bits(c, status, 16);
    put_bits(c, sd_crc7(c->out, 5), 7);
    put_bit(c, 1);
    begin_response(c);
}

static void resp_r7(sdcard_t *c, uint32_t arg)
{
    resp_r1(c, 8, arg & 0xFFF);
}

/* R2: 0 0 111111 then 120 bits of register (the CID/CSD's bits
 * 127..8, the register's own CRC7 in bits 7..1 and the end bit). */
static void resp_r2(sdcard_t *c, const uint8_t reg[16])
{
    memset(c->out, 0, sizeof c->out);
    c->out_bits = 0;
    put_bits(c, 0, 2);
    put_bits(c, 0x3F, 6);
    for (int i = 0; i < 15; i++)
        put_bits(c, reg[i], 8);
    put_bits(c, sd_crc7(reg, 15), 7);
    put_bit(c, 1);
    begin_response(c);
}

static void make_cid(uint8_t cid[16])
{
    memset(cid, 0, 16);
    cid[0] = 0x47;                    /* MID: 'G' */
    cid[1] = 'G'; cid[2] = 'X';       /* OID */
    memcpy(cid + 3, "GENIX", 5);      /* PNM */
    cid[8] = 0x10;                    /* PRV 1.0 */
    cid[9] = 0x00; cid[10] = 0x00; cid[11] = 0x26; cid[12] = 0x09;  /* PSN */
    cid[13] = 0x01; cid[14] = 0x89;   /* MDT: 2026-09 */
}

/* CSD version 2.0: C_SIZE in 512 KB units minus one. */
static void make_csd(const sdcard_t *c, uint8_t csd[16])
{
    memset(csd, 0, 16);
    uint32_t csize = c->size / (512u * 1024u);
    if (csize)
        csize--;
    csd[0] = 0x40;                    /* CSD_STRUCTURE 1 (v2) */
    csd[1] = 0x0E;                    /* TAAC */
    csd[3] = 0x32;                    /* TRAN_SPEED 25 MHz */
    csd[4] = 0x5B; csd[5] = 0x59;     /* CCC, READ_BL_LEN 9 */
    csd[7] = (uint8_t)((csize >> 16) & 0x3F);
    csd[8] = (uint8_t)(csize >> 8);
    csd[9] = (uint8_t)csize;
    csd[10] = 0x7F; csd[11] = 0x80;
    csd[12] = 0x0A; csd[13] = 0x40;
}

static void start_data_out(sdcard_t *c)
{
    for (int i = 0; i < 4; i++)
        c->crc[i] = 0;
    c->data_pos = 0;
    c->data_units = c->width4 ? 2 * SD_BLOCK : 8 * SD_BLOCK;
    c->data_phase = 0;
    c->out_wait = NAC;
    c->state = SD_DATA;
}

static void handle_command(sdcard_t *c, uint8_t idx, uint32_t arg)
{
    int app = c->app_cmd;
    c->app_cmd = 0;
    c->n_cmd++;
    c->last_cmd = idx;
    c->last_arg = arg;

    if (app) {
        switch (idx) {
        case 41:
            c->hcs = (arg >> 30) & 1;
            if (c->state == SD_IDLE) {
                if (++c->acmd41_polls >= ACMD41_POLLS) {
                    c->state = SD_READY;
                    c->ccs = c->hcs;
                }
            }
            resp_r3(c, (c->state == SD_IDLE ? 0 : 0x80000000u) | (c->ccs ? 0x40000000u : 0) | 0x00FF8000u);
            return;
        case 6:
            if (c->state != SD_TRAN) break;
            c->width4 = (arg & 3) == 2;
            resp_r1(c, 6, card_status(c) | ST_APP_CMD);
            return;
        case 42:
            if (c->state != SD_TRAN) break;
            resp_r1(c, 42, card_status(c) | ST_APP_CMD);
            return;
        default:
            break;               /* an ordinary command after CMD55 */
        }
    }
    switch (idx) {
    case 0:
        c->state = SD_IDLE;
        c->selected = 0;
        c->width4 = 0;
        c->acmd41_polls = 0;
        c->cmd_drive = 0;
        c->dat_drive = 0;
        return;                  /* no response */
    case 8:
        if (c->state != SD_IDLE) break;
        resp_r7(c, arg);
        return;
    case 55:
        c->app_cmd = 1;
        resp_r1(c, 55, card_status(c) | ST_APP_CMD);
        return;
    case 2:
        if (c->state != SD_READY) break;
        c->state = SD_IDENT;
        { uint8_t cid[16]; make_cid(cid); resp_r2(c, cid); }
        return;
    case 3:
        if (c->state != SD_IDENT && c->state != SD_STBY) break;
        c->rca = 0x0001;
        c->state = SD_STBY;
        resp_r6(c, c->rca, (uint16_t)(((card_status(c) >> 8) & 0xC000) | 0x0000));
        return;
    case 9:
        if (c->state != SD_STBY || (arg >> 16) != c->rca) break;
        { uint8_t csd[16]; make_csd(c, csd); resp_r2(c, csd); }
        return;
    case 7:
        if ((arg >> 16) == c->rca && c->state == SD_STBY) {
            c->state = SD_TRAN;
            c->selected = 1;
            resp_r1(c, 7, card_status(c));
            return;
        }
        if ((arg >> 16) != c->rca && c->state == SD_TRAN) {
            c->state = SD_STBY;   /* deselect: no response */
            c->selected = 0;
            return;
        }
        break;
    case 12:
        if (c->state == SD_DATA || c->state == SD_RCV) {
            c->state = SD_TRAN;
            c->dat_drive = 0;
        }
        c->multi = 0;
        resp_r1(c, 12, card_status(c));
        return;
    case 13:
        resp_r1(c, 13, card_status(c));
        return;
    case 16:
        if (c->state != SD_TRAN) break;
        resp_r1(c, 16, arg == SD_BLOCK ? card_status(c) : card_status(c) | ST_ILLEGAL);
        return;
    case 17:
    case 18:
    case 24: {
        if (c->state != SD_TRAN) break;
        uint32_t byte_addr = c->ccs ? arg * SD_BLOCK : arg;
        if ((byte_addr & (SD_BLOCK - 1)) || byte_addr + SD_BLOCK > c->size) {
            resp_r1(c, idx, card_status(c) | ST_OUT_OF_RANGE);
            return;
        }
        c->lba = byte_addr / SD_BLOCK;
        resp_r1(c, idx, card_status(c));
        if (idx == 17 || idx == 18) {
            c->multi = (idx == 18);
            memcpy(c->blk, c->image + byte_addr, SD_BLOCK);
            c->n_reads++;
            start_data_out(c);
        } else {
            c->state = SD_RCV;
            c->data_phase = 0;
            c->data_pos = 0;
            c->data_units = c->width4 ? 2 * SD_BLOCK : 8 * SD_BLOCK;
            for (int i = 0; i < 4; i++)
                c->crc[i] = c->crc_rx[i] = 0;
        }
        return;
    }
    default:
        break;
    }
    c->n_illegal++;
    resp_r1(c, idx, card_status(c) | ST_ILLEGAL);
}

/* The command line: collect 48 bits after a start bit. */
static void cmd_intake(sdcard_t *c, uint8_t cmd_in)
{
    if (c->cmd_drive)
        return;                  /* the card is talking; the host waits */
    if (c->cmd_bits == 0) {
        if (cmd_in)
            return;              /* idle high */
        c->cmd_shift = 0;
        c->cmd_bits = 1;         /* the start bit */
        return;
    }
    c->cmd_shift = (c->cmd_shift << 1) | (cmd_in & 1);
    c->cmd_bits++;
    if (c->cmd_bits < 48)
        return;
    /* 47 bits collected after the start bit: T(1) idx(6) arg(32) crc(7) end(1) */
    uint64_t f = c->cmd_shift;
    c->cmd_bits = 0;
    int transmission = (int)((f >> 46) & 1);
    uint8_t idx = (uint8_t)((f >> 40) & 0x3F);
    uint32_t arg = (uint32_t)((f >> 8) & 0xFFFFFFFFu);
    uint8_t crc = (uint8_t)((f >> 1) & 0x7F);
    int end = (int)(f & 1);
    if (!transmission || !end)
        return;                  /* not a host command */
    uint8_t bytes[5] = { (uint8_t)(0x40 | idx), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                         (uint8_t)(arg >> 8), (uint8_t)arg };
    if (sd_crc7(bytes, 5) != crc) {
        c->n_crc_err++;
        return;                  /* no response to a bad CRC */
    }
    handle_command(c, idx, arg);
}

/* Drive the next response bit, or release the line. */
static void cmd_output(sdcard_t *c)
{
    if (c->out_bits == 0) {
        c->cmd_drive = 0;
        c->cmd_level = 1;
        return;
    }
    if (c->out_wait > 0) {
        c->out_wait--;
        c->cmd_drive = 0;
        c->cmd_level = 1;
        return;
    }
    if (c->out_pos < c->out_bits) {
        c->cmd_drive = 1;
        c->cmd_level = (uint8_t)((c->out[c->out_pos >> 3] >> (7 - (c->out_pos & 7))) & 1);
        c->out_pos++;
        return;
    }
    c->out_bits = 0;
    c->cmd_drive = 0;
    c->cmd_level = 1;
}

static uint8_t block_nibble(const uint8_t *blk, int n)
{
    uint8_t b = blk[n >> 1];
    return (uint8_t)((n & 1) ? (b & 0xF) : (b >> 4));
}

static int block_bit(const uint8_t *blk, int n)
{
    return (blk[n >> 3] >> (7 - (n & 7))) & 1;
}

/* Data out (after CMD17): start, the block, the CRCs, the end. */
static void dat_output(sdcard_t *c)
{
    if (c->state != SD_DATA && c->state != SD_PRG) {
        c->dat_drive = 0;
        c->dat_level = 0xF;
        return;
    }
    if (c->state == SD_PRG) {
        /* busy: DAT0 low until programming ends */
        c->dat_drive = 1;
        if (c->busy_left > 0) {
            c->busy_left--;
            c->dat_level = 0xE;
        } else {
            c->dat_level = 0xF;
            c->dat_drive = 0;
            c->state = SD_TRAN;
        }
        return;
    }
    if (c->out_wait > 0 && c->data_phase == 0 && c->out_bits == 0) {
        /* Nac after the response has gone out */
        c->out_wait--;
        c->dat_drive = 0;
        c->dat_level = 0xF;
        return;
    }
    if (c->out_bits)             /* the R1 is still going out on CMD: wait */
        { c->dat_drive = 0; c->dat_level = 0xF; return; }
    uint8_t mask = c->width4 ? 0xF : 0x1;
    switch (c->data_phase) {
    case 0:                      /* the start nibble / bit: zeros */
        c->dat_drive = mask;
        c->dat_level = (uint8_t)(0xF & ~mask);
        c->data_phase = 1;
        break;
    case 1: {
        c->dat_drive = mask;
        if (c->width4) {
            uint8_t nib = block_nibble(c->blk, c->data_pos);
            c->dat_level = nib;
            for (int i = 0; i < 4; i++)
                c->crc[i] = sd_crc16_bit(c->crc[i], (nib >> i) & 1);
        } else {
            int bit = block_bit(c->blk, c->data_pos);
            c->dat_level = (uint8_t)(0xE | bit);
            c->crc[0] = sd_crc16_bit(c->crc[0], bit);
        }
        if (++c->data_pos >= c->data_units) {
            c->data_phase = 2;
            c->data_pos = 0;
        }
        break;
    }
    case 2: {                    /* 16 CRC bits per line, MSB first */
        c->dat_drive = mask;
        uint8_t lv = 0;
        for (int i = 0; i < 4; i++)
            lv |= (uint8_t)(((c->crc[i] >> (15 - c->data_pos)) & 1) << i);
        c->dat_level = (uint8_t)(c->width4 ? lv : (0xE | (lv & 1)));
        if (++c->data_pos >= 16)
            c->data_phase = 3;
        break;
    }
    case 3:                      /* the end nibble / bit: ones */
        c->dat_drive = mask;
        c->dat_level = 0xF;
        c->data_phase = 4;
        break;
    default:
        c->dat_drive = 0;
        c->dat_level = 0xF;
        /* CMD18: the next block follows (its own Nac, start, data,
         * CRC, end) until CMD12 or the end of the card. */
        if (c->multi && (uint64_t)(c->lba + 2) * SD_BLOCK <= c->size) {
            c->lba++;
            memcpy(c->blk, c->image + (size_t)c->lba * SD_BLOCK, SD_BLOCK);
            c->n_reads++;
            start_data_out(c);
            break;
        }
        c->multi = 0;
        c->state = SD_TRAN;
        break;
    }
}

/* Data in (after CMD24): the host's start, block, CRCs, end; then the
 * CRC status token and the busy period. */
static void dat_intake(sdcard_t *c, uint8_t dat_in)
{
    if (c->state != SD_RCV)
        return;
    uint8_t mask = c->width4 ? 0xF : 0x1;
    switch (c->data_phase) {
    case 0:                      /* wait for the start nibble / bit */
        if ((dat_in & mask) == 0) {
            c->data_phase = 1;
            c->data_pos = 0;
        }
        break;
    case 1:
        if (c->width4) {
            int n = c->data_pos;
            uint8_t nib = dat_in & 0xF;
            if (n & 1)
                c->blk[n >> 1] = (uint8_t)((c->blk[n >> 1] & 0xF0) | nib);
            else
                c->blk[n >> 1] = (uint8_t)(nib << 4);
            for (int i = 0; i < 4; i++)
                c->crc[i] = sd_crc16_bit(c->crc[i], (nib >> i) & 1);
        } else {
            int n = c->data_pos;
            int bit = dat_in & 1;
            if ((n & 7) == 0)
                c->blk[n >> 3] = 0;
            c->blk[n >> 3] |= (uint8_t)(bit << (7 - (n & 7)));
            c->crc[0] = sd_crc16_bit(c->crc[0], bit);
        }
        if (++c->data_pos >= c->data_units) {
            c->data_phase = 2;
            c->data_pos = 0;
        }
        break;
    case 2:
        for (int i = 0; i < 4; i++)
            c->crc_rx[i] = (uint16_t)((c->crc_rx[i] << 1) | ((dat_in >> i) & 1));
        if (++c->data_pos >= 16)
            c->data_phase = 3;
        break;
    case 3: {                    /* the end nibble; then judge */
        int ok = 1;
        int lines = c->width4 ? 4 : 1;
        for (int i = 0; i < lines; i++)
            if (c->crc_rx[i] != c->crc[i])
                ok = 0;
        c->token_ok = ok;
        if (ok) {
            memcpy(c->image + (uint64_t)c->lba * SD_BLOCK, c->blk, SD_BLOCK);
            c->n_writes++;
        } else {
            c->n_write_crc_err++;
        }
        c->data_phase = 4;
        c->data_pos = 0;
        c->out_wait = NWR;
        break;
    }
    default:
        break;
    }
}

/* The CRC status token on DAT0: 0 s2 s1 s0 1, then busy (0) or not. */
static void token_output(sdcard_t *c)
{
    if (c->state != SD_RCV || c->data_phase != 4)
        return;
    if (c->out_wait > 0) {
        c->out_wait--;
        c->dat_drive = 0;
        c->dat_level = 0xF;
        return;
    }
    static const uint8_t ok_tok[5] = { 0, 0, 1, 0, 1 };
    static const uint8_t bad_tok[5] = { 0, 1, 0, 1, 1 };
    const uint8_t *tok = c->token_ok ? ok_tok : bad_tok;
    c->dat_drive = 1;
    if (c->data_pos < 5) {
        c->dat_level = (uint8_t)(0xE | tok[c->data_pos]);
        c->data_pos++;
        return;
    }
    if (c->token_ok) {
        c->state = SD_PRG;
        c->busy_left = NBUSY;
        c->dat_level = 0xE;
    } else {
        c->state = SD_TRAN;
        c->dat_drive = 0;
        c->dat_level = 0xF;
    }
}

void sdcard_clock(sdcard_t *c, uint8_t cmd_in, uint8_t dat_in)
{
    if (!c->image)
        return;
    /* sample */
    cmd_intake(c, cmd_in);
    dat_intake(c, dat_in);
    /* then the outputs for the next cycle */
    cmd_output(c);
    if (c->state == SD_RCV && c->data_phase == 4)
        token_output(c);
    else
        dat_output(c);
}

uint8_t sdcard_cmd_out(const sdcard_t *c, int *drive)
{
    *drive = c->image ? c->cmd_drive : 0;
    return c->image ? c->cmd_level : 1;
}

uint8_t sdcard_dat_out(const sdcard_t *c, uint8_t *drive_mask)
{
    *drive_mask = c->image ? c->dat_drive : 0;
    return c->image ? c->dat_level : 0xF;
}
