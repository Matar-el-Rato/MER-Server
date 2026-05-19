/* =============================================================
 * parchis_logic.c — pure Parchís movement rules (no I/O, no globals).
 * ============================================================= */

#include "parchis_logic.h"
#include <stdlib.h>
#include <time.h>

/* slot 0=blue, 1=green, 2=yellow, 3=red */
const int PARCHIS_EXIT[MAX_ROOM_PLAYERS]      = {22, 56,  5, 39};
const int PARCHIS_ENTRY[MAX_ROOM_PLAYERS]     = {17, 51, 68, 34};
const int PARCHIS_CORR_BASE[MAX_ROOM_PLAYERS] = {111, 131, 101, 121};
const int PARCHIS_GOAL[MAX_ROOM_PLAYERS]      = {118, 138, 108, 128};

/* Universal safe squares — 9 total (corridor entrances + sq 1). */
const int PARCHIS_SAFE[9] = {1, 12, 17, 29, 34, 46, 51, 63, 68};

/* Golden square pools: one per quadrant, 12 candidates each.
 * Excludes safe squares and the 2 trapezoid squares per quadrant (8,9 / 25,26 / 42,43 / 59,60). */
const int PARCHIS_GOLDEN_POOL[4][12] = {
    { 1, 2, 3, 4, 6, 7, 10, 11, 13, 14, 15, 16 },   /* yellow quadrant */
    {18,19,20,21,23,24, 27, 28, 30, 31, 32, 33 },   /* blue quadrant   */
    {35,36,37,38,40,41, 44, 45, 47, 48, 49, 50 },   /* red quadrant    */
    {52,53,54,55,57,58, 61, 62, 64, 65, 66, 67 },   /* green quadrant  */
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

bool parchis_is_goal(int sq)
{
    return sq == 108 || sq == 118 || sq == 128 || sq == 138;
}

/* Advance one step on the ring (wraps 68→1). */
static int ring_step(int pos) { return pos == 68 ? 1 : pos + 1; }

/* Advance one step in the corridor (stays within corrBase..goal). */
static int corr_step(int pos) { return pos + 1; }

int parchis_advance(int slot, int from_sq, int steps)
{
    if (from_sq == 0) return -1; /* home — caller handles exit separately */

    int entry    = PARCHIS_ENTRY[slot];
    int corrBase = PARCHIS_CORR_BASE[slot];

    if (from_sq >= 100) {
        /* Already in corridor; step = from_sq - corrBase + 1 (1-indexed) */
        int corrStep = from_sq - corrBase + 1;
        int newStep  = corrStep + steps;
        if (newStep > 8) return -1;  /* overshoot past goal */
        return corrBase + newStep - 1;
    }

    /* On main ring — compute steps to reach the entry square. */
    int stepsToEntry;
    if (entry >= from_sq) stepsToEntry = entry - from_sq;
    else                  stepsToEntry = (68 - from_sq) + entry;

    if (steps <= stepsToEntry) {
        /* Stays on ring */
        int to = from_sq + steps;
        if (to > 68) to -= 68;
        return to;
    } else {
        /* Enters corridor */
        int remaining = steps - stepsToEntry;
        if (remaining > 8) return -1;  /* overshoot past goal */
        return corrBase + remaining - 1;
    }
}

bool parchis_is_safe(int sq, int mover_slot)
{
    for (int i = 0; i < 9; i++)
        if (PARCHIS_SAFE[i] == sq) return true;
    return sq == PARCHIS_EXIT[mover_slot];
}

bool parchis_is_barrier(int sq, int blocker_slot, int positions[][4])
{
    int count = 0;
    for (int p = 0; p < 4; p++)
        if (positions[blocker_slot][p] == sq) count++;
    return count >= 2;
}

bool parchis_path_clear(int mover_slot, int from_sq, int steps, int positions[][4])
{
    if (steps <= 1) return true; /* no intermediate squares */

    int entry    = PARCHIS_ENTRY[mover_slot];
    int corrBase = PARCHIS_CORR_BASE[mover_slot];
    int pos      = from_sq;

    /* Walk each intermediate step (skip the final destination). */
    for (int i = 0; i < steps - 1; i++) {
        /* Advance pos by one step */
        if (pos >= 100) {
            pos = corr_step(pos);
        } else if (pos == entry) {
            pos = corrBase;
        } else {
            pos = ring_step(pos);
        }

        /* Check for an enemy barrier at this intermediate square. */
        for (int s = 0; s < MAX_ROOM_PLAYERS; s++) {
            if (s == mover_slot) continue;
            if (parchis_is_barrier(pos, s, positions)) return false;
        }
    }
    return true;
}

/* True if mover can land on sq (not an enemy barrier). */
static bool can_land(int sq, int mover_slot, int positions[][4])
{
    if (sq == 0) return false; /* home is never a landing target */
    for (int s = 0; s < MAX_ROOM_PLAYERS; s++) {
        if (s == mover_slot) continue;
        if (parchis_is_barrier(sq, s, positions)) return false;
    }
    return true;
}

int parchis_moveable_pieces(int slot, int die1, int die2,
                             int positions[][4],
                             int *out_pieces, bool *out_can_exit)
{
    int count    = 0;
    int total    = die1 + die2;
    bool can_exit = (die1 == 5 || die2 == 5 || total == 5);
    if (out_can_exit) *out_can_exit = can_exit;

    for (int p = 0; p < 4; p++) {
        int from = positions[slot][p];

        if (from == 0) {
            /* Piece in home — can only exit with a 5. */
            if (!can_exit) continue;
            int exit_sq = PARCHIS_EXIT[slot];
            if (!can_land(exit_sq, slot, positions)) continue;
            out_pieces[count++] = p;
        } else {
            /* Piece on ring or in corridor — move by total. */
            int to = parchis_advance(slot, from, total);
            if (to < 0) continue;
            if (!parchis_path_clear(slot, from, total, positions)) continue;
            if (!can_land(to, slot, positions)) continue;
            out_pieces[count++] = p;
        }
    }
    return count;
}

void parchis_random_golden_squares(int out_squares[4])
{
    static int seeded = 0;
    if (!seeded) { srand((unsigned int)time(NULL)); seeded = 1; }

    for (int q = 0; q < 4; q++)
        out_squares[q] = PARCHIS_GOLDEN_POOL[q][rand() % 12];
}
