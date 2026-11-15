/* P1's closed-loop path controller, shared by port/botrun.c (the oracle's
 * runs) and the planner's unit tests. Reads P1 from WRAM only. */
#ifndef SBM2_PATH_PAD_H
#define SBM2_PATH_PAD_H
#include <stdint.h>
#include <stdlib.h>

enum { PP_UP = 0x0800, PP_DOWN = 0x0400, PP_LEFT = 0x0200, PP_RIGHT = 0x0100 };
/* A waypoint's x may carry PP_HOLD (planner kJumpHold): the jump's flight
 * there keeps the way held (it lands 4 cells on, not 3). */
enum { PP_HOLD = 0x4000 };
/* PP_RIDE (planner kRideEnd): where a ride lets P1 off. Riding (P1 +2 bit
 * 15, docs/mechanics.md 5b) nothing is pressed (a press steps off) and the
 * path skips to that waypoint; with none ahead the path is over (the ride
 * goes where it goes: the agent learns where). */
enum { PP_RIDE = 0x2000 };
#define PP_X(wp, i) ((wp)[2 * (i)] & ~(PP_HOLD | PP_RIDE))

/* P1 on a spring pad ($4C at its cell): a $0801 lock there is the pad's,
 * not a death (docs/mechanics.md 5). */
static inline int p1_on_pad(const uint8_t *wram)
{
    int x = (uint16_t)((wram[0x8010] | wram[0x8011] << 8) + (wram[0x5F01] | wram[0x5F02] << 8));
    int y = (uint16_t)((wram[0x8018] | wram[0x8019] << 8) + (wram[0x5EFB] | wram[0x5EFC] << 8));
    int c = x / 16, r = y / 16;
    return c >= 0 && c < 32 && r >= 0 && r < 32 && wram[0x1C000 + r * 0x80 + c * 2] == 0x4C;
}

/* The path controller (the same rule as bot.lua's pathPad; waypoints are
 * world pixels, cell * 16 + 8, so a scrolling stage doesn't move them): skip
 * waypoints reached (within 1 px), then press toward the next one along
 * the axis with the larger distance (Down/Up on a tie); 0 once at the end.
 * No closer to the waypoint for 12 frames: within 4 px it counts as
 * reached, else the other axis for a while. (1-2, off the stairs: P1 rests
 * at y 86 in (6,5), centre (104,88); Down slides it left onto the stairs,
 * Right brings it back, forever. Up, to the next waypoint, goes.) */
static inline uint16_t path_pad_w(const uint8_t *wram, const int16_t *wp, int n, int *at, int *mem)
{
    /* world pixels: P1's position plus the scroll origin ($5F01, $5EFB) */
    int x = (uint16_t)((wram[0x8010] | wram[0x8011] << 8) + (wram[0x5F01] | wram[0x5F02] << 8));
    int y = (uint16_t)((wram[0x8018] | wram[0x8019] << 8) + (wram[0x5EFB] | wram[0x5EFC] << 8));
    /* The start cell's centre is only there to line P1 up for its turn: if
     * the first leg runs along the axis P1 is already lined up on, go
     * straight (back to the centre first is a step the wrong way when P1
     * sits off it toward the next cell: 2-8, a drop dodge walked up into
     * the blast's row). */
    if (*at == 0 && n >= 2 && ((PP_X(wp, 1) == PP_X(wp, 0) && abs(x - PP_X(wp, 0)) <= 1 && wp[3] != wp[1]) ||
                               (wp[3] == wp[1] && abs(y - wp[1]) <= 1 && PP_X(wp, 1) != PP_X(wp, 0))))
        *at = 1;
    int lock = wram[0x8052] | wram[0x8053] << 8;
    int air = (lock & 0xF801) == 0x1801;
    /* (in a jump's flight, passing a waypoint is no arrival: 3-6, the arc
     * from (5,24) crossed the (5,21) pad's centre, the next jump's hold
     * started early and carried P1 on into the water) */
    while (!air && *at < n && abs(PP_X(wp, *at) - x) <= 1 && abs(wp[2 * *at + 1] - y) <= 1) (*at)++;
    if (*at >= n) return 0;
    /* (a jump pad object's throw sets the bit too, but locks P1: not a ride) */
    if ((wram[0x8003] & 0x80) && !(wram[0x8052] & 1)) {
        int k = *at;
        while (k < n && !(wp[2 * k] & PP_RIDE)) k++;
        *at = k;
        if (mem) mem[1] = 0, mem[2] = -1;
        return 0;
    }
    /* thrown by a spring pad ($52 = $1801 in the air): let go, so it lands
     * 3 cells on (docs/mechanics.md 5), unless the waypoint says hold (4) */
    if (air && !(wp[2 * *at] & PP_HOLD)) return 0;
    /* held by a pad ($0801): P1 can't move, and the way held when it lets
     * go is the jump: aim past waypoints in P1's own cell (3-2: landed on
     * the (13,7) pad 2 px below its centre, Down to line up was the jump) */
    int held = (lock & 0xF801) == 0x0801;
    if (held)
        while (*at + 1 < n && PP_X(wp, *at) / 16 == x / 16 && wp[2 * *at + 1] / 16 == y / 16) (*at)++;
    int dx = PP_X(wp, *at) - x, dy = wp[2 * *at + 1] - y;
    if (mem) {  /* mem: best distance, frames since it got better, waypoint index */
        int d = abs(dx) + abs(dy);
        /* (held, no progress is no sign of being stuck: the other axis
         * there would be the jump, 3-2's (13,7) pad went Down) */
        if (mem[2] != *at || d < mem[0] || held || air) mem[0] = d, mem[1] = 0, mem[2] = *at;
        else mem[1]++;
        if (mem[1] >= 12 && abs(dx) <= 4 && abs(dy) <= 4 && *at + 1 < n) {
            (*at)++;
            dx = PP_X(wp, *at) - x, dy = wp[2 * *at + 1] - y;
            mem[0] = abs(dx) + abs(dy), mem[1] = 0, mem[2] = *at;
        }
    }
    int horiz = abs(dx) > abs(dy);
    if (mem && mem[1] >= 12 && (horiz ? dy : dx) != 0 && (mem[1] / 12) % 2 == 1) horiz = !horiz;
    if (horiz) return dx > 0 ? PP_RIGHT : PP_LEFT;
    return dy > 0 ? PP_DOWN : PP_UP;
}

#endif
