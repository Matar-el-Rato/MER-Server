#ifndef PARCHIS_LOGIC_H
#define PARCHIS_LOGIC_H

#include "protocol.h"  /* MAX_ROOM_PLAYERS */
#include <stdbool.h>

/* ── Board constants ─────────────────────────────────────────────────────────
 * Slot order: 0=blue, 1=green, 2=yellow, 3=red  (matches SLOT_COLORS[]).
 */

/* Home exit square (piece lands here on first 5). */
extern const int PARCHIS_EXIT[MAX_ROOM_PLAYERS];

/* Last ring square before the home corridor begins. */
extern const int PARCHIS_ENTRY[MAX_ROOM_PLAYERS];

/* First corridor square (step 1 of 8). Goal = base + 7. */
extern const int PARCHIS_CORR_BASE[MAX_ROOM_PLAYERS];

/* Goal square (step 8, piece is finished). */
extern const int PARCHIS_GOAL[MAX_ROOM_PLAYERS];

/* Universal safe squares — cannot be captured here (9 squares). */
extern const int PARCHIS_SAFE[9];

/* Golden-square candidate pools, one per quadrant [0..3] (yellow/blue/red/green quadrants).
 * Pool sizes are 14 each (range of 16 minus 2 excluded squares). */
extern const int PARCHIS_GOLDEN_POOL[4][14];

/* ── Position encoding ───────────────────────────────────────────────────────
 *  0          = house (waiting)
 *  1–68       = main ring
 *  101–108    = yellow corridor HY1–GY
 *  111–118    = blue   corridor HB1–GB
 *  121–128    = red    corridor HR1–GR
 *  131–138    = green  corridor HG1–GG
 */

/* True if sq is a goal square for any color. */
bool parchis_is_goal(int sq);

/* Advance piece at from_sq by steps.
 * Returns destination square, or -1 if the move is illegal (overshoot). */
int parchis_advance(int slot, int from_sq, int steps);

/* True if sq is safe for mover_slot (universal safe OR own exit square). */
bool parchis_is_safe(int sq, int mover_slot);

/* True if blocker_slot has 2+ pieces at sq (barrier). */
bool parchis_is_barrier(int sq, int blocker_slot, int positions[][4]);

/* True if no enemy barrier exists on the intermediate squares between
 * from_sq and to_sq (exclusive of from_sq and to_sq). */
bool parchis_path_clear(int mover_slot, int from_sq, int steps, int positions[][4]);

/* Fill out_pieces[4] with piece indices that can legally be moved.
 * Returns count. Sets *out_can_exit if any home piece can exit.
 * Checks home-exit eligibility, advancement validity, path clarity, and landing. */
int parchis_moveable_pieces(int slot, int die1, int die2,
                             int positions[][4],
                             int *out_pieces, bool *out_can_exit);

/* Randomise one golden square per quadrant using the candidate pools.
 * Writes 4 values into out_squares[0..3]. */
void parchis_random_golden_squares(int out_squares[4]);

#endif /* PARCHIS_LOGIC_H */
