// Enemy motion model (docs/enemies.md 4.1, 4.2), for ranking options.
//
// The game's enemy choices read a random word that P1's own actions also
// stir, so a single run ahead can't say where an enemy will be once P1
// moves. Here the random choices are random: many rollouts of the rules give
// odds (an enemy is killed by this bomb, an enemy comes by this cell), and
// the oracle (the real game) has the last word on each option.
//
//   generic (4.1)  straight on until the cell ahead is blocked (solid, a
//                  bomb), then stop 32 frames, then a random free direction
//   chaser  (4.2)  legs toward P1 along the longer axis; blocked: a detour
//                  of up to 3 cells to a random side
//   $A5            generic at 1 px/f with no rest when blocked (1-7);
//                  it rests while its own bomb is out (see hold); in the
//                  intro (world.intro), or before it ever started (c0 0, no
//                  step or target), it can't be hurt until it first moves
//   $86            generic at 2.5 px/f, at most 4 cells a go, then rests
//                  128 frames; then toward P1 if it sees it (8 cells, no
//                  solid between), else random (C1:664B)
//   $8C            a chaser that halts at a cell centre while it sees P1
//                  (SeesP1 C1:8AF9: same column, up to 4 cells, no solid
//                  tile or bomb between; the same-row test never fires
//                  while both are centred) and pulls P1 in
//
// A bomb placed in the cell an enemy is entering stops it at once (back to
// the centre of its cell). Flames don't block; an enemy whose cell holds
// flames dies.
#pragma once
#include <cstdint>
#include <vector>

#include "world.hpp"

namespace sbm2 {

struct ModelBomb {
  int c = 0, r = 0;
  int placed = 0;  // frame it blocks from
  int boom = 0;    // frame it goes off (real frames, as the model's clock)
  int range = 1;
  bool pierce = false;
};

// Where P1 is from frame t on (chasers head there; $8C halts on sight).
struct P1At { int t, c, r; };

struct EnemyRun {
  // Each rollout's track for one enemy: (cell, from, to) spans.
  struct Span { int16_t c, r; int t0, t1; };
  std::vector<std::vector<Span>> rolls;
  std::vector<int> diedAt;  // per rollout: frame killed, or -1
};

class EnemyModel {
 public:
  // horizon: frames to roll out. p1c/p1r: where chasers head (P1's cover).
  // timeScale: real frames per enemy step frame (the game's enemies run
  // slower than their speed says when it lags; the agent measures it).
  EnemyModel(const World& w, int horizon, uint32_t seed = 1, double timeScale = 1);
  // Roll every enemy that can be hurt (hp > 0, not dormant/hidden) n times
  // with the bomb (if any) in place. Results per enemy, in world.enemies
  // order (an enemy the model skips has no rolls).
  std::vector<EnemyRun> roll(const ModelBomb* bomb, int p1c, int p1r, int n) { return roll(bomb, {{0, p1c, p1r}}, n); }
  std::vector<EnemyRun> roll(const ModelBomb* bomb, const std::vector<P1At>& p1, int n);

  // Odds from a roll(): some enemy dies / one comes by (c,r) (or a cell
  // next to it) in [t0, t1].
  static double killOdds(const std::vector<EnemyRun>& runs);
  static double nearOdds(const std::vector<EnemyRun>& runs, int c, int r, int t0, int t1);

  static bool isChaser(const Enemy& e);
  // A type that chases P1 now or may start to (on sight: +$D0 bit 7).
  static bool mayChase(const Enemy& e);
  // A $91 (x-ray chaser) at a centre with P1 within 4 cells in line (walls
  // block) stops with hp $FF and attacks (docs/enemies.md, C1:BA6B): from
  // (c,r) P1 keeps it stopped and unhurtable.
  static bool x91Sees(const World& w, const Enemy& e, int c, int r);
  static bool modelled(const Enemy& e);
  // Frames a stopped enemy rests before it picks a new way (the generic
  // 32; $A5 turns at once).
  static int stopWait(const Enemy& e);

  // Per enemy (world.enemies order): frames it stays in its cell from now
  // (measured: a run of the real game with P1 holding still), or -1 if not
  // known. A stopped enemy waits this long instead of its wait word: some
  // rest on timers the world view doesn't show ($A5's start, its own bomb).
  std::vector<int> hold;

 private:
  bool freeAt(int c, int r, int t, const ModelBomb* b) const;
  uint32_t rnd();
  void rollOne(const Enemy& e, int hold, const ModelBomb* b, EnemyRun& out);
  P1At p1At(int t) const;
  bool seesP1(int c, int r, int t, const ModelBomb* b) const;
  int sightDir(int c, int r, int t, const ModelBomb* b) const;

  const World& w_;
  int horizon_;
  uint32_t s_;
  double scale_;
  std::vector<uint8_t> blast_;  // cells the bomb's blast reaches
  std::vector<P1At> p1_;
};

}  // namespace sbm2
