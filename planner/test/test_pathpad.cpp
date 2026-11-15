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
