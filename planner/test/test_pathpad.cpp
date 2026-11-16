// The oracle's path controller (port/path_pad.h) on a fake WRAM.
#include <vector>

#include "path_pad.h"
#include "test.hpp"

namespace {
struct Ram {
  std::vector<uint8_t> w = std::vector<uint8_t>(0x20000, 0);
  void p1(int x, int y, int lock) {
    w[0x8010] = uint8_t(x), w[0x8011] = uint8_t(x >> 8);
    w[0x8018] = uint8_t(y), w[0x8019] = uint8_t(y >> 8);
    w[0x8052] = uint8_t(lock), w[0x8053] = uint8_t(lock >> 8);
  }
};
}  // namespace

// Thrown by a spring pad ($1801 in the air): nothing held, so P1 lands 3
// cells on; the path is not over (botrun's sim_pad goes on to the bomb
// press only with every waypoint used: 3-2 pressed A in the air).
TEST("path_pad: let go in a jump's flight, the path not over") {
  Ram r;
  std::vector<int16_t> wp{24, 24, 24, 72};  // (1,1) then (1,4), jumped
  int at = 1, mem[3] = {0, 0, -1};
  r.p1(24, 40, 0x1801);
  CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 2, &at, mem), 0);
  CHECK(at < 2);
  r.p1(24, 40, 0);
  CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 2, &at, mem), PP_DOWN);
}

// Held by a pad ($0801): P1 can't move; the way held when it lets go is
// the jump. Aim at the next waypoint, not the pad's centre a pixel or two
// off, and never swap axes for want of progress (3-2's (13,7) pad: 12
// frames still, the swap held Down and it jumped down).
TEST("path_pad: held by a pad, hold the jump's way") {
  Ram r;
  std::vector<int16_t> wp{88, 120, 216, 120, 280, 120};  // pad (13,7), then (17,7)
  int at = 1, mem[3] = {0, 0, -1};
  for (int f = 0; f < 30; f++) {
    r.p1(216, 122, 0x0801);
    CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 3, &at, mem), PP_RIGHT);
  }
  CHECK_EQ(at, 2);
  // Not held, the same spot lines up first (the pad's centre).
  Ram q;
  int at2 = 1, mem2[3] = {0, 0, -1};
  q.p1(216, 122, 0);
  CHECK_EQ(path_pad_w(q.w.data(), wp.data(), 3, &at2, mem2), PP_UP);
}

// A held jump (the landing waypoint carries PP_HOLD): keep the way in the air.
TEST("path_pad: a held jump keeps the way through the flight") {
  Ram r;
  std::vector<int16_t> wp{40, 88, int16_t(40 | PP_HOLD), 24};  // pad (2,5), held to (2,1)
  int at = 1, mem[3] = {0, 0, -1};
  for (int f = 0; f < 30; f++) {
    r.p1(40, 70 - f, 0x1801);
    CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 2, &at, mem), PP_UP);
  }
}

// A held sideways jump climbs before it comes down: no closer for a while,
// which is no reason to try the other axis (that would steer the flight).
TEST("path_pad: a held jump's arc is no stall") {
  Ram r;
  std::vector<int16_t> wp{40, 88, int16_t(104 | PP_HOLD), 88};  // pad (2,5), held right to (6,5)
  int at = 1, mem[3] = {0, 0, -1};
  for (int f = 0; f < 30; f++) {
    r.p1(40, 88 - f, 0x1801);
    CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 2, &at, mem), PP_RIGHT);
  }
}

// Riding (P1 +2 bit 15): press nothing (a press steps off) and skip to the
// waypoint the ride lets P1 off at; none ahead: the path is over.
TEST("path_pad: riding presses nothing and skips to the ride's end") {
  Ram r;
  std::vector<int16_t> wp{24, 24, 72, 24, int16_t(136 | PP_RIDE), 24, 152, 24};
  int at = 2, mem[3] = {0, 0, -1};
  r.p1(80, 40, 0);
  r.w[0x8003] = 0x80;
  CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 4, &at, mem), 0);
  CHECK_EQ(at, 2);
  r.w[0x8003] = 0;
  r.p1(136, 24, 0);
  CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 4, &at, mem), PP_RIGHT);
  CHECK_EQ(at, 3);
  // Thrown by a jump pad object: the bit is set but P1 is locked; not a
  // ride, the path goes on (3-2: ended mid-throw, pressed A in the air).
  int at3 = 1;
  r.p1(80, 40, 0x0801);
  r.w[0x8003] = 0x80;
  path_pad_w(r.w.data(), wp.data(), 4, &at3, nullptr);
  CHECK(at3 < 2);
  std::vector<int16_t> probe{24, 24, 72, 24};  // (a probe: no end known)
  r.p1(80, 40, 0);
  int at2 = 1;
  r.w[0x8003] = 0x80;
  CHECK_EQ(path_pad_w(r.w.data(), probe.data(), 2, &at2, nullptr), 0);
  CHECK_EQ(at2, 2);
}

// A $0801 lock on a spring pad is the pad's, not a death (the death checks
// in botrun's sims and bot/agent.lua; a long jump read as one in 3-1).
TEST("path_pad: p1_on_pad") {
  Ram r;
  r.p1(12 * 16 + 8, 8 * 16 + 8, 0x0801);
  CHECK(!p1_on_pad(r.w.data()));
  r.w[0x1C000 + 8 * 0x80 + 12 * 2] = 0x4C;
  CHECK(p1_on_pad(r.w.data()));
}

// In a jump's flight, passing a waypoint is no arrival (3-6: the arc from
// (5,24) crossed the (5,21) pad's centre; the held jump after it started
// early and carried P1 on into the water).
TEST("path_pad: a waypoint passed in the air is not reached") {
  Ram r;
  std::vector<int16_t> wp{88, 392, 88, 344, int16_t(88 | PP_HOLD), 264};  // pads (5,24), (5,21), held to (5,16)
  int at = 1, mem[3] = {0, 0, -1};
  r.p1(88, 345, 0x1801);
  CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 3, &at, mem), 0);  // (let go: the first jump lands on (5,21))
  CHECK_EQ(at, 1);
  r.p1(88, 344, 0x0801);  // (held by the (5,21) pad: the next jump, Up)
  CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 3, &at, mem), PP_UP);
  CHECK_EQ(at, 2);
}

// A platform lane's far end (PP_LANE): held toward it by the lane (the
// platform boards when it waits there) and riding it (bit 15: never skip
// ahead and let go, the platform shuttles back with P1), and no swapping
// axes for want of progress (it steps off the near end: 4-2).
TEST("path_pad: a lane ride holds toward its far end, waiting and riding") {
  Ram r;
  std::vector<int16_t> wp{56, 152, 200, 152, int16_t(264 | PP_LANE), 152};  // (3,9), (12,9) L, (16,9) L
  int at = 1, mem[3] = {0, 0, -1};
  for (int f = 0; f < 60; f++) {
    r.p1(200, 151, 0);  // (by the lane, a pixel off its line, waiting)
    CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 3, &at, mem), PP_RIGHT);
  }
  CHECK_EQ(at, 2);
  r.w[0x8003] = 0x80;  // riding
  for (int x = 216; x < 248; x++) {
    r.p1(x, 152, 0);
    CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 3, &at, mem), PP_RIGHT);
  }
  r.p1(220, 152, 0);  // (carried back)
  CHECK_EQ(path_pad_w(r.w.data(), wp.data(), 3, &at, mem), PP_RIGHT);
  CHECK_EQ(at, 2);
  // Off at the far end, the bit still set: the path goes on (4-2: stood
  // on (16,9) for 4000 frames).
  std::vector<int16_t> on{200, 152, int16_t(264 | PP_LANE), 152, 264, 136};
  int at2 = 1;
  r.p1(264, 152, 0);
  CHECK_EQ(path_pad_w(r.w.data(), on.data(), 3, &at2, mem), PP_UP);
}
