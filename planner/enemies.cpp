#include "enemies.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "blast.hpp"
#include "goap.hpp"

namespace sbm2 {

namespace {
constexpr int kStopWait = 32;  // generic: frames stopped before a new direction
constexpr int kDetour = 3;     // chaser: cells of a detour
constexpr int kHunterRun = 4, kHunterRest = 128, kHunterSight = 8;  // $86 (C1:664B)
const int kDir[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};  // up down left right ($000400 & 3)

int dirOf(char step) { return step == 'u' ? 0 : step == 'd' ? 1 : step == 'l' ? 2 : step == 'r' ? 3 : -1; }
}  // namespace

bool EnemyModel::isChaser(const Enemy& e) {
  switch (e.type) {
    case 0x88: case 0x89: case 0x8C: return true;
    case 0x82: case 0x85: case 0x8A: case 0x91: case 0x93: case 0x97: return e.d0 & 0x80;
    default: return false;
  }
}

bool EnemyModel::modelled(const Enemy& e) {
  if (!e.type || (e.type & 0x40) || e.type >= 0xC0 || isThrower(e.type)) return false;
  // ($90/$98 walk as generic ones; +$BE bit 15: a mount, or hidden)
  if ((e.type == 0x90 || e.type == 0x98) && (e.be & 0x8000)) return false;
  return e.hp > 0 && e.hp < 0x7F && !e.dormant() && !e.hidden();
}

int EnemyModel::stopWait(const Enemy& e) { return e.type == 0xA5 ? 0 : e.type == 0x86 ? kHunterRest : kStopWait; }

EnemyModel::EnemyModel(const World& w, int horizon, uint32_t seed, double timeScale)
    : w_(w), horizon_(horizon), s_(seed | 1), scale_(timeScale) {}

uint32_t EnemyModel::rnd() {
  s_ ^= s_ << 13;
  s_ ^= s_ >> 17;
  s_ ^= s_ << 5;
  return s_;
}

// Blocked for an enemy: solid tiles (bit 7: walls, soft blocks, ...) and
// bombs ($43); flames and P1 are not.
bool EnemyModel::freeAt(int c, int r, int t, const ModelBomb* b) const {
  if (!w_.inside(c, r)) return false;
  char k = w_.at(c, r);
  if (!walkableClass(k) || k == 'b' || (k == 'g' && !w_.gateOpen)) return false;
  if (w_.bombAt(c, r)) return false;
  if (b && c == b->c && r == b->r && t >= b->placed) return false;
  return true;
}

P1At EnemyModel::p1At(int t) const {
  P1At at = p1_.front();
  for (const P1At& q : p1_)
    if (q.t <= t) at = q;
  return at;
}

// $86: P1 in a straight line within 8 cells, nothing solid between: the
// way to it, else -1.
int EnemyModel::sightDir(int c, int r, int t, const ModelBomb* b) const {
  P1At p = p1At(t);
  if (p.c != c && p.r != r) return -1;
  if (std::abs(p.c - c) + std::abs(p.r - r) > kHunterSight || (p.c == c && p.r == r)) return -1;
  int d = p.c > c ? 3 : p.c < c ? 2 : p.r > r ? 1 : 0;
  for (int cc = c + kDir[d][0], rr = r + kDir[d][1]; cc != p.c || rr != p.r; cc += kDir[d][0], rr += kDir[d][1])
    if (!freeAt(cc, rr, t, b)) return -1;
  return d;
}

bool EnemyModel::seesP1(int c, int r, int t, const ModelBomb* b) const {
  P1At p = p1At(t);
  if (p.c != c || std::abs(p.r - r) > 4) return false;
  for (int k = std::min(p.r, r) + 1; k < std::max(p.r, r); k++)
    if (!freeAt(c, k, t, b)) return false;
  return true;
}

void EnemyModel::rollOne(const Enemy& e, int hold, const ModelBomb* b, EnemyRun& out) {
  std::vector<EnemyRun::Span> sp;
  double speed = std::max(std::fabs(e.vx), std::fabs(e.vy));
  if (e.spd >= 0.25) speed = e.spd;
  if (speed < 0.25) speed = 0.5;
  const int T = std::max(4, int(16.0 / speed * scale_ + 0.5));  // frames per cell
  const int kStop = int(stopWait(e) * scale_ + 0.5);
  const bool chaser = isChaser(e), halts = e.type == 0x8C;
  // $86: at most 4 cells a go, then a 128-frame rest; then toward P1 if it
  // sees P1, else a random way.
  const bool hunter = e.type == 0x86;
  int run = 0;
  int halted = 0;
  int c = e.col(), r = e.row();
  int t = 0, died = -1;
  // Unhurt until its measured rest ends: in a mini-boss intro, or not
  // started yet (no step, no target: 1-7's $A5 runs no code, so no flame
  // check, for its first ~206 frames).
  const int t3 = e.type & 0x3F;
  const bool asleep = (w_.intro && t3 >= 0x24 && t3 <= 0x28) || (e.c0 == 0 && e.step == '-' && e.tc == 0 && e.tr == 0);
  const int safeTo = asleep ? std::max(0, hold) : -1;
  auto kills = [&](int cc, int rr, int t0, int t1) {
    if (!b || died >= 0) return;
    t0 = std::max(t0, safeTo);
    if (!blast_[size_t(rr * w_.cols + cc)]) return;
    int f0 = std::max(t0, b->boom), f1 = std::min(t1, b->boom + int((kFlameFrames - 4) * scale_));
    if (f0 <= f1) died = f0;
  };
  auto stay = [&](int cc, int rr, int t0, int t1) {
    if (t1 < t0) return;
    sp.push_back({int16_t(cc), int16_t(rr), t0, t1});
    kills(cc, rr, t0, t1);
  };
  // Moving from (c,r) to (nc,nr), arriving at t + left: in the old cell for
  // the first half. A bomb dropped in the new cell before it gets there
  // stops it in the old one.
  auto move = [&](int nc, int nr, int left) -> bool {
    if (b && nc == b->c && nr == b->r && b->placed > t && b->placed < t + left) {
      stay(c, r, t, b->placed);
      t = b->placed;
      return false;
    }
    int mid = t + left / 2;
    stay(c, r, t, mid);
    stay(nc, nr, mid + 1, t + left);
    c = nc, r = nr, t += left;
    return true;
  };

  int dir = dirOf(e.step);
  bool moving = (e.c0 & 0x8000) && dir >= 0 && (e.tc != c || e.tr != r);
  int wait = moving ? 0 : int(std::max(0, e.wait) * scale_);
  // (measured: it leaves its cell at `hold`, half a cell's walk after it sets off)
  if (!moving && hold >= 0) wait = std::max(0, hold - T / 2);
  if (moving) {
    int left = int((std::abs(e.x - (e.tc * 16 + 8)) + std::abs(e.y - (e.tr * 16 + 7))) / speed * scale_ + 0.5);
    if (!move(e.tc, e.tr, std::max(1, left))) moving = false, wait = kStop;
  }
  int detour = 0, detourAxis = 0;
  while (t < horizon_ && died < 0) {
    if (!moving) {
      // Stopped: wait, then a direction (generic: random each frame until a
      // free one comes up; chaser: plan a leg).
      if (wait > 0) {
        stay(c, r, t, t + wait - 1);
        t += wait;
        wait = 0;
        continue;
      }
      int nd = -1;
      if (chaser) {
        P1At p = p1At(t);
        int dx = p.c - c, dy = p.r - r;
        int want = -1;
        if (halts && halted < 320 && seesP1(c, r, t, b)) {  // (stands and pulls)
          stay(c, r, t, t + 7);
          t += 8, halted += 8;
          continue;
        }
        if (detour > 0) {
          int a = rnd() & 1;
          int perp[2] = {detourAxis ? 0 : 2, detourAxis ? 1 : 3};
          for (int k = 0; k < 2 && want < 0; k++) {
            int d = perp[(a + k) & 1];
            if (freeAt(c + kDir[d][0], r + kDir[d][1], t, b)) want = d;
          }
          detour--;
        } else if (dx || dy) {
          want = std::abs(dx) >= std::abs(dy) && dx ? (dx > 0 ? 3 : 2) : (dy > 0 ? 1 : 0);
          if (!freeAt(c + kDir[want][0], r + kDir[want][1], t, b)) {
            detour = kDetour;
            detourAxis = want >= 2;  // 1: it was moving along x
            want = -1;
            continue;
          }
        }
        nd = want;
        if (nd < 0) {  // nowhere it wants: any free side
          int a = int(rnd() & 3);
          for (int k = 0; k < 4 && nd < 0; k++)
            if (freeAt(c + kDir[(a + k) & 3][0], r + kDir[(a + k) & 3][1], t, b)) nd = (a + k) & 3;
        }
        if (nd < 0) {
          stay(c, r, t, t + 7);
          t += 8;
          continue;
        }
      } else if (hunter && (nd = sightDir(c, r, t, b)) >= 0 && freeAt(c + kDir[nd][0], r + kDir[nd][1], t, b)) {
      } else {
        nd = -1;
        int tries = 0;
        for (;;) {
          int d = int(rnd() & 3);
          if (freeAt(c + kDir[d][0], r + kDir[d][1], t, b)) {
            nd = d;
            break;
          }
          if (++tries > 64) break;
          stay(c, r, t, t);
          t++;
        }
        if (nd < 0) continue;
      }
      dir = nd;
      run = 0;
      if (move(c + kDir[dir][0], r + kDir[dir][1], T)) moving = true, run = 1;
      else wait = kStop;
      continue;
    }
    // At a cell centre, moving: straight on if free (a chaser re-plans
    // when P1's band is reached or it is blocked).
    int nc = c + kDir[dir][0], nr = r + kDir[dir][1];
    bool on = freeAt(nc, nr, t, b);
    if (halts && halted < 320 && seesP1(c, r, t, b)) on = false;
    if (chaser && on) {
      P1At p = p1At(t);
      int dx = p.c - c, dy = p.r - r;
      if (dir >= 2 ? dx == 0 || (dx > 0) != (dir == 3) : dy == 0 || (dy > 0) != (dir == 1)) on = false;
    }
    if (hunter && run >= kHunterRun) on = false;
    if (!on) {
      moving = false;
      wait = chaser ? 0 : kStop;
      continue;
    }
    if (!move(nc, nr, T)) moving = false, wait = kStop;
    else run++;
  }
  out.rolls.push_back(std::move(sp));
  out.diedAt.push_back(died);
}

std::vector<EnemyRun> EnemyModel::roll(const ModelBomb* bomb, const std::vector<P1At>& p1, int n) {
  p1_ = p1.empty() ? std::vector<P1At>{{0, w_.p.col(), w_.p.row()}} : p1;
  blast_.assign(size_t(w_.cols * w_.rows), 0);
  if (bomb)
    for (const Cell& x : blastCells(w_, bomb->c, bomb->r, bomb->range, bomb->pierce)) blast_[size_t(x.r * w_.cols + x.c)] = 1;
  std::vector<EnemyRun> out(w_.enemies.size());
  for (size_t i = 0; i < w_.enemies.size(); i++) {
    const Enemy& e = w_.enemies[i];
    if (!modelled(e) || !w_.inside(e.col(), e.row())) continue;
    for (int k = 0; k < n; k++) rollOne(e, i < hold.size() ? hold[i] : -1, bomb, out[i]);
  }
  return out;
}

double EnemyModel::killOdds(const std::vector<EnemyRun>& runs) {
  // P(some enemy dies), the enemies taken as independent.
  double none = 1;
  for (const EnemyRun& r : runs) {
    if (r.diedAt.empty()) continue;
    int k = 0;
    for (int d : r.diedAt) k += d >= 0;
    none *= 1.0 - double(k) / double(r.diedAt.size());
  }
  return 1 - none;
}

double EnemyModel::nearOdds(const std::vector<EnemyRun>& runs, int c, int r, int t0, int t1) {
  double none = 1;
  for (const EnemyRun& er : runs) {
    if (er.rolls.empty()) continue;
    int k = 0;
    for (size_t i = 0; i < er.rolls.size(); i++) {
      int dead = er.diedAt[i];
      for (const auto& s : er.rolls[i]) {
        if (dead >= 0 && s.t0 > dead) break;
        if (s.t1 < t0 || s.t0 > t1) continue;
        if (std::abs(s.c - c) + std::abs(s.r - r) <= 1) {
          k++;
          break;
        }
      }
    }
    none *= 1.0 - double(k) / double(er.rolls.size());
  }
  return 1 - none;
}

}  // namespace sbm2
