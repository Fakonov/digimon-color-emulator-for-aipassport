#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DMC1: the Web Serial transfer protocol.
 *
 * A published build cannot ship the ROM - it is Bandai's, and 8 MiB does not fit
 * next to a 2 MiB app anyway.  The `roms` partition (data/0x40, 0x280000) holds a
 * sparse page store built by tools/make_sparse_rom.py on the host; this module
 * lets a browser build the same store over the USB-CDC link instead, and moves
 * the save partition in both directions.
 *
 * Line protocol, '\n' terminated, all replies start with "DMC1 ":
 *
 *   DMC1 HELLO                        -> DMC1 READY <rom_ok> <sha1|-> <save_ok>
 *   DMC1 ROM BEGIN <img> <pages> <sha1>
 *                                     -> DMC1 OK BEGIN
 *   DMC1 PAGE <index> <len>           -> <len raw bytes>  -> DMC1 ACK <done>
 *   DMC1 END                          -> DMC1 OK END     (header written, store re-mounted)
 *   DMC1 SAVE GET                     -> DMC1 SAVE <size> <raw bytes> DMC1 OK SAVE
 *   DMC1 SAVE BEGIN <size>            -> DMC1 OK BEGIN
 *   DMC1 PAGE <index> <len>           (same framing; index is a byte offset)
 *   DMC1 END                          -> DMC1 OK END
 *   DMC1 FINISH                       -> DMC1 OK FINISH  (restarts into the game)
 *
 * The browser does the sparsening (it can hold the 8 MiB image, the C3 cannot),
 * so only non-zero pages cross the wire: 611 x 4 KiB instead of 8192 KiB.
 * The guest's own pixel stream is the same cable, so a transfer has to be the
 * only thing talking: ROM BEGIN is refused while the guest is running.
 */

/* Handle one console line.  Returns true when the line belonged to DMC1 (the
 * caller must then not try its own command table). */
bool dmclink_exec(const char *line);

/* True while the engine has no usable ROM: app_main keeps the console alive and
 * paints a hint instead of letting the guest run. */
bool dmclink_waiting_for_rom(void);

/* True while a ROM or save transfer owns the cable.  The guest has to be parked
 * for the duration: a ROM upload reprograms the partition the engine has mapped,
 * so letting the interpreter read through it would execute whatever half-erased
 * page happens to be there. */
bool dmclink_busy(void);

/* One-shot: was the ROM store mounted at boot? */
bool dmclink_rom_ok(void);

/* app_main calls this once the engine has booted (or failed to).  Checking
 * romstore::image() here would be wrong: it is non-null as soon as the partition
 * is found, before the header is validated. */
void dmclink_note_rom_state(bool ok);

#ifdef __cplusplus
}
#endif
