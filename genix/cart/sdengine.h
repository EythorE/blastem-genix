/*
 * sdengine.h - the cart's registers, shifter and pulse train
 * (hardware/production-cart/design.md secs 5 and 6), host-buildable.
 *
 * The console side of the SD engine: CTRL, BANK, DOUT, STATUS, DATA
 * and SRM_ON exactly as design.md sec 5 has them (A1 and A4 decoded,
 * so the /TIME window aliases), the four 74HC595 receive chains, the
 * pulse train of N clocks that follows a DATA access, and BUSY held
 * for the train's length so a DATA access issued before the pulses
 * finish returns stale data and loses its request (sec 6.1).  The
 * card is sdcard.c behind it.
 *
 * Time is a counter the caller owns: `cycle` in units of the slot's
 * VCLK (7.67 MHz, one SD_CLK pulse each) scaled by cycles_per_vclk
 * (1 on the host, 7 in BlastEm's master-clock cycles).  The train's
 * length in VCLKs is N + TRAIN_EXTRA_VCLK (sec 6.1's sequence: a
 * train of 2 ends ~450 ns after the access, 4 ~700 ns, 8 ~1.2 us);
 * TRAIN_EXTRA_VCLK is the parameter the breadboard's scope re-tunes
 * (production-cart.md sec 8.5 item 4).
 *
 * The mapper (RAM_ON, the bank fields) is the BlastEm model's: this
 * engine only keeps the register values.
 */
#ifndef SDENGINE_H
#define SDENGINE_H

#include <stdint.h>
#include "sdcard.h"

#define CTRL_LED      0x01
#define CTRL_RAM_ON   0x02
#define CTRL_M0       0x04
#define CTRL_M1       0x08
#define CTRL_CMD_DIR  0x10
#define CTRL_DAT_DIR  0x20
#define CTRL_CLK      0x40

#define STATUS_DAT_MASK 0x0F
#define STATUS_CMD      0x10
#define STATUS_BUSY     0x20
#define STATUS_CD       0x40   /* 0 = card present */
#define STATUS_DIP      0x80

#define DOUT_DAT_MASK 0x0F
#define DOUT_CMD      0x10

#define BANK_L(bank)  ((bank) & 3)
#define BANK_U(bank)  (((bank) >> 2) & 3)

#ifndef TRAIN_EXTRA_VCLK
#define TRAIN_EXTRA_VCLK 2
#endif

typedef struct {
    sdcard_t *card;
    uint8_t ctrl, bank, dout, srm_on, dip;

    uint8_t  chain[4];        /* per DAT line, the four stages: bit 0 newest */
    uint16_t latch;           /* the 595 output registers: what DATA reads */
    uint16_t next_latch;      /* what RCLK will copy when the train ends */
    uint64_t busy_until;      /* cycle the train in flight ends */
    int      train_pending;   /* next_latch is waiting for busy_until */
    uint32_t cycles_per_vclk;

    /* live lines as the STATUS buffer sees them */
    uint8_t dat_lines, cmd_line;

    /* diagnostics */
    uint32_t n_trains, n_pulses, n_lost_requests, n_stale_reads, n_manual_clocks;
} sdengine_t;

void sdengine_init(sdengine_t *e, sdcard_t *card, uint8_t dip, uint32_t cycles_per_vclk);
/* Console reset: CTRL, BANK, SRM_ON, the request and run flops, the counter. */
void sdengine_reset(sdengine_t *e);

/* The /TIME window.  addr: the address's low byte (bits 1 and 4 are
 * decoded).  Word access: both halves; byte access: uwr for the even
 * address (D15-8), lwr for the odd (D7-0); a byte write to 0xA130E0
 * loads BANK, to 0xA130E1 loads CTRL, sec 5. */
uint16_t sdengine_read(sdengine_t *e, uint8_t addr, uint64_t cycle);
uint8_t  sdengine_read_byte(sdengine_t *e, uint8_t addr, uint64_t cycle);
void     sdengine_write(sdengine_t *e, uint8_t addr, uint16_t value, uint64_t cycle);
void     sdengine_write_byte(sdengine_t *e, uint8_t addr, uint8_t value, uint64_t cycle);

/* STATUS as a byte (also what a word read at E0 returns in its low byte). */
uint8_t  sdengine_status(sdengine_t *e, uint64_t cycle);

/* Register values for the mapper. */
static inline int sdengine_ram_on(const sdengine_t *e) { return (e->ctrl & CTRL_RAM_ON) != 0; }
static inline int sdengine_bank_l(const sdengine_t *e) { return BANK_L(e->bank); }
static inline int sdengine_bank_u(const sdengine_t *e)
{
    int u = BANK_U(e->bank);
    if (e->srm_on)
        u &= 1;               /* sec 4 U1G: the top bit forced to 0 */
    return u;
}
/* The pulse count the mode bits select. */
static inline int sdengine_train_len(const sdengine_t *e)
{
    switch ((e->ctrl >> 2) & 3) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 4;
    default: return 8;
    }
}

#endif
