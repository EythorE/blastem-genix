/*
 * sdcard.h - an SD card in native mode, at the clock level.
 *
 * The card side of hardware/production-cart/design.md sec 6: CMD
 * (bidirectional), DAT0-3 (bidirectional), one call per SD_CLK
 * rising edge.  Host-buildable (tests/test_sdcard.c) and compiled
 * into the BlastEm cart model (blastem-genix genix/, a checksummed
 * copy like the dongle's link/).  No allocation, no globals, no
 * timing beyond clock counts.
 *
 * What it does: CMD0, CMD8, CMD55 + ACMD41 (ready after a few
 * polls, CCS from HCS), CMD2 (CID), CMD3 (RCA 0x0001), CMD7, CMD9
 * (CSD v2 for the image size), CMD12, CMD13, CMD16, ACMD6 (bus
 * width), ACMD42, CMD17 (single-block read), CMD24 (single-block
 * write).  CRC7 is checked on every command (a bad one gets no
 * response), CRC16 per line on writes (a bad one gets the 101
 * token and no write).  1-bit and 4-bit data.  The busy period
 * after a write.  Card detect and the no-card case (nothing ever
 * answers).
 *
 * Clocking model: at each rising edge the card SAMPLES its inputs
 * (the levels the caller passes) and then updates the outputs it
 * will drive until the next edge (a real card launches after the
 * falling edge, the sampler catches it at the next rising edge:
 * design.md sec 6.4).  Read the outputs with sdcard_cmd_out /
 * sdcard_dat_out BEFORE the next sdcard_clock.
 */
#ifndef SDCARD_H
#define SDCARD_H

#include <stdint.h>

#define SD_BLOCK 512

enum sd_state { SD_IDLE, SD_READY, SD_IDENT, SD_STBY, SD_TRAN, SD_DATA, SD_RCV, SD_PRG };

typedef struct {
    uint8_t *image;          /* the whole card, NULL = no card */
    uint32_t size;           /* bytes, a multiple of 512 */

    /* command intake */
    uint64_t cmd_shift;
    int cmd_bits;            /* bits collected since the start bit; 0 = idle */

    /* response and data transmit */
    uint8_t out[20];         /* the response bits, packed MSB first */
    int out_bits, out_pos;
    int out_wait;            /* Ncr / Nac clocks before the first bit */
    uint8_t cmd_level, cmd_drive;
    uint8_t dat_level, dat_drive;   /* dat_level: 4 bits; dat_drive: mask */

    /* card state */
    int state;
    int app_cmd;
    int hcs, ccs;
    int acmd41_polls;
    uint16_t rca;
    int width4;
    int selected;

    /* block transfer */
    uint8_t blk[SD_BLOCK];
    uint32_t lba;
    int data_pos;            /* nibbles (4-bit) or bits (1-bit) sent or received */
    int data_units;          /* 1024 nibbles or 4096 bits */
    int data_phase;          /* 0 start, 1 data, 2 crc, 3 end, 4 token, 5 busy */
    uint16_t crc[4];
    uint16_t crc_rx[4];
    int busy_left;
    int token_ok;

    /* diagnostics */
    uint32_t n_cmd, n_crc_err, n_illegal, n_reads, n_writes, n_write_crc_err;
    uint8_t last_cmd;
    uint32_t last_arg;
} sdcard_t;

/* image NULL = no card in the socket.  size in bytes. */
void sdcard_init(sdcard_t *c, uint8_t *image, uint32_t size);
static inline int sdcard_present(const sdcard_t *c) { return c->image != 0; }

/* One SD_CLK rising edge.  cmd_in: the CMD level the card sees (1 when
 * nobody drives); dat_in: DAT3..DAT0 likewise (0xF when nobody
 * drives). */
void sdcard_clock(sdcard_t *c, uint8_t cmd_in, uint8_t dat_in);

/* The card's outputs until the next edge; *drive says whether it
 * drives the line(s) (else the pull-up wins). */
uint8_t sdcard_cmd_out(const sdcard_t *c, int *drive);
uint8_t sdcard_dat_out(const sdcard_t *c, uint8_t *drive_mask);

/* The CRCs the SD bus uses, exposed for the host driver and the tests. */
uint8_t  sd_crc7(const uint8_t *bytes, int nbytes);         /* over 8*nbytes bits, MSB first */
uint16_t sd_crc16_bit(uint16_t crc, int bit);               /* one bit into a CRC16 (0x1021) */

#endif
