/*
 * genix_cart.c - the Genix production cartridge mapper.  See genix_cart.h.
 *
 * The memory map: the lower slot (0x000000-0x1FFFFF) is a pointer
 * chunk at the flash image while RAM_ON is clear and at PSRAM bank L
 * after; the upper slot (0x200000-0x3FFFFF) is PSRAM bank U.  Reads
 * and code go through the pointer; writes go through functions that
 * write PSRAM (and invalidate the 68000 core's code cache) or, on the
 * flash, do nothing (the JEDEC sequences are not modelled: a flash
 * write is counted, plan sec 8.4 item 9 is where they would matter).
 * The /TIME window 0xA13000-0xA130FF is the engine's; time for the
 * pulse train is the 68000 core's cycle count (master clock units,
 * seven per VCLK).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "genix_cart.h"
#include "../util.h"
#include "../m68k_core.h"
#include "cart/sdcard.h"
#include "cart/sdengine.h"

#ifndef GENIX_CART_SHA
#define GENIX_CART_SHA "unknown"
#endif

#define SLOT        0x200000u
#define PSRAM_BANKS 4
#define MCLKS_PER_VCLK 7

char *genix_cart_spec;

typedef struct {
    sdcard_t card;
    sdengine_t eng;
    uint8_t *image;
    uint32_t image_size;
    uint8_t *psram;              /* PSRAM_BANKS * SLOT, host word order */
    uint8_t *flash;              /* the ROM buffer BlastEm loaded (byteswapped later) */
    uint32_t flash_mask;
    uint8_t dip;
    uint16_t ptr_lower, ptr_upper;
    genesis_context *gen;
    char *dump_path;
    uint32_t dump_bytes;
    uint32_t trace_left;
    uint32_t n_flash_writes;
} cart_t;

static cart_t *cart;

static void trace(cart_t *c, const char *what, uint32_t addr, uint32_t value, uint32_t cycle)
{
    if (!c->trace_left)
        return;
    c->trace_left--;
    fprintf(stderr, "genix cart: %10u %s %06x %04x | ctrl %02x bank %02x busy %d\n",
            cycle, what, addr, value, c->eng.ctrl, c->eng.bank,
            cycle < c->eng.busy_until);
}

int genix_cart_wanted(uint8_t *rom, uint32_t rom_size)
{
    if (genix_cart_spec)
        return 1;
    if (rom_size < 0x200)
        return 0;
    /* the domestic name at 0x120 and the overseas name at 0x150 */
    for (uint32_t off = 0x120; off + 10 <= 0x180; off++)
        if (!memcmp(rom + off, "GENIX CART", 10))
            return 1;
    return 0;
}

static void dump_at_exit(void)
{
    cart_t *c = cart;
    if (!c || !c->dump_path)
        return;
    FILE *f = fopen(c->dump_path, "wb");
    if (!f) {
        fprintf(stderr, "genix cart: cannot write %s\n", c->dump_path);
        return;
    }
    /* the 68000 core keeps words in host order: put them back big-endian */
    for (uint32_t i = 0; i < c->dump_bytes; i += 2) {
        uint16_t w = *(uint16_t *)(c->psram + i);
        fputc(w >> 8, f);
        fputc(w & 0xFF, f);
    }
    fclose(f);
    fprintf(stderr, "genix cart: dumped %u bytes of PSRAM bank 0 to %s (trains %u, pulses %u, lost %u, stale %u, card cmds %u, crc errs %u, reads %u, writes %u, flash writes %u)\n",
            c->dump_bytes, c->dump_path, c->eng.n_trains, c->eng.n_pulses,
            c->eng.n_lost_requests, c->eng.n_stale_reads, c->card.n_cmd,
            c->card.n_crc_err, c->card.n_reads, c->card.n_writes, c->n_flash_writes);
}

static void parse_spec(cart_t *c)
{
    char *spec = genix_cart_spec ? strdup(genix_cart_spec) : strdup("none");
    char *source = spec, *opts = strchr(spec, ',');
    if (opts)
        *opts++ = 0;
    while (opts && *opts) {
        char *next = strchr(opts, ',');
        if (next)
            *next++ = 0;
        if (!strncmp(opts, "dip=", 4)) {
            c->dip = (uint8_t)(atoi(opts + 4) & 1);
        } else if (!strncmp(opts, "dump=", 5)) {
            char *colon = strchr(opts + 5, ':');
            if (colon) {
                *colon = 0;
                c->dump_bytes = (uint32_t)strtoul(colon + 1, NULL, 0);
            }
            c->dump_path = strdup(opts + 5);
        } else if (!strncmp(opts, "trace=", 6)) {
            c->trace_left = (uint32_t)atoi(opts + 6);
        } else {
            fatal_error("genix cart: unknown option '%s'\n", opts);
        }
        opts = next;
    }
    if (strcmp(source, "none") != 0 && *source) {
        FILE *f = fopen(source, "rb");
        if (!f)
            fatal_error("genix cart: cannot open card image %s\n", source);
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0 || (n % SD_BLOCK))
            fatal_error("genix cart: %s is not a whole number of 512-byte sectors\n", source);
        c->image = malloc((size_t)n);
        if (fread(c->image, 1, (size_t)n, f) != (size_t)n)
            fatal_error("genix cart: short read on %s\n", source);
        fclose(f);
        c->image_size = (uint32_t)n;
    }
    if (c->dump_bytes == 0)
        c->dump_bytes = 8192;
    if (c->dump_bytes & 1)
        c->dump_bytes++;
    fprintf(stderr, "genix cart: model %s flash %u KB dip %u card %s (%u KB)%s%s\n",
            GENIX_CART_SHA, (c->flash_mask + 1) / 1024, c->dip,
            c->image ? source : "none", c->image_size / 1024,
            c->dump_path ? " dump " : "", c->dump_path ? c->dump_path : "");
    fflush(stderr);
    free(spec);
}

/* ---- the slots ---- */

static void *lower_ptr(cart_t *c)
{
    if (sdengine_ram_on(&c->eng))
        return c->psram + (uint32_t)sdengine_bank_l(&c->eng) * SLOT;
    uint32_t off = (c->dip && c->flash_mask >= SLOT) ? SLOT : 0;
    return c->flash + off;
}

static void *upper_ptr(cart_t *c)
{
    return c->psram + (uint32_t)sdengine_bank_u(&c->eng) * SLOT;
}

static void remap(cart_t *c, m68k_context *context)
{
    void *lo = lower_ptr(c), *up = upper_ptr(c);
    if (context->mem_pointers[c->ptr_lower] != lo) {
        m68k_invalidate_code_range(context, 0, SLOT);
        context->mem_pointers[c->ptr_lower] = lo;
    }
    if (context->mem_pointers[c->ptr_upper] != up) {
        m68k_invalidate_code_range(context, SLOT, 2 * SLOT);
        context->mem_pointers[c->ptr_upper] = up;
    }
}

static void *write_lower_w(uint32_t address, void *vcontext, uint16_t value)
{
    m68k_context *context = vcontext;
    cart_t *c = cart;
    address &= SLOT - 1;
    if (sdengine_ram_on(&c->eng)) {
        uint8_t *bank = c->psram + (uint32_t)sdengine_bank_l(&c->eng) * SLOT;
        *(uint16_t *)(bank + address) = value;
        m68k_handle_code_write(address, context);
    } else {
        c->n_flash_writes++;
    }
    return context;
}

static void *write_lower_b(uint32_t address, void *vcontext, uint8_t value)
{
    m68k_context *context = vcontext;
    cart_t *c = cart;
    address &= SLOT - 1;
    if (sdengine_ram_on(&c->eng)) {
        uint8_t *bank = c->psram + (uint32_t)sdengine_bank_l(&c->eng) * SLOT;
        bank[address ^ 1] = value;
        m68k_handle_code_write(address & ~1u, context);
    } else {
        c->n_flash_writes++;
    }
    return context;
}

static void *write_upper_w(uint32_t address, void *vcontext, uint16_t value)
{
    m68k_context *context = vcontext;
    cart_t *c = cart;
    address &= SLOT - 1;
    uint8_t *bank = c->psram + (uint32_t)sdengine_bank_u(&c->eng) * SLOT;
    *(uint16_t *)(bank + address) = value;
    m68k_handle_code_write(SLOT + address, context);
    return context;
}

static void *write_upper_b(uint32_t address, void *vcontext, uint8_t value)
{
    m68k_context *context = vcontext;
    cart_t *c = cart;
    address &= SLOT - 1;
    uint8_t *bank = c->psram + (uint32_t)sdengine_bank_u(&c->eng) * SLOT;
    bank[address ^ 1] = value;
    m68k_handle_code_write(SLOT + (address & ~1u), context);
    return context;
}

/* ---- the /TIME window ---- */

static uint16_t time_read_w(uint32_t address, void *vcontext)
{
    m68k_context *context = vcontext;
    cart_t *c = cart;
    uint16_t v = sdengine_read(&c->eng, (uint8_t)address, context->cycles);
    trace(c, "rd.w", 0xA13000 + (address & 0xFF), v, context->cycles);
    return v;
}

static uint8_t time_read_b(uint32_t address, void *vcontext)
{
    m68k_context *context = vcontext;
    cart_t *c = cart;
    uint8_t v = sdengine_read_byte(&c->eng, (uint8_t)address, context->cycles);
    trace(c, "rd.b", 0xA13000 + (address & 0xFF), v, context->cycles);
    return v;
}

static void *time_write_w(uint32_t address, void *vcontext, uint16_t value)
{
    m68k_context *context = vcontext;
    cart_t *c = cart;
    trace(c, "wr.w", 0xA13000 + (address & 0xFF), value, context->cycles);
    sdengine_write(&c->eng, (uint8_t)address, value, context->cycles);
    remap(c, context);
    return context;
}

static void *time_write_b(uint32_t address, void *vcontext, uint8_t value)
{
    m68k_context *context = vcontext;
    cart_t *c = cart;
    trace(c, "wr.b", 0xA13000 + (address & 0xFF), value, context->cycles);
    sdengine_write_byte(&c->eng, (uint8_t)address, value, context->cycles);
    remap(c, context);
    return context;
}

/* ---- configuration ---- */

rom_info genix_cart_configure_rom(uint8_t *rom, uint32_t rom_size,
                                  memmap_chunk const *base_map, uint32_t base_chunks)
{
    rom_info info;
    memset(&info, 0, sizeof info);
    cart_t *c = calloc(1, sizeof *c);
    cart = c;
    c->flash = rom;
    uint32_t p2 = 1;
    while (p2 < rom_size)
        p2 <<= 1;
    if (p2 > 2 * SLOT)
        p2 = 2 * SLOT;
    c->flash_mask = p2 - 1;
    c->psram = calloc(PSRAM_BANKS, SLOT);
    parse_spec(c);
    sdcard_init(&c->card, c->image, c->image_size);
    sdengine_init(&c->eng, &c->card, c->dip, MCLKS_PER_VCLK);
    if (c->dump_path)
        atexit(dump_at_exit);

    info.name = strdup("Genix production cart");
    info.rom = rom;
    info.rom_size = rom_size;
    info.regions = 0;
    info.mapper_type = MAPPER_GENIX_CART;
    info.mapper_start_index = 0;
    c->ptr_lower = 0;
    c->ptr_upper = 1;
    info.map_chunks = base_chunks + 3;
    info.map = calloc(info.map_chunks, sizeof(memmap_chunk));
    memmap_chunk *m = info.map;
    m[0].start = 0;
    m[0].end = SLOT;
    m[0].mask = c->flash_mask < SLOT - 1 ? c->flash_mask : SLOT - 1;
    m[0].flags = MMAP_READ | MMAP_PTR_IDX | MMAP_CODE;
    m[0].ptr_index = c->ptr_lower;
    m[0].buffer = c->flash;
    m[0].write_16 = write_lower_w;
    m[0].write_8 = write_lower_b;
    m[1].start = SLOT;
    m[1].end = 2 * SLOT;
    m[1].mask = SLOT - 1;
    m[1].flags = MMAP_READ | MMAP_PTR_IDX | MMAP_CODE;
    m[1].ptr_index = c->ptr_upper;
    m[1].buffer = c->psram;
    m[1].write_16 = write_upper_w;
    m[1].write_8 = write_upper_b;
    m[2].start = 0xA13000;
    m[2].end = 0xA13100;
    m[2].mask = 0xFF;
    m[2].read_16 = time_read_w;
    m[2].read_8 = time_read_b;
    m[2].write_16 = time_write_w;
    m[2].write_8 = time_write_b;
    memcpy(m + 3, base_map, base_chunks * sizeof(memmap_chunk));
    return info;
}

void genix_cart_reset(genesis_context *gen)
{
    cart_t *c = cart;
    if (!c)
        return;
    c->gen = gen;
    sdengine_reset(&c->eng);
    /* the lower-slot chunk's mask was the flash's: with RAM_ON the
     * slot is a full 2 MB bank, so the mask must be the slot's.  The
     * flash image smaller than 2 MB then wraps as BlastEm does. */
    remap(c, gen->m68k);
}

void genix_cart_adjust_cycles(genesis_context *gen, uint32_t deduction)
{
    (void)gen;
    cart_t *c = cart;
    if (!c)
        return;
    if (c->eng.busy_until >= deduction)
        c->eng.busy_until -= deduction;
    else
        c->eng.busy_until = 0;
}
