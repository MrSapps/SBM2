// Bomb blasts: which cells a blast reaches, chain reactions, and the frames
// each cell is deadly (docs/mechanics.md; ported from bot.lua blastCells /
// blastWindows, with the cell classes of docs/worldview.md).
#pragma once
#include <vector>

#include "world.hpp"

namespace sbm2 {

// Timing (frames). FLAME is an estimate carried over from bot.lua: TODO pin
// it from the asm (FlameCleanup C0:E14F) and a probe.
constexpr int kFlameFrames = 40;
constexpr int kBlastLead = 4;      // a cell is deadly from (explode - 4)
constexpr int kNever = 1000000;    // remote / frozen bombs: until set off

struct Cell {
  int c, r;
  bool operator==(const Cell& o) const { return c == o.c && r == o.r; }
};

// Cells a blast from (c,r) of the given range reaches. Walls stop it before
// the cell; soft blocks, closed gates, push blocks, items ('i'), stairs and
// '$4B' traps are reached and stop it (pierce goes on through soft blocks,
// items and '$4B' traps). through: items don't stop it (P1 picks them up on
// the way out, then the blast goes on). A barrel sets off 5 range-2 pierce
// bursts (the barrel cell and the cells 2 away).
std::vector<Cell> blastCells(const World& w, int c, int r, int range, bool pierce, bool through = true);

struct Window {
  int t0, t1;  // deadly from t0 to t1 inclusive, frames from now
};

// Deadly windows per cell, for all bombs and flames in the world, with chain
// reactions (a bomb reached by an earlier blast goes off with it).
class DangerMap {
 public:
  explicit DangerMap(const World& w);
  // Add a bomb P1 would place at (c,r) going off in fuse frames (or earlier
  // if a blast reaches it); bombs it reaches go off with it.
  void addBomb(int c, int r, int range, bool pierce, int fuse);
  bool deadly(int c, int r, int t0, int t1) const;  // any window overlaps [t0,t1]
  const std::vector<Window>& at(int c, int r) const;
  // Frame each bomb goes off (index matches world.bombs, then added bombs).
  const std::vector<int>& times() const { return times_; }

 private:
  struct B {
    int c, r, range;
    bool pierce;
    int t;
  };
  void rebuild();
  const World& w_;
  std::vector<B> bombs_;
  std::vector<int> times_;
  std::vector<std::vector<Window>> win_;
};

}  // namespace sbm2
