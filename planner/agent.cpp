#include "agent.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <queue>

#include "agent_api.h"
#include "blast.hpp"
#include "capture.hpp"
#include "enemies.hpp"

namespace sbm2 {

namespace {

constexpr int kWalkFPT = 22;     // frames per cell budgeted for a walk (P1 ~16-20)
constexpr int kNewFuse = 150;    // a fresh bomb's fuse (bot.lua NEW_FUSE)
constexpr int kTail = 60;        // frames past the commit the oracle must also live
constexpr int kSettle = 20;
constexpr int kRideProbe = 360;  // frames a ride may take (3-5's subs: ~170-250)
const int kDirs[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};

int rd16(const uint8_t* w, int a) { return w[a] | w[a + 1] << 8; }

// Walking steps along waypoints (cell centres): a step to a cell further
// than the next one is a jump or a ride (kSpringFrames, ~4 steps; 3-1:
// a 40-frame walk over a pad ended in the air, by the $98 it landed on).
bool hasJump(const std::vector<int16_t>& wp);
int wpSteps(const std::vector<int16_t>& wp) {
  int n = 0;
  for (size_t i = 2; i + 1 < wp.size(); i += 2) {
    int d = std::abs((wp[i] & ~kWpFlags) - (wp[i - 2] & ~kWpFlags)) / 16 + std::abs(wp[i + 1] - wp[i - 1]) / 16;
    n += d > 1 ? kSpringFrames / kWalkFPT : 1;
  }
  return n;
}
bool hasJump(const std::vector<int16_t>& wp) { return wpSteps(wp) > int(wp.size() / 2) - 1; }
int sx16(int v) { return v >= 0x8000 ? v - 0x10000 : v; }

// Cells P1 can walk now: soft blocks, bombs (but its own cell) and flames
// are not.
struct Grid {
  const World& w;
  int pc, pr;
  explicit Grid(const World& world) : w(world), pc(world.p.x / 16), pr(world.p.y / 16) {}
  int idx(int c, int r) const { return r * w.cols + c; }
  bool open(int c, int r, int fromC = -1, int fromR = -1) const {
    if (!w.inside(c, r)) return false;
    char t = w.at(c, r);
    if (c == fromC && r == fromR) return true;
    if (t == '*' || t == 'b' || breakable(t)) return false;
    if (w.bombAt(c, r)) return false;
    return walkableClass(t);
  }
  // The cells a step from (c,r) reaches: its neighbours P1 can walk (or
  // spring pads), or from a pad only its jumps' landings (goap.hpp).
  template <class F>
  void steps(int c, int r, int c0, int r0, F f) const {
    int land[kMaxJumps][2], nj = jumps(w, c, r, land);
    if (nj >= 0) {
      for (int i = 0; i < nj; i++) f(land[i][0], land[i][1], true);
      return;
    }
    for (auto& d : kDirs) {
      int nc = c + d[0], nr = r + d[1];
      if (w.inside(nc, nr) && (w.at(nc, nr) == 'S' || open(nc, nr, c0, r0))) f(nc, nr, false);
    }
  }
  // Shorten a cell path to n cells, but never to end on a pad or a jump-pad
  // object's cell (P1 is thrown on: 3-6, a walk cut on (5,13) bounced
  // there); up to the next cell P1 can stop on.
  void cut(std::vector<int>& p, size_t n) const {
    while (n < p.size() && throwsP1(p[n - 1])) n++;
    if (p.size() > n) p.resize(n);
  }
  bool throwsP1(int k) const { int l[kMaxJumps][2]; return jumps(w, k % w.cols, k / w.cols, l) >= 0; }
  // BFS from (c0,r0): dist (-1 unreachable) and prev cell index. A pad is
  // passed through, never a place to stop: its dist stays -1.
  void bfs(int c0, int r0, std::vector<int>& dist, std::vector<int>& prev) const {
    dist.assign(size_t(w.cols * w.rows), -1);
    prev.assign(size_t(w.cols * w.rows), -1);
    if (!w.inside(c0, r0)) return;
    std::deque<int> q{idx(c0, r0)};
    dist[size_t(idx(c0, r0))] = 0;
    while (!q.empty()) {
      int k = q.front();
      q.pop_front();
      int c = k % w.cols, r = k / w.cols;
      steps(c, r, c0, r0, [&](int nc, int nr, bool jump) {
        if (dist[size_t(idx(nc, nr))] >= 0) return;
        dist[size_t(idx(nc, nr))] = dist[size_t(k)] + (jump ? kSpringFrames / kWalkFPT : 1);
        prev[size_t(idx(nc, nr))] = k;
        q.push_back(idx(nc, nr));
      });
    }
    for (int k = 0; k < w.cols * w.rows; k++)
      if (dist[size_t(k)] > 0 && throwsP1(k)) dist[size_t(k)] = -1;
  }
  std::vector<int> pathTo(const std::vector<int>& prev, int c0, int r0, int c, int r) const {
    std::vector<int> cells;
    for (int k = idx(c, r); k >= 0; k = prev[size_t(k)]) {
      cells.push_back(k);
      if (k == idx(c0, r0)) break;
    }
    std::reverse(cells.begin(), cells.end());
    return cells;
  }
  // Waypoints (cell centres) for a cell path; the first is the start cell,
  // so P1 lines up before it turns.
  std::vector<int16_t> waypoints(const std::vector<int>& cells) const {
    std::vector<int16_t> wp;
    for (size_t i = 0; i < cells.size(); i++) {
      int k = cells[i], p = i ? cells[i - 1] : -1;
      bool hold = p >= 0 && jumpHeld(w, p % w.cols, p / w.cols, k % w.cols, k / w.cols);
      bool ride = p >= 0 && (rideableAt(w, p % w.cols, p / w.cols) || rideDock(w, p % w.cols, p / w.cols));
      wp.push_back(int16_t(((k % w.cols) * 16 + 8) | (hold ? kJumpHold : 0) | (ride ? kRideEnd : 0)));
      wp.push_back(int16_t((k / w.cols) * 16 + 8));
    }
    return wp;
  }
  // Route to (tc,tr) where soft blocks cost extra (bombed on the way): the
  // cells, start first.
  std::vector<int> softRoute(int tc, int tr, const std::set<int>& avoid = {}) const {
    size_t n = size_t(w.cols * w.rows);
    std::vector<int> d(n, INT_MAX), prev(n, -1);
    using QE = std::pair<int, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    d[size_t(idx(pc, pr))] = 0;
    q.push({0, idx(pc, pr)});
    while (!q.empty()) {
      auto [g, k] = q.top();
      q.pop();
      if (g != d[size_t(k)]) continue;
      int c = k % w.cols, r = k / w.cols;
      int land[kMaxJumps][2], nj = jumps(w, c, r, land);
      for (int di = 0; di < (nj >= 0 ? nj : 4); di++) {
        auto& dd = kDirs[di % 4];
        int nc = c + dd[0], nr = r + dd[1];
        int cost;
        if (nj >= 0) {
          nc = land[di][0], nr = land[di][1];
          cost = kSpringFrames / kWalkFPT;
        }
        else if (!w.inside(nc, nr)) continue;
        else if (breakable(w.at(nc, nr))) {
          if (avoid.count(idx(nc, nr))) continue;
          cost = 10;
        }
        else if (w.at(nc, nr) == 'S' || open(nc, nr, pc, pr)) cost = 1;
        else continue;
        int nk = idx(nc, nr);
        if (g + cost < d[size_t(nk)]) {
          d[size_t(nk)] = g + cost;
          prev[size_t(nk)] = k;
          q.push({g + cost, nk});
        }
      }
    }
    if (d[size_t(idx(tc, tr))] == INT_MAX) return {};
    return pathTo(prev, pc, pr, tc, tr);
  }
};

bool covers(const World& w, int bc, int br, int c, int r) {
  for (const Cell& x : blastCells(w, bc, br, w.p.fire, w.p.pierce))
    if (x.c == c && x.r == r) return true;
  return false;
}

}  // namespace

Live readLive(const uint8_t* w) {
  Live l;
  int t0 = w[0x1C000];
  bool hard = (t0 >= 0xC3 && t0 <= 0xCF) || (t0 >= 0xD0 && t0 <= 0xEF);
  l.inStage = hard && !(rd16(w, 0xB9) & 0x8000) && w[0xBC] != 0 && (rd16(w, 0x8000) & 0xF000) == 0x9000;
  l.locked = (rd16(w, 0x8052) & 1) || rd16(w, 0x5E38) != 0;  // (+ PlayerCutsceneLock: intros)
  l.px = uint16_t(sx16(rd16(w, 0x8010)) + sx16(rd16(w, 0x5F01)));
  l.py = uint16_t(sx16(rd16(w, 0x8018)) + sx16(rd16(w, 0x5EFB)));
  // Held by a spring pad ($0801 on a $4C, docs/mechanics.md 5): the way
  // held when it lets go is the jump, so decide (nothing held bounces in
  // place, forever).
  int c = (l.px & 0x1FF) / 16, r = (l.py & 0x1FF) / 16;
  if (rd16(w, 0x8052) == 0x0801 && c < 32 && r < 32 && w[0x1C000 + r * 0x80 + c * 2] == 0x4C) l.locked = false;
  agent_enemies(w, &l.enemies, &l.hp);
  l.enemyMask = agent_enemy_mask(w);
  l.mapSig = agent_map_sig(w);
  return l;
}

void noteRemotes(const World& w, std::vector<int>& order) {
  std::vector<int> now;
  for (const Bomb& b : w.bombs)
    if (b.remote && !b.enemyOwned && w.inside(b.c, b.r)) now.push_back(b.r * w.cols + b.c);
  order.erase(std::remove_if(order.begin(), order.end(),
                             [&](int k) { return std::find(now.begin(), now.end(), k) == now.end(); }),
              order.end());
  for (int k : now)
    if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
}

bool remoteTrigger(const World& w, const std::vector<int>& order, std::vector<uint8_t>* blast, bool needEnemy) {
  if (order.empty()) return false;
  std::vector<uint8_t> in(size_t(w.cols * w.rows), 0);
  // The oldest, and every bomb of P1's its blast reaches (they go too).
  std::vector<int> q{order[0]}, fired{order[0]};
  while (!q.empty()) {
    int k = q.back();
    q.pop_back();
    const Bomb* b = w.bombAt(k % w.cols, k / w.cols);
    for (const Cell& x : blastCells(w, k % w.cols, k / w.cols, b ? b->range : w.p.fire, b ? b->pierce : w.p.pierce)) {
      int j = x.r * w.cols + x.c;
      in[size_t(j)] = 1;
      const Bomb* o = w.bombAt(x.c, x.r);
      if (o && !o->enemyOwned && std::find(fired.begin(), fired.end(), j) == fired.end()) fired.push_back(j), q.push_back(j);
    }
  }
  if (blast) *blast = in;
  auto hit = [&](int c, int r) { return w.inside(c, r) && in[size_t(r * w.cols + c)]; };
  // P1 clear of every remote bomb of its own, not just the oldest: one
  // already going off still reads as the oldest, and B then sets off the
  // next (2-3: a second press for (8,6) set off the bomb P1 stood on).
  std::vector<uint8_t> any = in;
  for (int k : order) {
    const Bomb* b = w.bombAt(k % w.cols, k / w.cols);
    for (const Cell& x : blastCells(w, k % w.cols, k / w.cols, b ? b->range : w.p.fire, b ? b->pierce : w.p.pierce))
      any[size_t(x.r * w.cols + x.c)] = 1;
  }
  // (the flame kills by the cell under P1's point, C0:A5F1; P1 stands
  // still for the press, so a few px of margin: with 7, 1 px off centre
  // beside the blast never counted as clear, 3-6 sat 40000 frames boxed in
  // by its own trap)
  for (int dx : {-4, 4})
    for (int dy : {-4, 4}) {
      int c = (w.p.x + dx) / 16, r = (w.p.y + dy) / 16;
      if (w.inside(c, r) && any[size_t(r * w.cols + c)]) return false;
    }
  if (w.hasGate && hit(w.gateC, w.gateR)) return false;
  for (const Item& it : w.items)
    if (wantedItem(it.kind) && hit(it.c, it.r)) return false;
  if (!needEnemy) return true;
  for (const Enemy& e : w.enemies) {
    if (!e.type || (e.type & 0x40) || isThrower(e.type) || e.type == 0x8F || e.dormant() || e.hidden() || e.flashing() || e.offscreen) continue;
    if (w.intro && e.type >= 0xA4 && e.type <= 0xA8) continue;  // (no flame check in the intro)
    if (hit(e.col(), e.row())) return true;
  }
  return false;
}

int Agent::trapCheck(const World& tw) {
  noteRemotes(tw, remoteOrder_);
  if (remoteOrder_.empty()) return 0;
  // (on a spring pad B is the held jump, not the remote: 3-6 pressed it
  // 180 times on (5,21) and the trap never went off)
  if (tw.inside(tw.p.col(), tw.p.row()) && tw.at(tw.p.col(), tw.p.row()) == 'S') return 0;
  if (remoteTrigger(tw, remoteOrder_)) return 1;
  bool stale = tw.frame - trapSince_ > kTrapWait;
  if ((stale || trapStuck_) && remoteTrigger(tw, remoteOrder_, nullptr, false)) return 2;
  return 0;
}

void Agent::learnSpeed(const SimReq& q, const SimOut& r) {
  if (q.post.empty() || !q.pre.empty() || q.path.size() < 6) return;
  int cells = int(q.path.size() / 2) - 1;
  for (size_t f = 0; f < r.pads.size(); f++)
    if (r.pads[f] & kPadA) {
      if (r.dead < 0 || r.dead > int(f)) fpc_ = 0.5 * fpc_ + 0.5 * double(f) / cells;
      return;
    }
}

void Agent::log(const std::string& s) {
  char b[32];
  std::snprintf(b, sizeof b, "%6d ", frame_);
  log_ += b + s + "\n";
}

std::string Agent::takeLog() {
  std::string s;
  s.swap(log_);
  return s;
}

void Agent::watchLag(const uint8_t* w) {
  int t = rd16(w, 0x32);
  if (lastTick_ >= 0) {
    int d = (t - lastTick_) & 0xFFFF;
    if (d <= 2) {
      lagReal_ += 1, lagLogic_ += d;
      if (lagReal_ >= 256) {
        if (lagLogic_ > 0) eScale_ = std::clamp(lagReal_ / lagLogic_, 1.0, 4.0);
        lagReal_ *= 0.5, lagLogic_ *= 0.5;
      }
    }
  }
  lastTick_ = t;
}

// A boss in a move that senses nothing (e.g. 1-8's leap) still has its
// points: the last ones seen, so the goal stays and P1 gets in place.
void Agent::noteBosses(World& w) {
  senseStale_ = false;
  bool sensing = false;
  for (BossObj& b : w.bosses) {
    if (!b.sense.empty()) senseMem_[b.base] = b.sense, sensing = true;
    else if (b.hp > 0 && senseMem_.count(b.base)) b.sense = senseMem_[b.base], senseStale_ = true;
  }
  senseStale_ &= !sensing;
}

uint16_t Agent::frame(const uint8_t* wram, int frame, Oracle& o, const char* extra) {
  frame_ = frame;
  watchLag(wram);
  Live live = readLive(wram);
  if (!live.inStage || live.locked) {
    if (committed() && !live.inStage) pads_.clear();
    if (committed() && live.locked) {  // (a lock the oracle saw coming: play on)
      if (at_ < int(pads_.size())) return pads_[size_t(at_++)];
    }
    return 0;
  }
  // Remote traps: an enemy in the oldest one's blast, P1 out of it: B now
  // (held 4 frames, P1 standing; then decide again: the move the oracle
  // played had that bomb in place).
  if (trapB_ > 0) {
    trapB_--;
    return kPadB;
  }
  if (trapCool_ > 0) trapCool_--;
  if (holdsRemote_ && trapCool_ == 0) {
    World tw = captureWorld(wram, frame);
    int why = trapCheck(tw);
    if (why == 2 && committed()) why = 0;  // (a stale one waits for the move to end)
    if (why) {
      int k = remoteOrder_[0];
      char b[128];
      std::snprintf(b, sizeof b, "trap: B sets off the remote bomb at (%d,%d) %s", k % tw.cols, k / tw.cols,
                    why == 1 ? "on an enemy" : "(stale or in the way, P1 clear)");
      log(b);
      if (why == 2) trapStuck_ = false, trapSince_ = frame;
      pads_.clear();
      at_ = 0;
      trapB_ = 3;
      trapCool_ = 24;
      return kPadB;
    }
  }
  if (committed()) {
    int tx = trace_[size_t(2 * at_)], ty = trace_[size_t(2 * at_ + 1)];
    if (tx == int16_t(live.px) && ty == int16_t(live.py)) return pads_[size_t(at_++)];
    char b[160];
    std::snprintf(b, sizeof b, "off track at step %d of '%s': P1 (%d,%d), oracle (%d,%d); deciding again", at_,
                  what_.c_str(), live.px, live.py, tx, ty);
    log(b);
    pads_.clear();
  }
  World w = captureWorld(wram, frame);
  if (extra && *extra) {  // (boss lines: parsed by the one snapshot parser)
    World x;
    if (parseWorld("worldview 1\nsize 1 1\n" + std::string(extra) + "\nmap\n.\n", x)) w.bosses = x.bosses;
  }
  noteBosses(w);
  for (const BossObj& b : w.bosses) live.bossHp += b.hp;
  decide(w, live, o);
  return committed() ? pads_[size_t(at_++)] : 0;
}

// Oracle batch sizes: the first option alone (it is the best guess and
// usually lives), then wider. Runs in a batch go in parallel, so a lone
// first run spends no CPU on options that turn out not to be needed.
int rideEnd(const SimOut& r, int c, int rr) {
  const int kStill = 20;
  size_t nf = r.trace.size() / 2;
  bool on = false, left = false;  // (boarded: on the cell; then carried off it)
  for (size_t f = 0; f + kStill < nf; f++) {
    int x = r.trace[2 * f], y = r.trace[2 * f + 1];
    if (x / 16 == c && y / 16 == rr) on = true;
    else if (on && std::abs(x / 16 - c) + std::abs(y / 16 - rr) > 1) left = true;
    if (!left) continue;
    bool still = true;
    for (size_t g = f + 1; g <= f + kStill && still; g++) still = r.trace[2 * g] == x && r.trace[2 * g + 1] == y;
    if (still) return r.dead >= 0 && r.dead <= int(f + kStill) ? -1 : int(f);
  }
  return -1;
}

static size_t batchSize(size_t n) { return n == 0 ? 1 : n == 1 ? 3 : 4; }

bool Agent::tryCands(std::vector<Cand>& cs, const Live& live, Oracle& o, size_t maxRuns, bool waits) {
  if (cs.size() > maxRuns) cs.resize(maxRuns);
  bool died = false;  // (some option ran into something)
  for (size_t b = 0, bn = 0; b < cs.size(); b += batchSize(bn++)) {
    std::vector<SimReq> reqs;
    for (size_t i = b; i < std::min(cs.size(), b + batchSize(bn)); i++) {
      reqs.push_back(cs[i].req);
      reqs.back().bossBase = bossBase_;
    }
    std::vector<SimOut> out;
    o.run(reqs, out);
    sims_ += int(reqs.size());
    for (size_t i = 0; i < out.size(); i++) {
      const Cand& c = cs[b + i];
      const SimOut& r = out[i];
      learnSpeed(c.req, r);
      died |= r.dead >= 0;
      if (verbose) {
        char vb[240];
        int aAt = -1;
        for (size_t f = 0; f < r.pads.size() && aAt < 0; f++)
          if (r.pads[f] & kPadA) aAt = int(f);
        std::snprintf(vb, sizeof vb, "  opt %zu %s -> dead %d en %d/%d hp %d/%d boss %d/%d at (%d,%d)%s A@%d cells %zu", b + i + 1,
                      c.what.c_str(), r.dead, r.enemies, live.enemies, r.hp, live.hp, r.bossHp, live.bossHp, r.px, r.py,
                      r.mapSig != live.mapSig ? " map" : r.mapChanged ? " map(back)" : "", aAt, c.req.path.size() / 2 - 1);
        log(vb);
        {  // where P1 stopped for good, and the waypoints it was given
          size_t nf = r.trace.size() / 2, still = nf;
          while (still > 1 && r.trace[2 * (still - 1)] == r.trace[2 * (still - 2)] &&
                 r.trace[2 * (still - 1) + 1] == r.trace[2 * (still - 2) + 1])
            still--;
          std::string wp;
          for (size_t k = 0; k + 1 < c.req.path.size(); k += 2)
            wp += " " + std::to_string((c.req.path[k] & ~kWpFlags) / 16) + "," + std::to_string(c.req.path[k + 1] / 16);
          std::string tr;
          for (size_t k = 0; k < nf; k += 24) tr += " " + std::to_string(r.trace[2 * k]) + "," + std::to_string(r.trace[2 * k + 1]);
          log("    trace/24:" + tr);
          if (nf) log("    still from " + std::to_string(still ? still - 1 : 0) + " at (" + std::to_string(r.trace[2 * (nf - 1)]) + "," +
                      std::to_string(r.trace[2 * (nf - 1) + 1]) + "); path" + wp);
        }
        if (!r.etrace.empty()) {
          for (int sl = 0; sl < kEnemySlots; sl++) {
            std::string t;
            int last = -2;
            for (size_t f = 0; f * kEnemySlots < r.etrace.size(); f++) {
              int v = r.etrace[f * kEnemySlots + size_t(sl)];
              if (v == last) continue;
              last = v;
              char tb[32];
              if (v < 0) std::snprintf(tb, sizeof tb, " %zu:-", f);
              else std::snprintf(tb, sizeof tb, " %zu:(%d,%d)", f, v & 0xFF, v >> 8);
              t += tb;
            }
            if (t != " 0:-") log("    slot" + std::to_string(sl + 4) + t);
          }
        }
      }
      bool ok = false;
      bool at = c.endC < 0 || (std::abs(r.px - (c.endC * 16 + 8)) <= 2 && std::abs(r.py - (c.endR * 16 + 8)) <= 2);
      switch (c.want) {
        case Cand::Exit: ok = r.dead == -2; break;
        case Cand::Live: ok = r.dead == -1 && at; break;
        case Cand::Change: ok = r.dead == -1 && at && (r.mapSig != live.mapSig || r.mapChanged); break;
        case Cand::Kill: {
          // (or the target's own slot no longer an enemy: a $98 hit is a
          // mount plus a new $99 rider, the count stays)
          bool gone = killSlot_ >= 0 && (live.enemyMask >> killSlot_ & 1) && !(r.enemyMask >> killSlot_ & 1);
          ok = r.dead == -1 && at && (r.enemies < live.enemies || r.hp < live.hp || gone);
          break;
        }
        case Cand::BossHit: ok = r.dead == -1 && at && r.bossHp <= live.bossHp - c.drop; break;
      }
      if (ok) {
        pads_ = r.pads;
        trace_ = r.trace;
        lastPx_ = r.px, lastPy_ = r.py;
        int n = c.want == Cand::Exit ? int(r.pads.size()) : std::min<int>(c.commit, int(r.pads.size()));
        if (c.afterA || c.afterB) {
          int aAt = -1, bAt = -1;
          for (size_t f = 0; f < r.pads.size(); f++) {
            if (aAt < 0 && (r.pads[f] & kPadA)) aAt = int(f);
            if (r.pads[f] & kPadB) bAt = int(f);
          }
          if (c.afterB && bAt >= 0) n = std::min(n, bAt + c.afterB);
          else if (c.afterA && aAt >= 0) n = std::min(n, aAt + c.afterA);
        }
        pads_.resize(size_t(std::max(1, n)));
        trace_.resize(pads_.size() * 2);
        at_ = 0;
        what_ = c.what;
        char msg[200];
        std::snprintf(msg, sizeof msg, "do %s (%d frames; option %zu, %d sims so far)", c.what.c_str(),
                      int(pads_.size()), b + i + 1, sims_);
        log(msg);
        return true;
      }
    }
  }
  // Every option ran into something: the best few again after standing
  // still a while (3-2, fire 1: the way along row 11 met the immune $8F
  // patrolling it every time; P1 survived on the spot for 15000 frames).
  if (waits && died && !cs.empty()) {
    std::vector<Cand> ws;
    for (int wait : {32, 64, 128})
      for (size_t i = 0; i < std::min<size_t>(2, cs.size()); i++) {
        Cand c = cs[i];
        c.req.pre.insert(c.req.pre.begin(), size_t(wait), 0);
        c.commit += wait;
        c.req.frames += wait;
        c.what += " (wait " + std::to_string(wait) + ")";
        ws.push_back(c);
      }
    return tryCands(ws, live, o, ws.size(), false);
  }
  return false;
}

// Nothing for the task lives (or there is no task): move to a cell nearby
// that lives, else the one that lives longest; play a little, decide again.
void Agent::survive(const World& w, const Live& live, Oracle& o, const char* why) {
  // Nothing to do but stay alive with traps out: they may be what is in the
  // way (a bomb is a wall). frame() sets the oldest off once P1 is clear.
  trapStuck_ = !remoteOrder_.empty() && std::string(why).rfind("traps out", 0) != 0;
  Grid g(w);
  std::vector<int> dist, prev;
  g.bfs(g.pc, g.pr, dist, prev);
  std::vector<std::pair<int, int>> cells;  // (dist, idx)
  for (int k = 0; k < w.cols * w.rows; k++)
    if (dist[size_t(k)] >= 0 && dist[size_t(k)] <= 6 && w.at(k % w.cols, k / w.cols) != 'S')  // (no standing on a pad)
      cells.push_back({dist[size_t(k)], k});
  std::sort(cells.begin(), cells.end());
  // Cells out of every known blast first, then (stuck behind a trap) out
  // of the oldest trap's, so frame() can set it off.
  DangerMap dm(w);
  std::vector<uint8_t> trap;
  if (trapStuck_) remoteTrigger(w, remoteOrder_, &trap, false);
  // (a cell next to the blast: P1 a pixel off its centre is still in it,
  // to remoteTrigger's box)
  auto inTrap = [&](int c, int r) { return !trap.empty() && w.inside(c, r) && trap[size_t(r * w.cols + c)]; };
  auto rank = [&](int k) {
    int c = k % w.cols, r = k / w.cols, near = 0;
    for (auto& d : kDirs) near |= inTrap(c + d[0], r + d[1]);
    return 4 * dm.deadly(c, r, 0, 400) + 2 * inTrap(c, r) + near;
  };
  std::stable_sort(cells.begin(), cells.end(), [&](auto& a, auto& b) { return rank(a.second) < rank(b.second); });
  std::vector<SimReq> reqs;
  std::vector<int> ends;
  for (auto& [d, k] : cells) {
    if (reqs.size() >= 16) break;
    SimReq r;
    r.path = g.waypoints(g.pathTo(prev, g.pc, g.pr, k % w.cols, k / w.cols));
    r.frames = kWalkFPT * d + kSettle + 120;
    reqs.push_back(r);
    ends.push_back(k);
  }
  int best = -1, bestDead = -1;
  std::vector<SimOut> keep;
  for (size_t b = 0, bn = 0; b < reqs.size(); b += batchSize(bn++)) {
    std::vector<SimReq> part(reqs.begin() + long(b), reqs.begin() + long(std::min(reqs.size(), b + batchSize(bn))));
    std::vector<SimOut> out;
    o.run(part, out);
    sims_ += int(part.size());
    for (size_t i = 0; i < out.size(); i++) {
      int dd = out[i].dead == -1 ? INT_MAX : out[i].dead == -2 ? INT_MAX - 1 : out[i].dead;
      if (dd > bestDead) {
        bestDead = dd;
        best = int(keep.size());
      }
      keep.push_back(out[i]);
    }
    if (bestDead == INT_MAX) break;
  }
  char b[200];
  if (best < 0) {
    pads_.clear();
    std::snprintf(b, sizeof b, "survive (%s): nowhere to go", why);
    log(b);
    return;
  }
  int k = ends[size_t(best)];
  pads_ = keep[size_t(best)].pads;
  trace_ = keep[size_t(best)].trace;
  // (a run that lived: play all but its last kTail frames, at most 60)
  int n = bestDead == INT_MAX ? std::clamp(int(keep[size_t(best)].pads.size()) - kTail, 24, 60)
                              : std::max(1, std::min(8, bestDead / 2));
  pads_.resize(std::min(pads_.size(), size_t(n)));
  trace_.resize(pads_.size() * 2);
  at_ = 0;
  what_ = "survive";
  std::snprintf(b, sizeof b, "survive (%s): toward (%d,%d), %s, play %d", why, k % w.cols, k / w.cols,
                bestDead == INT_MAX ? "lives" : ("dies at +" + std::to_string(bestDead)).c_str(), int(pads_.size()));
  log(b);
  (void)live;
}

bool Agent::bombDropped(int cell) const {
  auto n = bombFails_.find(cell), at = bombFailAt_.find(cell);
  return n != bombFails_.end() && n->second >= 3 && at != bombFailAt_.end() && frame_ - at->second < 2 * kBadSoftTTL;
}

std::set<int> Agent::badSoftNow() const {
  std::set<int> out;
  for (auto& [k, at] : badSoft_)
    if (frame_ - at < kBadSoftTTL) out.insert(k);
  return out;
}

void Agent::decide(const World& w0, const Live& live, Oracle& o) {
  // An item that would cost the remote (another bomb kind) is kept off:
  // a wall as far as routes go.
  World w = w0;
  w.items.erase(std::remove_if(w.items.begin(), w.items.end(),
                               [&](const Item& it) {
                                 if (!unwantedItem(it.kind, w.p) || !w.inside(it.c, it.r)) return false;
                                 if (it.c != w.p.x / 16 || it.r != w.p.y / 16) w.map[size_t(it.r * w.cols + it.c)] = '#';
                                 return true;
                               }),
                w.items.end());
  // Riding a mount ($90/$98, P1 +$60 bit 15) no spring pad launches P1:
  // a pad is floor (3-1: riding a $98, the route over the pad at (4,6)
  // to the east half never went).
  const bool riding = w.p.f60 & 0x8000;
  if (riding)
    for (char& ch : w.map)
      if (ch == 'S') ch = '.';
  decisions_++;
  frame_ = w.frame;
  killSlot_ = -1;
  holdsRemote_ = w.p.remote;
  noteRemotes(w, remoteOrder_);
  int mine = int(remoteOrder_.size());  // P1's remote bombs out
  if (lastEnemies_ >= 0 && live.enemies < lastEnemies_) trapSince_ = w.frame;  // (a kill: the traps work)
  lastEnemies_ = live.enemies;
  if (w.stage != stage_) stage_ = w.stage, bombFails_.clear(), bombFailAt_.clear(), badSoft_.clear(), itemFails_.clear(), softSeen_.clear(), rides_.clear(),
                            rideFails_.clear(), dockFor_ = -1;
  // The ride just probed let P1 off: its object sits next to P1 now, where
  // it can be boarded back (its dock).
  if (dockFor_ >= 0 && dockFor_ < int(rides_.size())) {
    for (const Enemy& e : w.enemies)
      if (isRideable(e.type) && std::abs(e.col() - w.p.x / 16) + std::abs(e.row() - w.p.y / 16) <= 1)
        rides_[size_t(dockFor_)].dc = e.col(), rides_[size_t(dockFor_)].dr = e.row();
    dockFor_ = -1;
  }
  w.rides.insert(w.rides.end(), rides_.begin(), rides_.end());  // (a snapshot's own rides kept: repros)
  for (int k = 0; k < w.cols * w.rows; k++)
    if (breakable(w.map[size_t(k)])) softSeen_.insert(k);
  worldText_ = encodeWorld(w);
  bossBase_.clear();
  for (const BossObj& b : w.bosses) bossBase_.push_back(uint16_t(b.base));
  pads_.clear();
  at_ = 0;
  Grid g(w);
  if (!w.inside(g.pc, g.pr)) return survive(w, live, o, "P1 off the map");
  std::vector<int> dist, prev;
  g.bfs(g.pc, g.pr, dist, prev);

  // Soft blocks a bomb already out will break are done as far as the plan
  // goes: with bombs to spare P1 doesn't wait for one blast before the next.
  std::set<int> pending;
  for (const Bomb& b : w.bombs)
    for (const Cell& x : blastCells(w, b.c, b.r, b.range, b.pierce))
      if (breakable(w.at(x.c, x.r))) pending.insert(g.idx(x.c, x.r));
  World pw = w;
  for (int k : pending) pw.map[size_t(k)] = '.';
  // Power-ups are optional: one that failed 3 times is left (P1 can't get
  // there, e.g. a route the map model gets wrong) rather than stall on it.
  auto given = [&](const Item& it) {
    auto f = itemFails_.find(g.idx(it.c, it.r));
    return f != itemFails_.end() && f->second >= 3;
  };
  pw.hidden.erase(std::remove_if(pw.hidden.begin(), pw.hidden.end(), given), pw.hidden.end());
  for (Item& it : pw.items)
    if (given(it)) it.kind = -1;
  // Get off the mount: a bomb on P1's own cell, P1 staying in the flame
  // (C0:A630 -> C1:29E0: the mount is lost, not a heart; P1 flashes $C0
  // frames and the mount stays there, idle). Once per kBadSoftTTL.
  auto getOff = [&]() {
    if (!riding || frame_ - dismountAt_ < kBadSoftTTL || !canPlaceOn(w0.at(g.pc, g.pr)) || w.bombAt(g.pc, g.pr) ||
        w.p.bombs <= 0)
      return false;
    Cand c;
    c.req.pre = {kPadA, kPadA};
    c.req.detEnd = w.p.remote ? 1 + mine : 0;
    c.commit = 2 + int((w.p.remote ? 30 + kFlameFrames : kNewFuse + kFlameFrames) * eScale_ + 0.5);
    c.req.frames = c.commit + kTail;
    c.endC = g.pc, c.endR = g.pr;
    c.want = Cand::Live;
    c.what = "get off the mount: bomb at (" + std::to_string(g.pc) + "," + std::to_string(g.pr) + "), stay in it";
    std::vector<Cand> cs{c};
    dismountAt_ = frame_;
    return tryCands(cs, live, o, 1, false);
  };
  Plan gp = plan(pw, goal_);
  // Power-ups are optional: one no plan reaches (3-4: a fire-up in the
  // soft block at (7,8), ringed by pads) doesn't hold up the rest.
  if ((!gp.ok || gp.steps.empty()) && goal_.powerups) {
    Goal less = goal_;
    less.powerups = false;
    gp = plan(pw, less);
  }
  if (!gp.ok || gp.steps.empty()) {
    Goal more;
    more.blocks = true;
    gp = plan(pw, more);
    if (!gp.ok || gp.steps.empty()) {
      // A ride not tried yet may lead on (3-5: the island is reached by
      // submarine only): board it and see where it goes.
      for (const Enemy& e : w.enemies) {
        int c = e.col(), r = e.row(), k = w.inside(c, r) ? g.idx(c, r) : -1;
        if (!isRideable(e.type) || k < 0 || rideFails_[k] >= 2 || (prev[size_t(k)] < 0 && k != g.idx(g.pc, g.pr))) continue;
        bool known = false;
        for (const Ride& rd : rides_) known |= rd.c == c && rd.r == r;
        if (known) continue;
        auto p = g.pathTo(prev, g.pc, g.pr, c, r);
        SimReq q;
        q.path = g.waypoints(p);
        q.frames = kWalkFPT * wpSteps(q.path) + kSettle + kRideProbe;
        std::vector<SimOut> out;
        o.run({q}, out);
        sims_++;
        int end = out.empty() ? -1 : rideEnd(out[0], c, r);
        if (end >= 0) {  // (what comes after the landing is the next move's business)
          const SimOut& ro = out[0];
          pads_.assign(ro.pads.begin(), ro.pads.begin() + end + 1);
          trace_.assign(ro.trace.begin(), ro.trace.begin() + 2 * (end + 1));
          at_ = 0;
          int tc = ro.trace[size_t(2 * end)] / 16, tr = ro.trace[size_t(2 * end + 1)] / 16;
          rides_.push_back({c, r, tc, tr});
          dockFor_ = int(rides_.size()) - 1;
          what_ = "ride";
          char b[120];
          std::snprintf(b, sizeof b, "ride: (%d,%d) carries P1 to (%d,%d) (%d frames)", c, r, tc, tr, end);
          log(b);
          return;
        }
        pads_.clear();
        rideFails_[k]++;
      }
      // Riding a mount, every idle one is a wall (C1:C75A writes $C9 into
      // its cell while P1 +$60 bit 15); off the mount they are floor
      // again, and walking onto one boards it. A flame on P1 costs the
      // mount, not a heart (C0:A630 -> C1:29E0: P1 flashes $C0 frames, the
      // mount stays, idle): if a plan needs an idle mount's cell, P1 bombs
      // itself off (3-3: a $90 parked at (6,4) shut the corridor to the
      // last $99 for 12000 frames).
      // The same for a plan that needs the spring pads back.
      if (riding && pending.empty()) {
        World mw = pw;
        for (size_t k = 0; k < mw.map.size(); k++)
          if (w0.map[k] == 'S') mw.map[k] = 'S';
        for (const Enemy& e : w.enemies)
          if ((e.type == 0x90 || e.type == 0x98) && (e.be & 0xC000) == 0x8000 && w.inside(e.col(), e.row()) &&
              mw.at(e.col(), e.row()) == '#')
            mw.map[size_t(g.idx(e.col(), e.row()))] = '.';
        if (mw.map != pw.map && plan(mw, more).ok && getOff()) return;
      }
      return survive(w, live, o, pending.empty() ? "no plan" : "waiting for a blast");
    }
  }
  // The first step not stuck behind a pending blast (its route's first soft
  // block is one a bomb out will break: wait for it, or do a later step).
  size_t si = 0;
  for (; si < gp.steps.size() && si < 4; si++) {
    const Step& s = gp.steps[si];
    if (pending.empty() || dist[size_t(g.idx(s.standC, s.standR))] >= 0) break;
    auto route = g.softRoute(s.standC, s.standR, badSoftNow());
    size_t i = 1;
    while (i < route.size() && !breakable(w.at(route[i] % w.cols, route[i] / w.cols))) i++;
    if (i >= route.size() || !pending.count(route[i])) break;
  }
  if (si >= gp.steps.size() || si >= 4) return survive(w, live, o, "waiting for a blast");
  if (verbose) {
    std::string ps = gp.str();
    for (char& ch : ps)
      if (ch == '\n') ch = ';';
    log("  plan: " + ps.substr(0, 400));
  }
  const Step& st = gp.steps[si];
  const Task& task = gp.tasks[size_t(st.task)];
  auto itemFailed = [&] {
    if (task.kind == TaskKind::PickItem && task.powerup) itemFails_[g.idx(task.c, task.r)]++;
  };
  // Every bomb is out: a bomb task waits for one to come back.
  bool needsBomb = (task.kind != TaskKind::PickItem && task.kind != TaskKind::EnterGate) ||
                   dist[size_t(g.idx(st.standC, st.standR))] < 0;
  if (needsBomb && w.p.bombs <= 0 && !w.bombs.empty()) return survive(w, live, o, "no bomb left");
  char tb[96];
  std::snprintf(tb, sizeof tb, "%s %d,%d", taskName(task.kind), task.c, task.r);
  std::string tname = tb;

  // Bomb at x, then cover: candidates for every safe cell near x.
  // (risk: odds an enemy comes by a cover cell in [t0, t1], if known:
  // risky covers go last; waitPre: frames P1 stands before it sets off)
  using Risk = std::function<double(int, int, int, int)>;
  // A blast that reaches the gate brings enemies back; one that reaches an
  // item burns it (goap.cpp's Harm): no bomb there, unless the item is out
  // of reach (then the plan has priced burning it in).
  std::vector<uint8_t> panelsNow = panelsDone(w);
  auto spoils = [&](int xc, int xr) {
    for (const Cell& x : blastCells(w, xc, xr, w.p.fire, w.p.pierce)) {
      if (w.hasGate && x.c == w.gateC && x.r == w.gateR) return true;
      for (size_t i = 0; i < w.panels.size(); i++)  // (a done mode 1 panel: undone)
        if (w.panels[i].mode == 1 && panelsNow[i] && w.panels[i].c == x.c && w.panels[i].r == x.r) return true;
      for (const Item& it : w.items)
        if (it.c == x.c && it.r == x.r && wantedItem(it.kind) && dist[size_t(g.idx(x.c, x.r))] >= 0 &&
            !(x.c == xc && x.r == xr))  // (one on the bomb's cell: P1 picks it up placing it)
          return true;
    }
    return false;
  };
  auto bombCands = [&](int xc, int xr, Cand::Want want, const std::string& what, int maxY, std::vector<Cand>& out,
                       const Risk& risk = nullptr, int waitPre = 0) {
    if (dist[size_t(g.idx(xc, xr))] < 0 || !canPlaceOn(w.at(xc, xr)) || w.bombAt(xc, xr) || spoils(xc, xr)) return;
    auto p1 = g.pathTo(prev, g.pc, g.pr, xc, xr);
    int tx = waitPre + int(fpc_ * double(p1.size() - 1) + 0.5) + 2;  // the press, by P1's speed
    int t1 = kWalkFPT * wpSteps(g.waypoints(p1)) + kSettle;
    std::vector<int> d2, prev2;
    g.bfs(xc, xr, d2, prev2);
    DangerMap dm(w);
    dm.addBomb(xc, xr, w.p.fire, w.p.pierce, t1 + kNewFuse);
    for (const Bomb& b : w.bombs)  // (P1's remote bombs out: set off first, by the same B presses)
      if (b.remote && !b.enemyOwned) dm.addBomb(b.c, b.r, b.range, b.pierce, t1 + kNewFuse);
    // A bomb under a furnace (2-5: the $DF takes it in) sets the fuse
    // lines alight: every 'f' cell burns a while later (World2FuseIgnite
    // C1:4922), so no cover on one (2-5: all four covers died at A+88).
    bool underFurnace = w.inside(xc, xr - 1) && w.at(xc, xr - 1) == 'F';
    std::vector<std::pair<int, int>> ys;
    for (int k = 0; k < w.cols * w.rows; k++) {
      if (d2[size_t(k)] <= 0) continue;
      int yc = k % w.cols, yr = k / w.cols;
      if (underFurnace && w.at(yc, yr) == 'f') continue;
      int t2 = t1 + kWalkFPT * d2[size_t(k)] + kSettle;
      if (dm.deadly(yc, yr, t2, t1 + kNewFuse + kFlameFrames + kTail)) continue;
      int key = d2[size_t(k)];
      if (risk) {
        // (P1 there from about tx + its walk until the commit's tail)
        int ty = tx + int(fpc_ * d2[size_t(k)]);
        if (risk(yc, yr, ty - 8, tx + kNewFuse + kFlameFrames + kTail) > 0.2) key += 1000;
      }
      ys.push_back({key, k});
    }
    std::sort(ys.begin(), ys.end());
    int n = 0;
    for (auto& [key, k] : ys) {
      if (n++ >= maxY) break;
      int d = key % 1000;
      Cand c;
      c.req.path = g.waypoints(p1);
      c.req.post = {kPadA, kPadA};
      c.req.path2 = g.waypoints(g.pathTo(prev2, xc, xr, k % w.cols, k / w.cols));
      c.req.detEnd = w.p.remote ? 1 + mine : 0;
      int t2 = kWalkFPT * d + kSettle;
      int wait = int((w.p.remote ? 30 + kFlameFrames : kNewFuse + kFlameFrames) * eScale_ + 0.5);  // (logic frames: lag stretches them)
      c.commit = t1 + 2 + t2 + wait;
      c.req.frames = c.commit + kTail;
      // (play to the flames' end by the real press, not the walk budget)
      if (w.p.remote) c.afterB = int((30 + kFlameFrames) * eScale_ + 0.5);
      else c.afterA = 2 + wait + 4;
      // A bomb to spare: once in cover P1 is free to plan the next (the run
      // still covers the whole blast).
      if (!w.p.remote && w.p.bombs >= 2 && want == Cand::Change) c.commit = t1 + 2 + t2 + kSettle, c.afterA = 0;
      c.endC = k % w.cols;
      c.endR = k / w.cols;
      c.want = want;
      char b[160];
      std::snprintf(b, sizeof b, "%s: bomb at (%d,%d), cover (%d,%d)", what.c_str(), xc, xr, c.endC, c.endR);
      c.what = b;
      out.push_back(c);
    }
  };
  // Covers enemies are likely to come by go last for every bomb move, not
  // just kills: the enemy model rolled once with no bomb, P1 where it is
  // (2-2: a cover at the end of a dead end, a $86 walked in after P1).
  std::vector<EnemyRun> amb;
  bool ambRolled = false;
  auto ambient = [&]() -> Risk {
    if (!ambRolled) {
      ambRolled = true;
      bool any = false;
      for (const Enemy& q : w.enemies)
        if (EnemyModel::modelled(q) && std::abs(q.col() - g.pc) + std::abs(q.row() - g.pr) <= 16) any = true;
      if (any) amb = EnemyModel(w, 700, uint32_t(w.frame) * 2654435761u + 7, eScale_).roll(nullptr, g.pc, g.pr, 16);
    }
    if (amb.empty()) return nullptr;
    return [&amb](int c, int r, int t0, int t1) { return EnemyModel::nearOdds(amb, c, r, t0, t1); };
  };
  auto walkCand = [&](int tc, int tr, Cand::Want want, const std::string& what) {
    Cand c;
    auto p = g.pathTo(prev, g.pc, g.pr, tc, tr);
    c.req.path = g.waypoints(p);
    c.commit = kWalkFPT * wpSteps(c.req.path) + kSettle;
    c.req.frames = c.commit + kTail;
    c.want = want;
    if (want != Cand::Exit) c.endC = tc, c.endR = tr;
    c.what = what + ": walk to (" + std::to_string(tc) + "," + std::to_string(tr) + ")";
    return c;
  };

  std::vector<Cand> cs;
  // Where the task is done from; if a soft block is in the way, bomb the
  // first one on the route instead.
  int sc = st.standC, sr = st.standR;
  if (dist[size_t(g.idx(sc, sr))] < 0) {
    // The first soft block on the route, from the cell before it, else
    // from any cell whose blast reaches it; a cell that failed 3 times
    // (the game won't take a bomb there) is dropped, and a soft block no
    // cell is left for is routed around.
    std::vector<int> tried;
    std::set<int> bad = badSoftNow();
    for (int pass = 0; pass < 4 && cs.empty(); pass++) {
      auto route = g.softRoute(sc, sr, bad);
      size_t i = 1;
      while (i < route.size() && !breakable(w.at(route[i] % w.cols, route[i] / w.cols))) i++;
      if (i >= route.size()) break;
      int k = route[i], kc = k % w.cols, kr = k / w.cols;
      std::vector<std::pair<int, int>> xs{{-1, route[i - 1]}};
      for (int x = 0; x < w.cols * w.rows; x++)
        if (x != route[i - 1] && dist[size_t(x)] >= 0 && covers(w, x % w.cols, x / w.cols, kc, kr))
          xs.push_back({dist[size_t(x)], x});
      std::sort(xs.begin(), xs.end());
      for (auto& [d, x] : xs) {
        if (bombDropped(x) || tried.size() >= 2) continue;
        size_t n0 = cs.size();
        bombCands(x % w.cols, x / w.cols, Cand::Change, "clear the way to " + tname, 4, cs, ambient());
        if (cs.size() > n0) tried.push_back(x);
      }
      if (cs.empty()) bad.insert(k), badSoft_[k] = w.frame;
    }
    if (!cs.empty() && tryCands(cs, live, o, 8)) return;
    for (int x : tried) {
      if (frame_ - bombFailAt_[x] >= 2 * kBadSoftTTL) bombFails_[x] = 0;  // (worn off: count afresh)
      bombFails_[x]++, bombFailAt_[x] = frame_;
    }
    itemFailed();
    return survive(w, live, o, ("way to " + tname).c_str());
  }
  switch (task.kind) {
    case TaskKind::EnterGate:
      // Riding and the gate still shut (the plan has nothing left to
      // kill): the mount's own $99 rider is parked dormant while P1 rides
      // (3-1: 10000 frames by the shut gate on a $98).
      if (riding && !w.gateOpen && getOff()) return;
      cs.push_back(walkCand(task.c, task.r, Cand::Exit, tname));
      break;
    case TaskKind::PickItem:
      cs.push_back(walkCand(task.c, task.r, Cand::Change, tname));
      break;
    case TaskKind::BombSoft:
    case TaskKind::TogglePanel:
    case TaskKind::LightCrystal:
    case TaskKind::PushBlock:
    case TaskKind::PressButton:
      bombCands(sc, sr, Cand::Change, tname, 6, cs, ambient());
      break;
    case TaskKind::HurtBoss: {
      // The boss's path: one run with P1 holding still, every boss object's
      // position per frame. A bomb at x (reached in d cells after `wait`)
      // hurts it if, while the flames are out, one of its sense points is
      // in the blast, and none was on x from the drop to the blast (a bomb
      // on a sense point is eaten, BossEatBomb CC:A2C4). Earliest first;
      // the oracle checks each (a boss reacts to P1, e.g. 1-8's strikes).
      const int kMaxD = 12, kMaxWait = 120;
      const int fuse = int(kNewFuse * eScale_ + 0.5), flame = int(kFlameFrames * eScale_ + 0.5);
      SimReq probe;
      probe.path = g.waypoints({g.idx(g.pc, g.pr)});
      probe.frames = int(fpc_ * kMaxD) + kMaxWait + 4 + fuse + flame;
      probe.bossBase = bossBase_;
      probe.wantBoss = true;
      std::vector<SimOut> pout;
      o.run({probe}, pout);
      sims_++;
      if (pout.empty() || pout[0].btrace.empty()) return survive(w, live, o, tname.c_str());
      const std::vector<int16_t>& bt = pout[0].btrace;
      const int nb = int(bossBase_.size()), pf = int(bt.size()) / (2 * nb);
      // Sense cells at frame f (cell index per point), from the last frame's points.
      auto senseCells = [&](int f, std::vector<int>& out) {
        out.clear();
        for (int b = 0; b < nb; b++) {
          const BossObj& bo = w.bosses[size_t(b)];
          int x = bt[size_t(2 * (nb * f + b))], y = bt[size_t(2 * (nb * f + b) + 1)];
          for (auto& sp : bo.sense) {
            int c = (x + sp.first) >> 4, r = (y + sp.second) >> 4;
            if (w.inside(c, r)) out.push_back(g.idx(c, r));
          }
        }
      };
      std::vector<std::vector<int>> sense(static_cast<size_t>(pf));
      for (int f = 0; f < pf; f++) senseCells(f, sense[size_t(f)]);
      // (the contact boxes, for covers: a cell any box reaches in [t0, t1])
      auto boxed = [&](int c, int r, int t0, int t1) {
        int px = c * 16 + 8, py = r * 16 + 8;
        for (int f = std::max(0, t0); f <= std::min(pf - 1, t1); f += 2)
          for (int b = 0; b < nb; b++) {
            const BossObj& bo = w.bosses[size_t(b)];
            if (!bo.hasBox) continue;
            int x = bt[size_t(2 * (nb * f + b))], y = bt[size_t(2 * (nb * f + b) + 1)];
            if (x + bo.box[0] - 8 < px && px <= x + bo.box[1] + 7 && y + bo.box[2] - 8 < py && py <= y + bo.box[3] + 7)
              return 1.0;
          }
        return 0.0;
      };
      std::vector<uint8_t> inBlast(size_t(w.cols * w.rows));
      struct X { int boom, d, k, wait; };
      std::vector<X> xs;
      for (int k = 0; k < w.cols * w.rows; k++) {
        int d = dist[size_t(k)];
        if (d < 0 || d > kMaxD) continue;
        int xc = k % w.cols, xr = k / w.cols;
        if (!canPlaceOn(w.at(xc, xr)) || w.bombAt(xc, xr)) continue;
        std::fill(inBlast.begin(), inBlast.end(), 0);
        for (const Cell& x : blastCells(w, xc, xr, w.p.fire, w.p.pierce)) inBlast[size_t(g.idx(x.c, x.r))] = 1;
        for (int wait = 0; wait <= kMaxWait; wait += 8) {
          int tp = wait + int(fpc_ * d + 0.5) + 2, boom = tp + fuse;
          if (boom + flame / 2 >= pf) break;
          bool eaten = false;
          for (int f = tp; f <= boom && !eaten; f++)
            for (int q : sense[size_t(f)]) eaten |= q == k;
          if (eaten) continue;
          int hits = 0;
          for (int f = boom; f < boom + flame * 3 / 4; f++) {
            bool h = false;
            for (int q : sense[size_t(f)]) h |= inBlast[size_t(q)] != 0;
            hits += h;
          }
          if (hits >= 6) {
            xs.push_back({boom, d, k, wait});
            break;
          }
        }
      }
      std::sort(xs.begin(), xs.end(), [](const X& a, const X& b) { return a.boom != b.boom ? a.boom < b.boom : a.d < b.d; });
      if (xs.size() > 6) xs.resize(6);
      Risk risk = boxed;
      // (two covers each: the boss strikes back where P1 hides, so the
      // second is often the one that lives)
      std::vector<std::vector<Cand>> per;
      // P1's bombs already out: the run must show their hits too, and this
      // one's. A run is played only until P1 is in cover; the next bomb
      // is planned while this one ticks (the run lived through its blast).
      int out = 0;
      for (const Bomb& b : w.bombs) out += !b.enemyOwned && b.fuse > 0;
      for (const X& x : xs) {
        per.emplace_back();
        bombCands(x.k % w.cols, x.k / w.cols, Cand::BossHit, tname + " (wait " + std::to_string(x.wait) + ")", 2, per.back(),
                  risk, x.wait);
        for (Cand& c : per.back()) {
          c.req.pre.assign(size_t(x.wait), 0);
          c.req.frames += x.wait;
          int d1 = wpSteps(c.req.path), d2 = wpSteps(c.req.path2);
          c.commit = x.wait + kWalkFPT * (d1 + d2) + 2 * kSettle;
          c.drop = 1 + out;
          if (out) c.what += " (+" + std::to_string(out) + " out)";
        }
      }
      for (size_t y = 0; y < 2; y++)
        for (auto& p : per)
          if (y < p.size()) cs.push_back(p[y]);
      if (senseStale_) cs.clear();  // (it senses nothing now: just get in place)
      if (!cs.empty() && tryCands(cs, live, o, 10)) return;
      // No hit from here: get in place for the next one. The cell whose
      // blast holds a sense point on the most frames of the boss's path
      // (out of the contact boxes), a few cells' walk at a time.
      cs.clear();
      std::vector<std::pair<int, int>> spots;  // (-score, idx)
      for (int k = 0; k < w.cols * w.rows; k++) {
        int d = dist[size_t(k)];
        if (d < 0 || d > kMaxD) continue;
        int xc = k % w.cols, xr = k / w.cols;
        if (!canPlaceOn(w.at(xc, xr)) || boxed(xc, xr, 0, pf - 1) > 0) continue;
        std::fill(inBlast.begin(), inBlast.end(), 0);
        for (const Cell& x : blastCells(w, xc, xr, w.p.fire, w.p.pierce)) inBlast[size_t(g.idx(x.c, x.r))] = 1;
        int score = 0;
        for (int f = 0; f < pf; f += 2) {
          bool h = false, on = false;
          for (int q : sense[size_t(f)]) h |= inBlast[size_t(q)] != 0, on |= q == k;
          score += h && !on;
        }
        if (score > 0) spots.push_back({-(score * 4 - d), k});
      }
      std::sort(spots.begin(), spots.end());
      for (size_t i = 0; i < spots.size() && cs.size() < 3; i++) {
        int k = spots[i].second;
        auto p = g.pathTo(prev, g.pc, g.pr, k % w.cols, k / w.cols);  // (here already: wait)
        g.cut(p, 5);
        Cand c = walkCand(p.back() % w.cols, p.back() / w.cols, Cand::Live, tname + " (get in place)");
        c.commit = p.size() == 1 ? 30 : hasJump(c.req.path) ? c.commit : std::min(c.commit, 60);
        cs.push_back(c);
      }
      if (!cs.empty() && tryCands(cs, live, o, 3)) return;
      survive(w, live, o, tname.c_str());
      return;
    }
    case TaskKind::KillEnemy: {
      killSlot_ = task.ref >= 0 && w.enemies[size_t(task.ref)].slot >= 4 ? w.enemies[size_t(task.ref)].slot - 4 : -1;
      // The remote with a bomb to spare: lay a trap where enemies are likely
      // to walk (the model's odds an enemy enters its blast within kTrapH),
      // go to cover and leave it; frame() sets it off when one is in. Traps
      // out and none to spare: wait for them a while before the usual kill
      // (its B presses would spend them).
      const int kMaxTraps = 3, kTrapH = 600;
      const double kGapBonus = 0.25;
      if (w.p.remote && w.p.bombs >= 2 && mine < kMaxTraps) {
        EnemyModel tm(w, kTrapH, uint32_t(w.frame) * 2654435761u + 3, eScale_);
        auto runs = tm.roll(nullptr, g.pc, g.pr, 16);
        std::vector<std::pair<double, int>> spots;  // (-odds, idx)
        std::vector<uint8_t> in(size_t(w.cols * w.rows));
        for (int k = 0; k < w.cols * w.rows; k++) {
          int d = dist[size_t(k)];
          if (d < 0 || d > 8) continue;
          int xc = k % w.cols, xr = k / w.cols;
          if (!canPlaceOn(w.at(xc, xr)) || w.bombAt(xc, xr) || spoils(xc, xr)) continue;
          std::fill(in.begin(), in.end(), 0);
          for (const Cell& x : blastCells(w, xc, xr, w.p.fire, w.p.pierce)) in[size_t(g.idx(x.c, x.r))] = 1;
          int tp = int(fpc_ * d) + 2 + 30;  // (placed, P1 gone)
          int hits = 0, n = 0;
          for (const EnemyRun& er : runs) n = std::max(n, int(er.rolls.size()));
          for (int i = 0; i < n; i++) {
            bool h = false;
            for (const EnemyRun& er : runs)
              if (i < int(er.rolls.size()))
                for (const auto& sp : er.rolls[size_t(i)])
                  if (sp.t1 >= tp && w.inside(sp.c, sp.r) && in[size_t(g.idx(sp.c, sp.r))]) h = true;
            hits += h;
          }
          // A gap a block left (it may grow back; a bomb on it keeps it
          // open, and enemies come through it) ranks higher.
          double gap = softSeen_.count(k) ? kGapBonus : 0;
          if (n && hits * 10 >= n * 4) spots.push_back({-double(hits) / n - gap + d / 100.0, k});
        }
        std::sort(spots.begin(), spots.end());
        for (size_t i = 0; i < spots.size() && i < 3; i++) {
          size_t n0 = cs.size();
          int k = spots[i].second;
          bombCands(k % w.cols, k / w.cols, Cand::Live, "trap for " + tname, 2, cs, ambient());
          for (size_t j = n0; j < cs.size(); j++) {
            Cand& c = cs[j];
            int d1 = wpSteps(c.req.path), d2 = wpSteps(c.req.path2);
            c.req.detEnd = 0;
            c.afterA = c.afterB = 0;
            c.commit = kWalkFPT * (d1 + d2) + 2 * kSettle;
            c.req.frames = c.commit + kTail;
            char b[48];
            std::snprintf(b, sizeof b, " (odds %.2f%s)", -spots[i].first + double(d1) / 100.0 - (softSeen_.count(k) ? kGapBonus : 0),
                          softSeen_.count(k) ? ", on a gap" : "");
            c.what += b;
          }
        }
        if (!cs.empty() && tryCands(cs, live, o, 6)) {
          trapSince_ = w.frame;
          return;
        }
        cs.clear();
      }
      if (w.p.remote && mine > 0 && w.p.bombs < 2 && w.frame - trapSince_ < kTrapWait)
        return survive(w, live, o, "traps out: waiting for an enemy");
      // The enemy model (enemies.hpp) gives the odds a bomb at x kills:
      // reached in d cells after standing `wait` frames, it goes off at
      // wait + fpc*d + 2 + fuse. The best few cells go to the oracle, each
      // with covers enemies are unlikely to come by, and with a few waits:
      // the game's random choices follow P1's moves, so another wait is
      // another draw.
      const Enemy& e = w.enemies[size_t(task.ref)];
      const int kMaxD = 10, kRolls = 16;
      const int horizon = int(fpc_ * kMaxD) + 48 + 2 + int((kNewFuse + kFlameFrames) * eScale_) + kTail + 40;
      {
        char eb[64];
        std::snprintf(eb, sizeof eb, "lag: x%.2f real frames per logic frame", eScale_);
        if (verbose) log(eb);
      }
      EnemyModel em(w, horizon, uint32_t(w.frame) * 2654435761u + 1, eScale_);
      {
        // How long each stopped enemy stays put: one run of the real game
        // with P1 holding still (rests on timers the world view lacks).
        SimReq probe;
        probe.path = g.waypoints({g.idx(g.pc, g.pr)});
        probe.frames = horizon;
        probe.wantEnemies = true;
        std::vector<SimOut> pout;
        o.run({probe}, pout);
        sims_++;
        if (!pout.empty() && !pout[0].etrace.empty()) {
          const auto& et = pout[0].etrace;
          const size_t nf = et.size() / kEnemySlots;
          em.hold.assign(w.enemies.size(), -1);
          for (size_t i = 0; i < w.enemies.size(); i++) {
            int sl = w.enemies[i].slot - 4;
            if (sl < 0 || sl >= kEnemySlots || et[size_t(sl)] < 0) continue;
            size_t f = 1;
            while (f < nf && et[f * kEnemySlots + size_t(sl)] == et[size_t(sl)]) f++;
            em.hold[i] = int(f);
          }
        }
      }
      // One option: bomb at x after `wait`, the cover picked by the odds
      // of meeting an enemy there; its odds are for that exact plan (the
      // chasers head for P1 in that cover). score = kill odds x the odds no
      // enemy comes by the cover.
      struct X { double p, score; int t; Cand c; };
      auto option = [&](int xc, int xr, int d, int wait, X& out) {
        int tp = wait + int(fpc_ * d + 0.5) + 2;
        int boom = tp + int(kNewFuse * eScale_ + 0.5);
        ModelBomb mb{xc, xr, tp, boom, w.p.fire, w.p.pierce};
        auto trackTo = [&](const Cand& c) {
          int d2 = int(c.req.path2.size() / 2) - 1;
          return std::vector<P1At>{{0, g.pc, g.pr}, {tp - 2, xc, xr}, {tp + int(fpc_ * d2), c.endC, c.endR}};
        };
        std::vector<Cand> first;
        bombCands(xc, xr, Cand::Kill, "", 1, first);
        if (first.empty()) return false;
        auto runs = em.roll(&mb, trackTo(first[0]), kRolls);
        Risk risk = [&runs](int c, int r, int t0, int t1) { return EnemyModel::nearOdds(runs, c, r, t0, t1); };
        std::vector<Cand> pick;
        bombCands(xc, xr, Cand::Kill, "", 1, pick, risk, wait);
        if (pick.empty()) return false;
        if (pick[0].endC != first[0].endC || pick[0].endR != first[0].endR) runs = em.roll(&mb, trackTo(pick[0]), kRolls);
        int d2 = int(pick[0].req.path2.size() / 2) - 1, ty = tp + int(fpc_ * d2);
        double p = EnemyModel::killOdds(runs);
        double safe = 1 - EnemyModel::nearOdds(runs, pick[0].endC, pick[0].endR, ty - 8, boom + kFlameFrames + kTail);
        out = {p, p * safe, boom, pick[0]};
        char what[120];
        std::snprintf(what, sizeof what, "%s (odds %.2f, cover %.2f, wait %d): bomb at (%d,%d), cover (%d,%d)", tname.c_str(), p,
                      safe, wait, xc, xr, out.c.endC, out.c.endR);
        out.c.what = what;
        out.c.req.pre.assign(size_t(wait), 0);
        out.c.commit += wait;
        out.c.req.frames += wait;
        return true;
      };
      std::vector<X> xs;
      for (int k = 0; k < w.cols * w.rows; k++) {
        int d = dist[size_t(k)];
        if (d < 0 || d > kMaxD) continue;
        int xc = k % w.cols, xr = k / w.cols;
        if (!canPlaceOn(w.at(xc, xr)) || w.bombAt(xc, xr)) continue;
        bool any = false;  // (an enemy that could get into range at all)
        for (const Enemy& q : w.enemies)
          if (EnemyModel::modelled(q) && std::abs(q.col() - xc) + std::abs(q.row() - xr) <= w.p.fire + 8) any = true;
        if (!any) continue;
        X best{-1, -1, 0, {}};
        for (int wait : {0, 24, 48}) {
          X x;
          if (option(xc, xr, d, wait, x) && x.score - wait / 400.0 > best.score - (best.p < 0 ? 0 : (best.c.req.pre.size() / 400.0)))
            best = x;
        }
        if (best.p >= 0.15 && best.score >= 0.1) xs.push_back(best);
      }
      std::sort(xs.begin(), xs.end(), [](const X& a, const X& b) { return a.score - a.t / 1000.0 > b.score - b.t / 1000.0; });
      if (xs.size() > 4) xs.resize(4);
      std::vector<std::vector<Cand>> per;
      for (const X& x : xs) per.push_back({x.c});
      if (verbose)  // (the real enemy tracks, to hold the model to)
        for (auto& pc : per)
          for (Cand& c : pc) c.req.wantEnemies = true;
      for (int extra : {0, 8, 16})
        for (auto& pc : per)
          for (const Cand& c0 : pc) {
            Cand c = c0;
            c.req.pre.resize(c.req.pre.size() + size_t(extra), 0);
            c.commit += extra;
            c.req.frames += extra;
            if (extra) c.what += " +" + std::to_string(extra);
            cs.push_back(c);
          }
      // (last: the cells by the target or the cell it heads for)
      std::vector<std::pair<int, int>> near;
      for (int k = 0; k < w.cols * w.rows; k++) {
        if (dist[size_t(k)] < 0) continue;
        int xc = k % w.cols, xr = k / w.cols;
        if (std::abs(xc - e.col()) + std::abs(xr - e.row()) > w.p.fire + 3) continue;
        if (covers(w, xc, xr, e.col(), e.row()) || covers(w, xc, xr, e.tc, e.tr)) near.push_back({dist[size_t(k)], k});
      }
      std::sort(near.begin(), near.end());
      for (size_t i = 0, got = 0; i < near.size() && got < 2; i++) {  // (two cells that give a move)
        size_t n0 = cs.size();
        bombCands(near[i].second % w.cols, near[i].second / w.cols, Cand::Kill, tname, 1, cs);
        got += cs.size() > n0;
      }
      if (tryCands(cs, live, o, 14)) return;
      // No bomb gets it yet: close in a few cells, then look again.
      cs.clear();
      int bestK = -1, bestD = INT_MAX;
      for (int k = 0; k < w.cols * w.rows; k++) {
        if (dist[size_t(k)] <= 0) continue;
        int d = std::abs(k % w.cols - e.col()) + std::abs(k / w.cols - e.row());
        if (d >= 2 && d < bestD) bestD = d, bestK = k;
      }
      if (bestK >= 0) {
        auto p = g.pathTo(prev, g.pc, g.pr, bestK % w.cols, bestK / w.cols);
        g.cut(p, 4);
        int k = p.back();
        Cand c = walkCand(k % w.cols, k / w.cols, Cand::Live, tname + " (closing in)");
        if (!hasJump(c.req.path)) c.commit = std::min(c.commit, 40);  // (never end one in the air)
        cs.push_back(c);
      }
      if (!cs.empty() && tryCands(cs, live, o, 4)) return;
      return survive(w, live, o, tname.c_str());
    }
  }
  if (cs.empty() || !tryCands(cs, live, o, 12)) {
    itemFailed();
    survive(w, live, o, tname.c_str());
  }
}

static_assert(kEnemySlots == AG_ESLOTS, "enemy slots");

}  // namespace sbm2

// --- C glue ---
namespace {
struct CAgent {
  sbm2::Agent agent;
  std::string log, world;
  explicit CAgent(const sbm2::Goal& g) : agent(g) {}
};
struct COracle : sbm2::Oracle {
  AgSimFn fn;
  void* ctx;
  void run(const std::vector<sbm2::SimReq>& reqs, std::vector<sbm2::SimOut>& out) override {
    size_t n = reqs.size();
    std::vector<AgSimReq> rq(n);
    std::vector<AgSimRes> rs(n);
    out.assign(n, {});
    for (size_t i = 0; i < n; i++) {
      const sbm2::SimReq& r = reqs[i];
      rq[i] = {r.pre.data(), int(r.pre.size()), r.path.data(), int(r.path.size() / 2), r.post.data(),
               int(r.post.size()), r.path2.data(), int(r.path2.size() / 2), r.detEnd, r.frames, r.wantEnemies,
               r.bossBase.data(), int(r.bossBase.size()), r.wantBoss};
      out[i].pads.assign(size_t(r.frames + 1), 0);
      out[i].trace.assign(size_t(2 * (r.frames + 1)), 0);
      if (r.wantEnemies) out[i].etrace.assign(size_t(AG_ESLOTS * (r.frames + 1)), -1);
      rs[i] = {};
      rs[i].pads = out[i].pads.data();
      rs[i].trace = out[i].trace.data();
      rs[i].etrace = r.wantEnemies ? out[i].etrace.data() : nullptr;
      if (r.wantBoss) out[i].btrace.assign(size_t(2 * r.bossBase.size() * size_t(r.frames + 1)), 0);
      rs[i].btrace = r.wantBoss && !r.bossBase.empty() ? out[i].btrace.data() : nullptr;
      rs[i].cap = r.frames + 1;
    }
    fn(ctx, rq.data(), rs.data(), int(n));
    for (size_t i = 0; i < n; i++) {
      out[i].dead = rs[i].dead;
      out[i].enemies = rs[i].enemies;
      out[i].enemyMask = rs[i].enemyMask;
      out[i].hp = rs[i].hp;
      out[i].px = rs[i].px;
      out[i].py = rs[i].py;
      out[i].ph = rs[i].ph;
      out[i].mapSig = rs[i].mapSig;
      out[i].mapChanged = rs[i].mapChanged;
      out[i].bossHp = rs[i].bossHp;
    }
  }
};
}  // namespace

extern "C" {
void* sbm2_agent_new(const char* goal) {
  sbm2::Goal g;
  if (!sbm2::Goal::parse(goal && *goal ? goal : "exit", g)) return nullptr;
  CAgent* c = new CAgent(g);
  const char* v = std::getenv("SBM2_AGVERBOSE");
  c->agent.verbose = v && *v && *v != '0';
  return c;
}
void sbm2_agent_free(void* a) { delete static_cast<CAgent*>(a); }
uint16_t sbm2_agent_frame(void* a, const uint8_t* wram, int frame, const char* extra, AgSimFn sim, void* ctx) {
  COracle o;
  o.fn = sim;
  o.ctx = ctx;
  return static_cast<CAgent*>(a)->agent.frame(wram, frame, o, extra);
}
const char* sbm2_agent_log(void* a) {
  CAgent* c = static_cast<CAgent*>(a);
  c->log = c->agent.takeLog();
  return c->log.c_str();
}
const char* sbm2_capture(const uint8_t* wram, int frame) {
  static std::string text;
  text = sbm2::encodeWorld(sbm2::captureWorld(wram, frame));
  return text.c_str();
}
const char* sbm2_agent_world(void* a) {
  CAgent* c = static_cast<CAgent*>(a);
  c->world = c->agent.worldText();
  return c->world.c_str();
}
}
