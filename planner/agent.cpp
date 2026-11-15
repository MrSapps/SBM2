#include "agent.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <deque>
#include <queue>

#include "agent_api.h"
#include "blast.hpp"
#include "capture.hpp"

namespace sbm2 {

namespace {

constexpr int kWalkFPT = 22;     // frames per cell budgeted for a walk (P1 ~16-20)
constexpr int kNewFuse = 150;    // a fresh bomb's fuse (bot.lua NEW_FUSE)
constexpr int kTail = 60;        // frames past the commit the oracle must also live
constexpr int kSettle = 20;
const int kDirs[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};

int rd16(const uint8_t* w, int a) { return w[a] | w[a + 1] << 8; }
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
  // BFS from (c0,r0): dist (-1 unreachable) and prev cell index.
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
      for (auto& d : kDirs) {
        int nc = c + d[0], nr = r + d[1];
        if (!open(nc, nr, c0, r0) || dist[size_t(idx(nc, nr))] >= 0) continue;
        dist[size_t(idx(nc, nr))] = dist[size_t(k)] + 1;
        prev[size_t(idx(nc, nr))] = k;
        q.push_back(idx(nc, nr));
      }
    }
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
    for (int k : cells) {
      wp.push_back(int16_t((k % w.cols) * 16 + 8));
      wp.push_back(int16_t((k / w.cols) * 16 + 8));
    }
    return wp;
  }
  // Route to (tc,tr) where soft blocks cost extra (bombed on the way): the
  // cells, start first.
  std::vector<int> softRoute(int tc, int tr) const {
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
      for (auto& dd : kDirs) {
        int nc = c + dd[0], nr = r + dd[1];
        if (!w.inside(nc, nr)) continue;
        char t = w.at(nc, nr);
        int cost;
        if (breakable(t)) cost = 10;
        else if (open(nc, nr, pc, pr)) cost = 1;
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
  l.locked = rd16(w, 0x8052) & 1;
  l.px = uint16_t(sx16(rd16(w, 0x8010)) + sx16(rd16(w, 0x5F01)));
  l.py = uint16_t(sx16(rd16(w, 0x8018)) + sx16(rd16(w, 0x5EFB)));
  agent_enemies(w, &l.enemies, &l.hp);
  l.mapSig = agent_map_sig(w);
  return l;
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

uint16_t Agent::frame(const uint8_t* wram, int frame, Oracle& o) {
  frame_ = frame;
  Live live = readLive(wram);
  if (!live.inStage || live.locked) {
    if (committed() && !live.inStage) pads_.clear();
    if (committed() && live.locked) {  // (a lock the oracle saw coming: play on)
      if (at_ < int(pads_.size())) return pads_[size_t(at_++)];
    }
    return 0;
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
  decide(w, live, o);
  return committed() ? pads_[size_t(at_++)] : 0;
}

bool Agent::tryCands(std::vector<Cand>& cs, const Live& live, Oracle& o, size_t maxRuns) {
  if (cs.size() > maxRuns) cs.resize(maxRuns);
  const size_t kBatch = 4;
  for (size_t b = 0; b < cs.size(); b += kBatch) {
    std::vector<SimReq> reqs;
    for (size_t i = b; i < std::min(cs.size(), b + kBatch); i++) reqs.push_back(cs[i].req);
    std::vector<SimOut> out;
    o.run(reqs, out);
    sims_ += int(reqs.size());
    for (size_t i = 0; i < out.size(); i++) {
      const Cand& c = cs[b + i];
      const SimOut& r = out[i];
      bool ok = false;
      bool at = c.endC < 0 || (std::abs(r.px - (c.endC * 16 + 8)) <= 2 && std::abs(r.py - (c.endR * 16 + 8)) <= 2);
      switch (c.want) {
        case Cand::Exit: ok = r.dead == -2; break;
        case Cand::Live: ok = r.dead == -1 && at; break;
        case Cand::Change: ok = r.dead == -1 && at && r.mapSig != live.mapSig; break;
        case Cand::Kill: ok = r.dead == -1 && at && (r.enemies < live.enemies || r.hp < live.hp); break;
      }
      if (ok) {
        pads_ = r.pads;
        trace_ = r.trace;
        int n = c.want == Cand::Exit ? int(r.pads.size()) : std::min<int>(c.commit, int(r.pads.size()));
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
  return false;
}

// Nothing for the task lives (or there is no task): move to a cell nearby
// that lives, else the one that lives longest; play a little, decide again.
void Agent::survive(const World& w, const Live& live, Oracle& o, const char* why) {
  Grid g(w);
  std::vector<int> dist, prev;
  g.bfs(g.pc, g.pr, dist, prev);
  std::vector<std::pair<int, int>> cells;  // (dist, idx)
  for (int k = 0; k < w.cols * w.rows; k++)
    if (dist[size_t(k)] >= 0 && dist[size_t(k)] <= 6) cells.push_back({dist[size_t(k)], k});
  std::sort(cells.begin(), cells.end());
  // Cells out of every known blast first.
  DangerMap dm(w);
  std::stable_sort(cells.begin(), cells.end(), [&](auto& a, auto& b) {
    bool da = dm.deadly(a.second % w.cols, a.second / w.cols, 0, 400);
    bool db = dm.deadly(b.second % w.cols, b.second / w.cols, 0, 400);
    return da < db;
  });
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
  for (size_t b = 0; b < reqs.size(); b += 4) {
    std::vector<SimReq> part(reqs.begin() + long(b), reqs.begin() + long(std::min(reqs.size(), b + 4)));
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
  int n = bestDead == INT_MAX ? 24 : std::max(1, std::min(8, bestDead / 2));
  pads_.resize(std::min(pads_.size(), size_t(n)));
  trace_.resize(pads_.size() * 2);
  at_ = 0;
  what_ = "survive";
  std::snprintf(b, sizeof b, "survive (%s): toward (%d,%d), %s, play %d", why, k % w.cols, k / w.cols,
                bestDead == INT_MAX ? "lives" : ("dies at +" + std::to_string(bestDead)).c_str(), int(pads_.size()));
  log(b);
  (void)live;
}

void Agent::decide(const World& w, const Live& live, Oracle& o) {
  decisions_++;
  frame_ = w.frame;
  worldText_ = encodeWorld(w);
  pads_.clear();
  at_ = 0;
  Grid g(w);
  if (!w.inside(g.pc, g.pr)) return survive(w, live, o, "P1 off the map");
  std::vector<int> dist, prev;
  g.bfs(g.pc, g.pr, dist, prev);

  Plan gp = plan(w, goal_);
  if (!gp.ok || gp.steps.empty()) {
    Goal more;
    more.blocks = true;
    gp = plan(w, more);
    if (!gp.ok || gp.steps.empty()) return survive(w, live, o, "no plan");
  }
  const Step& st = gp.steps[0];
  const Task& task = gp.tasks[size_t(st.task)];
  char tb[96];
  std::snprintf(tb, sizeof tb, "%s %d,%d", taskName(task.kind), task.c, task.r);
  std::string tname = tb;

  // Bomb at x, then cover: candidates for every safe cell near x.
  auto bombCands = [&](int xc, int xr, Cand::Want want, const std::string& what, int maxY, std::vector<Cand>& out) {
    if (dist[size_t(g.idx(xc, xr))] < 0 || !canPlaceOn(w.at(xc, xr)) || w.bombAt(xc, xr)) return;
    auto p1 = g.pathTo(prev, g.pc, g.pr, xc, xr);
    int t1 = kWalkFPT * int(p1.size() - 1) + kSettle;
    std::vector<int> d2, prev2;
    g.bfs(xc, xr, d2, prev2);
    DangerMap dm(w);
    dm.addBomb(xc, xr, w.p.fire, w.p.pierce, t1 + kNewFuse);
    std::vector<std::pair<int, int>> ys;
    for (int k = 0; k < w.cols * w.rows; k++) {
      if (d2[size_t(k)] <= 0) continue;
      int yc = k % w.cols, yr = k / w.cols;
      int t2 = t1 + kWalkFPT * d2[size_t(k)] + kSettle;
      if (dm.deadly(yc, yr, t2, t1 + kNewFuse + kFlameFrames + kTail)) continue;
      ys.push_back({d2[size_t(k)], k});
    }
    std::sort(ys.begin(), ys.end());
    int n = 0;
    for (auto& [d, k] : ys) {
      if (n++ >= maxY) break;
      Cand c;
      c.req.path = g.waypoints(p1);
      c.req.post = {kPadA, kPadA};
      c.req.path2 = g.waypoints(g.pathTo(prev2, xc, xr, k % w.cols, k / w.cols));
      c.req.detEnd = w.p.remote;
      int t2 = kWalkFPT * d + kSettle;
      int wait = w.p.remote ? 30 + kFlameFrames : kNewFuse + kFlameFrames;
      c.commit = t1 + 2 + t2 + wait;
      c.req.frames = c.commit + kTail;
      c.endC = k % w.cols;
      c.endR = k / w.cols;
      c.want = want;
      char b[160];
      std::snprintf(b, sizeof b, "%s: bomb at (%d,%d), cover (%d,%d)", what.c_str(), xc, xr, c.endC, c.endR);
      c.what = b;
      out.push_back(c);
    }
  };
  auto walkCand = [&](int tc, int tr, Cand::Want want, const std::string& what) {
    Cand c;
    auto p = g.pathTo(prev, g.pc, g.pr, tc, tr);
    c.req.path = g.waypoints(p);
    c.commit = kWalkFPT * int(p.size() - 1) + kSettle;
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
    auto route = g.softRoute(sc, sr);
    for (size_t i = 1; i < route.size(); i++) {
      int k = route[i];
      if (!breakable(w.at(k % w.cols, k / w.cols))) continue;
      int bk = route[i - 1];
      bombCands(bk % w.cols, bk / w.cols, Cand::Change, "clear the way to " + tname, 6, cs);
      break;
    }
    if (cs.empty() || !tryCands(cs, live, o, 12)) return survive(w, live, o, ("way to " + tname).c_str());
    return;
  }
  switch (task.kind) {
    case TaskKind::EnterGate:
      cs.push_back(walkCand(task.c, task.r, Cand::Exit, tname));
      break;
    case TaskKind::PickItem:
      cs.push_back(walkCand(task.c, task.r, Cand::Change, tname));
      break;
    case TaskKind::BombSoft:
    case TaskKind::TogglePanel:
    case TaskKind::LightCrystal:
      bombCands(sc, sr, Cand::Change, tname, 6, cs);
      break;
    case TaskKind::KillEnemy: {
      // Cells whose blast covers the enemy's cell or the cell it heads for,
      // nearest first; two covers each.
      const Enemy& e = w.enemies[size_t(task.ref)];
      std::vector<std::pair<int, int>> xs;
      for (int k = 0; k < w.cols * w.rows; k++) {
        if (dist[size_t(k)] < 0) continue;
        int xc = k % w.cols, xr = k / w.cols;
        if (std::abs(xc - e.col()) + std::abs(xr - e.row()) > w.p.fire + 3) continue;
        if (covers(w, xc, xr, e.col(), e.row()) || covers(w, xc, xr, e.tc, e.tr)) xs.push_back({dist[size_t(k)], k});
      }
      std::sort(xs.begin(), xs.end());
      for (size_t i = 0; i < xs.size() && i < 6; i++)
        bombCands(xs[i].second % w.cols, xs[i].second / w.cols, Cand::Kill, tname, 2, cs);
      if (tryCands(cs, live, o, 12)) return;
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
        if (p.size() > 4) p.resize(4);
        int k = p.back();
        Cand c = walkCand(k % w.cols, k / w.cols, Cand::Live, tname + " (closing in)");
        c.commit = std::min(c.commit, 40);
        cs.push_back(c);
      }
      if (!cs.empty() && tryCands(cs, live, o, 4)) return;
      return survive(w, live, o, tname.c_str());
    }
  }
  if (cs.empty() || !tryCands(cs, live, o, 12)) survive(w, live, o, tname.c_str());
}

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
               int(r.post.size()), r.path2.data(), int(r.path2.size() / 2), r.detEnd, r.frames};
      out[i].pads.assign(size_t(r.frames + 1), 0);
      out[i].trace.assign(size_t(2 * (r.frames + 1)), 0);
      rs[i] = {};
      rs[i].pads = out[i].pads.data();
      rs[i].trace = out[i].trace.data();
      rs[i].cap = r.frames + 1;
    }
    fn(ctx, rq.data(), rs.data(), int(n));
    for (size_t i = 0; i < n; i++) {
      out[i].dead = rs[i].dead;
      out[i].enemies = rs[i].enemies;
      out[i].hp = rs[i].hp;
      out[i].px = rs[i].px;
      out[i].py = rs[i].py;
      out[i].ph = rs[i].ph;
      out[i].mapSig = rs[i].mapSig;
    }
  }
};
}  // namespace

extern "C" {
void* sbm2_agent_new(const char* goal) {
  sbm2::Goal g;
  if (!sbm2::Goal::parse(goal && *goal ? goal : "exit", g)) return nullptr;
  return new CAgent(g);
}
void sbm2_agent_free(void* a) { delete static_cast<CAgent*>(a); }
uint16_t sbm2_agent_frame(void* a, const uint8_t* wram, int frame, AgSimFn sim, void* ctx) {
  COracle o;
  o.fn = sim;
  o.ctx = ctx;
  return static_cast<CAgent*>(a)->agent.frame(wram, frame, o);
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
