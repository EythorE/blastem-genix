/*
 * genix_cart.h - the Genix production cartridge as a BlastEm mapper.
 *
 * genix/hardware/production-cart/design.md secs 4 to 6 (the register
 * authority), plan genix/docs/plans/production-cart.md sec 8.  Two
 * 2 MB slots over a 4 MB flash image and 8 MB of PSRAM in four banks,
 * the CTRL/BANK/DOUT/STATUS/DATA/SRM_ON registers in the /TIME window,
 * and the SD engine (genix/cart/, a checksummed copy of the Genix
 * tree's hardware/production-cart/model/) with a card image behind
 * it.
 *
 * Selected when the ROM's header name contains "GENIX CART" (the cart
 * flavour's crt0 puts it there), or by `-C <spec>` on the command
 * line for ROMs without a header (the probe ROM):
 *
 *   -C card.img                the card image (mkcard.py output)
 *   -C none                    no card in the socket
 *   ...,dip=1                  the DIP switch: flash image 1
 *   ...,dump=<path>[:<bytes>]  at exit, write PSRAM bank 0's first
 *                              <bytes> (default 8192) to <path>,
 *                              big-endian: the ladder's window
 *   ...,trace=<n>              log the first n register accesses
 *   ...,save                   at exit, write the card image (with
 *                              the CMD24 writes) back to its file
 *   ...,save=<path>            ... to <path> instead
 *   ...,eject=<frames>         pull the card at that frame count (the
 *                              -b count): card detect reads no card,
 *                              nothing answers, a transfer in flight
 *                              stops - the ",none" path from then on
 */
#ifndef GENIX_CART_H_
#define GENIX_CART_H_

#include <stdint.h>
#include "../romdb.h"
#include "../genesis.h"

extern char *genix_cart_spec;

int  genix_cart_wanted(uint8_t *rom, uint32_t rom_size);
rom_info genix_cart_configure_rom(uint8_t *rom, uint32_t rom_size,
                                  memmap_chunk const *base_map, uint32_t base_chunks);
void genix_cart_reset(genesis_context *gen);
void genix_cart_adjust_cycles(genesis_context *gen, uint32_t deduction);
void genix_cart_frame(genesis_context *gen, uint32_t elapsed);   /* at each frame end */

#endif
