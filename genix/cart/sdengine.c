/*
 * sdengine.c - the cart's SD engine.  See sdengine.h.
 */
#include "sdengine.h"

void sdengine_init(sdengine_t *e, sdcard_t *card, uint8_t dip, uint32_t cycles_per_vclk)
{
    e->card = card;
    e->dip = dip ? 1 : 0;
    e->cycles_per_vclk = cycles_per_vclk ? cycles_per_vclk : 1;
    e->latch = e->next_latch = 0;
    e->n_trains = e->n_pulses = e->n_lost_requests = e->n_stale_reads = e->n_manual_clocks = 0;
    sdengine_reset(e);
}

void sdengine_reset(sdengine_t *e)
{
    e->ctrl = 0;
    e->bank = 0;
    e->dout = 0;
    e->srm_on = 0;
    e->busy_until = 0;
    e->train_pending = 0;
    for (int i = 0; i < 4; i++)
        e->chain[i] = 0;
    e->dat_lines = 0xF;
    e->cmd_line = 1;
}

/* The lines as the STATUS buffer and the card see them right now. */
static void resolve_lines(sdengine_t *e, uint8_t *dat, uint8_t *cmd)
{
    uint8_t card_dat_drive = 0, card_dat = 0xF, card_cmd = 1;
    int card_cmd_drive = 0;
    if (e->card) {
        card_dat = sdcard_dat_out(e->card, &card_dat_drive);
        card_cmd = sdcard_cmd_out(e->card, &card_cmd_drive);
    }
    uint8_t d = 0xF;                                   /* the pull-ups */
    if (e->ctrl & CTRL_DAT_DIR)
        d = e->dout & DOUT_DAT_MASK;                   /* the cart drives */
    /* the card's drive wins on the lines it drives (a fight on the
     * real board; the software rule is not to) */
    d = (uint8_t)((d & ~card_dat_drive) | (card_dat & card_dat_drive));
    uint8_t c = 1;
    if (e->ctrl & CTRL_CMD_DIR)
        c = (e->dout & DOUT_CMD) ? 1 : 0;
    if (card_cmd_drive)
        c = card_cmd;
    *dat = d;
    *cmd = c;
}

static uint16_t chain_word(const sdengine_t *e)
{
    uint16_t w = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t h = e->chain[i];
        w |= (uint16_t)(((h >> 0) & 1) << i);
        w |= (uint16_t)(((h >> 1) & 1) << (4 + i));
        w |= (uint16_t)(((h >> 2) & 1) << (8 + i));
        w |= (uint16_t)(((h >> 3) & 1) << (12 + i));
    }
    return w;
}

/* One SD_CLK rising edge: the 595s and the card sample the lines,
 * then the card moves on. */
static void pulse(sdengine_t *e)
{
    uint8_t dat, cmd;
    resolve_lines(e, &dat, &cmd);
    for (int i = 0; i < 4; i++)
        e->chain[i] = (uint8_t)(((e->chain[i] << 1) | ((dat >> i) & 1)) & 0xF);
    if (e->card)
        sdcard_clock(e->card, cmd, dat);
    e->n_pulses++;
}

/* Complete a train whose end has passed: RCLK copies the chains. */
static void settle(sdengine_t *e, uint64_t cycle)
{
    if (e->train_pending && cycle >= e->busy_until) {
        e->latch = e->next_latch;
        e->train_pending = 0;
    }
}

/* A DATA access requests a train of N pulses (sec 6.1).  Lost when
 * one is already running. */
static void request_train(sdengine_t *e, uint64_t cycle)
{
    settle(e, cycle);
    if (cycle < e->busy_until) {
        e->n_lost_requests++;
        return;
    }
    int n = sdengine_train_len(e);
    if (!(e->ctrl & CTRL_CLK)) {          /* with CLK held high the pulses are invisible */
        for (int i = 0; i < n; i++)
            pulse(e);
    }
    e->next_latch = chain_word(e);
    e->train_pending = 1;
    e->busy_until = cycle + (uint64_t)(n + TRAIN_EXTRA_VCLK) * e->cycles_per_vclk;
    e->n_trains++;
}

uint8_t sdengine_status(sdengine_t *e, uint64_t cycle)
{
    uint8_t dat, cmd;
    settle(e, cycle);
    resolve_lines(e, &dat, &cmd);
    uint8_t s = (uint8_t)(dat & STATUS_DAT_MASK);
    if (cmd)
        s |= STATUS_CMD;
    if (cycle < e->busy_until)
        s |= STATUS_BUSY;
    if (!(e->card && sdcard_present(e->card)))
        s |= STATUS_CD;
    if (e->dip)
        s |= STATUS_DIP;
    return s;
}

uint16_t sdengine_read(sdengine_t *e, uint8_t addr, uint64_t cycle)
{
    settle(e, cycle);
    if (addr & 0x10)
        return 0xFFFF;                     /* A4 = 1: nothing decoded on reads */
    if (!(addr & 0x02))
        return (uint16_t)(0xFF00 | sdengine_status(e, cycle));   /* E0: STATUS, high byte open */
    /* E2: DATA - the latch, then a request */
    uint16_t v = e->latch;
    if (cycle < e->busy_until)
        e->n_stale_reads++;
    request_train(e, cycle);
    return v;
}

uint8_t sdengine_read_byte(sdengine_t *e, uint8_t addr, uint64_t cycle)
{
    uint16_t w = sdengine_read(e, (uint8_t)(addr & ~1), cycle);
    return (addr & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

static void write_ctrl(sdengine_t *e, uint8_t v)
{
    uint8_t old = e->ctrl;
    e->ctrl = v;
    if (!(old & CTRL_CLK) && (v & CTRL_CLK)) {
        /* a manual rising edge: one pulse for the card and the chains.
         * RCLK does not fire (no train ran), so the 595 output
         * register, the latch DATA reads, is untouched until the next
         * train's end copies the chains. */
        pulse(e);
        e->n_manual_clocks++;
    }
}

void sdengine_write(sdengine_t *e, uint8_t addr, uint16_t value, uint64_t cycle)
{
    settle(e, cycle);
    if (addr & 0x10) {
        if (!(addr & 0x02))
            e->srm_on = value & 1;         /* F0/F1: SRM_ON = D0 */
        return;                            /* F2: nothing */
    }
    if (!(addr & 0x02)) {                  /* E0: CTRL + BANK */
        e->bank = (uint8_t)(value >> 8);
        write_ctrl(e, (uint8_t)value);
        return;
    }
    /* E2: DOUT, and a request */
    e->dout = (uint8_t)value;
    request_train(e, cycle);
}

void sdengine_write_byte(sdengine_t *e, uint8_t addr, uint8_t value, uint64_t cycle)
{
    settle(e, cycle);
    if (addr & 0x10) {
        if (!(addr & 0x02))
            e->srm_on = value & 1;         /* F0/F1: WR_N accepts either lane */
        return;
    }
    if (!(addr & 0x02)) {
        /* Both 273s share CLK_CTRL. The 68000 duplicates a written
         * byte onto both bus halves, regardless of the selected lane.
         * Software must use word writes from the CTRL/BANK shadows. */
        e->bank = value;
        write_ctrl(e, value);
        return;
    }
    e->dout = value;                       /* E2/E3: DOUT */
    request_train(e, cycle);
}
