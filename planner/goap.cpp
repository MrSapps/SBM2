#include "goap.hpp"

#include "blast.hpp"

#include <algorithm>
#include <climits>
#include <deque>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_map>

namespace sbm2 {

namespace {
constexpr int kFPT = 20;          // frames per cell walked (bot.lua FPT)
constexpr int kBombCost = 40;     // place a bomb and step out of its line
constexpr int kEnemyCost = 120;   // line up on a moving enemy
constexpr int kSoftCross = 190;   // bomb a soft block in the way: fuse 150 + flames
constexpr int kBurnCost = 3000;   // a blast burns an item the goal wants: only if nothing else does
const int kDirs[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};

int baseCost(TaskKind k) {
  switch (k) {
    case TaskKind::KillEnemy:
    case TaskKind::HurtBoss: return kEnemyCost;
    case TaskKind::BombSoft:
    case TaskKind::TogglePanel:
    case TaskKind::LightCrystal:
    case TaskKind::PushBlock:
    case TaskKind::PressButton:
    case TaskKind::SpawnPlatform: return kBombCost;
    default: return 0;
  }
}

bool isBombTask(TaskKind k) { return k != TaskKind::PickItem && k != TaskKind::EnterGate; }

// Cells a blast passes on through (it stops at stairs, $4B traps, items).
bool blastPasses(char c) { return walkableClass(c) && std::string("^Ti").find(c) == std::string::npos; }

struct Node {
  int at;                     // cell index of P1
  std::vector<uint8_t> done;  // per task
  int g, h, parent, step;     // step: task index taken to get here (-1 root)
  int standC, standR, stepCost;
  std::vector<int> via;  // soft blocks bombed on the way (cell indices)
};

class Search {
 public:
  Search(const World& w, const Goal& goal, const PlanOptions& o) : w_(w), goal_(goal), o_(o) {
    tasks_ = worldTasks(w);
    softAt_.assign(size_t(w.cols * w.rows), -1);
    staticAt_.assign(size_t(w.cols * w.rows), {});
    for (size_t i = 0; i < tasks_.size(); i++) {
      const Task& t = tasks_[i];
      if (t.kind == TaskKind::BombSoft || t.kind == TaskKind::TogglePanel || t.kind == TaskKind::LightCrystal ||
          t.kind == TaskKind::PressButton || t.kind == TaskKind::SpawnPlatform)
        staticAt_[size_t(t.r * w.cols + t.c)].push_back(int(i));
      if (t.kind == TaskKind::PushBlock) {
        blockTasks_[t.r * w.cols + t.c].push_back(int(i));
        holeTasks_[(t.r + t.dr) * w.cols + t.c + t.dc].push_back(int(i));
      }
      if (t.kind == TaskKind::PressButton) buttons_.push_back(int(i));
      if (t.kind == TaskKind::SpawnPlatform) spawners_.push_back(int(i));
      if (t.kind == TaskKind::BombSoft) softAt_[size_t(t.r * w.cols + t.c)] = int(i);
      if (t.kind == TaskKind::PickItem) itemAt_[t.r * w.cols + t.c].push_back(int(i));
      if (t.kind == TaskKind::TogglePanel) hasPanels_ = true;
    }
    for (const Panel& p : w.panels)
      if (!p.done) hasPanels_ = true;
    std::vector<uint8_t> pd = panelsDone(w);
    for (size_t i = 0; i < w.panels.size(); i++)
      if (w.panels[i].mode == 1 && pd[i]) donePanel_.insert(w.panels[i].r * w.cols + w.panels[i].c);
  }

  Plan run() {
    Plan out;
    out.tasks = tasks_;
    Node root{cellIdx(w_.p.col(), w_.p.row()), std::vector<uint8_t>(tasks_.size(), 0), 0, 0, -1, -1, 0, 0, 0, {}};
    root.h = heuristic(root);
    nodes_.push_back(root);
    auto cmp = [&](int a, int b) {
      int fa = nodes_[a].g + nodes_[a].h, fb = nodes_[b].g + nodes_[b].h;
      return fa != fb ? fa > fb : nodes_[a].h > nodes_[b].h;
    };
    std::priority_queue<int, std::vector<int>, decltype(cmp)> open(cmp);
    open.push(0);
    std::unordered_map<std::string, int> best;
    best[key(root)] = 0;
    int bestPartial = 0;
    while (!open.empty()) {
      int n = open.top();
      open.pop();
      if (nodes_[n].g > best[key(nodes_[n])]) continue;
      if (satisfied(nodes_[n])) return finish(out, n, true);
      if (++out.expanded > o_.maxNodes) {
        out.expanded--;
        return greedy(out, bestPartial);
      }
      const Node& nd = nodes_[n];
      if (nd.h < nodes_[bestPartial].h || (nd.h == nodes_[bestPartial].h && nd.g < nodes_[bestPartial].g))
        bestPartial = n;
      for (Node& s : successors(n)) {
        std::string k = key(s);
        auto it = best.find(k);
        if (it != best.end() && it->second <= s.g) continue;
        best[k] = s.g;
        nodes_.push_back(std::move(s));
        open.push(int(nodes_.size() - 1));
      }
    }
    out.why = blocked();
    return out;
  }

 private:
  int cellIdx(int c, int r) const { return r * w_.cols + c; }

  std::string key(const Node& n) const {
    std::string k(reinterpret_cast<const char*>(&n.at), sizeof n.at);
    k.append(n.done.begin(), n.done.end());
    return k;
  }

  bool allDone(const Node& n, TaskKind k) const {
    for (size_t i = 0; i < tasks_.size(); i++)
      if (tasks_[i].kind == k && !n.done[i]) return false;
    return true;
  }

  // The exit opens when nothing blocks it (UpdateExitBlocked C1:531D) and
  // every gold panel is lit (CheckGoldPanels C1:2335 sets Exit_PanelState
  // bit 15; sub_C194F6 needs both).
  bool gateOpen(const Node& n) const {
    if (!w_.hasGate) return false;
    if (w_.gateOpen) return true;
    for (size_t i = 0; i < tasks_.size(); i++)
      if (!n.done[i] && (tasks_[i].blocksExit || tasks_[i].kind == TaskKind::TogglePanel)) return false;
    return true;
  }

  // (no gate: a boss stage, ended by the boss's end; one hit counts here,
  // the agent asks again after it)
  bool exited(const Node& n) const {
    bool boss = false;
    for (size_t i = 0; i < tasks_.size(); i++) {
      if (tasks_[i].kind == TaskKind::EnterGate) return n.done[i];
      if (tasks_[i].kind == TaskKind::HurtBoss) {
        if (!n.done[i]) return false;
        boss = true;
      }
    }
    return boss;
  }

  // Everything but the exit (entering the gate ends the stage, so it's last).
  bool restDone(const Node& n) const {
    if (goal_.blocks && !allDone(n, TaskKind::BombSoft)) return false;
    if (goal_.enemies && !allDone(n, TaskKind::KillEnemy)) return false;
    if (goal_.items && !allDone(n, TaskKind::PickItem)) return false;
    if (goal_.powerups && !powerupsDone(n)) return false;
    return true;
  }

  bool powerupsDone(const Node& n) const {
    for (size_t i = 0; i < tasks_.size(); i++)
      if (tasks_[i].powerup && !n.done[i]) return false;
    return true;
  }

  bool satisfied(const Node& n) const {
    if (goal_.blocks && !allDone(n, TaskKind::BombSoft)) return false;
    if (goal_.enemies && !allDone(n, TaskKind::KillEnemy)) return false;
    if (goal_.items && !allDone(n, TaskKind::PickItem)) return false;
    if (goal_.powerups && !powerupsDone(n)) return false;
    if (goal_.exit && !exited(n)) return false;
    return true;
  }

  // Is the task needed by the goal (directly or to open the gate)?
  bool required(const Node& n, const Task& t) const {
    switch (t.kind) {
      case TaskKind::BombSoft: return goal_.blocks;
      case TaskKind::PickItem: return goal_.items || (goal_.powerups && t.powerup);
      case TaskKind::EnterGate: return goal_.exit;
      case TaskKind::KillEnemy: return goal_.enemies || (goal_.exit && t.blocksExit && !gateOpen(n));
      case TaskKind::HurtBoss: return goal_.exit || goal_.enemies;
      case TaskKind::TogglePanel: return goal_.exit && !gateOpen(n);
      case TaskKind::PushBlock:
      case TaskKind::PressButton:
      case TaskKind::SpawnPlatform: return false;  // (enablers: done when they open the way)
      case TaskKind::LightCrystal:
        // KillAllType8F: crystals are the only way to kill $8F enemies.
        for (size_t i = 0; i < tasks_.size(); i++)
          if (tasks_[i].needsCrystals && !n.done[i] && required(n, tasks_[i])) return true;
        return false;
    }
    return false;
  }

  // Each required task costs at least its base cost: admissible.
  int heuristic(const Node& n) const {
    int h = 0;
    for (size_t i = 0; i < tasks_.size(); i++)
      if (!n.done[i] && required(n, tasks_[i])) h += baseCost(tasks_[i].kind);
    return h;
  }

  bool anyDone(const Node& n, const std::unordered_map<int, std::vector<int>>& m, int k) const {
    auto it = m.find(k);
    if (it == m.end()) return false;
    for (int i : it->second)
      if (n.done[size_t(i)]) return true;
    return false;
  }
  // A push block that has left its cell; a hole a block has filled.
  bool moved(const Node& n, int c, int r) const { return anyDone(n, blockTasks_, cellIdx(c, r)); }
  bool filled(const Node& n, int c, int r) const { return anyDone(n, holeTasks_, cellIdx(c, r)); }
  bool pressed(const Node& n) const {
    for (int i : buttons_)
      if (n.done[size_t(i)]) return true;
    return false;
  }
  bool spawned(const Node& n) const {
    for (int i : spawners_)
      if (n.done[size_t(i)]) return true;
    return false;
  }
  // Can a blast push it now: still there, the hole still open, and a
  // dormant $F5 woken by the button.
  bool pushable(const Node& n, const Task& t) const {
    if (moved(n, t.c, t.r) || filled(n, t.c + t.dc, t.r + t.dr)) return false;
    return w_.at(t.c, t.r) == 'X' || pressed(n);
  }

  bool walkable(const Node& n, int c, int r) const {
    if (!w_.inside(c, r)) return false;
    char t = w_.at(c, r);
    if (breakable(t)) {
      int s = softAt_[size_t(cellIdx(c, r))];
      return w_.p.softpass || (s >= 0 && n.done[size_t(s)]);
    }
    if (t == 'X' || t == 'x') return moved(n, c, r);
    if (t == 'O') return filled(n, c, r);
    return walkableClass(t);
  }
  char cls(const Node& n, int c, int r) const {
    char t = w_.at(c, r);
    if ((breakable(t) || t == 'X' || t == 'x' || t == 'O') && walkable(n, c, r)) return '.';
    return t;
  }

  // Frames to reach each cell, bombing through soft blocks on the way
  // (Dijkstra; a soft block costs a bomb and its fuse). prev: the route.
  struct Routes {
    std::vector<int> d, prev;
  };
  Routes distances(const Node& n) const {
    size_t N = size_t(w_.cols * w_.rows);
    Routes rt{std::vector<int>(N, INT_MAX), std::vector<int>(N, -1)};
    using QE = std::pair<int, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    rt.d[size_t(n.at)] = 0;
    q.push({0, n.at});
    while (!q.empty()) {
      auto [dist, i] = q.top();
      q.pop();
      if (dist > rt.d[size_t(i)]) continue;
      int c = i % w_.cols, r = i / w_.cols;
      int land[kMaxJumps][2], nj = jumps(w_, c, r, land);
      int ln[4][3], nl = nj >= 0 ? 0 : lanes(w_, c, r, ln, spawned(n));
      for (int di = 0; di < (nj >= 0 ? nj : 4 + nl); di++) {
        auto& dd = kDirs[di % 4];
        int cc = c + dd[0], rr = r + dd[1];
        int step = kFPT;
        if (nj >= 0) {  // only a jump leaves a pad (or a jump-pad object's cell)
          cc = land[di][0], rr = land[di][1];
          step = kSpringFrames;
        }
        else if (di >= 4) {  // a platform lane's ride, from its end
          cc = ln[di - 4][0], rr = ln[di - 4][1];
          step = laneFrames(ln[di - 4][2]);
        }
        else if (!w_.inside(cc, rr)) continue;
        else if (w_.at(cc, rr) == 'S') {}
        else if (!walkable(n, cc, rr)) {
          if (!breakable(w_.at(cc, rr))) continue;
          step += kSoftCross;
        }
        int j = cellIdx(cc, rr);
        if (dist + step >= rt.d[size_t(j)]) continue;
        rt.d[size_t(j)] = dist + step;
        rt.prev[size_t(j)] = i;
        q.push({dist + step, j});
      }
    }
    return rt;
  }

  // Cheapest cell to do task t from: the cell itself for items and the gate,
  // else a cell in line within bomb range with nothing stopping the blast.
  bool standCell(const Node& n, const Routes& rt, const Task& t, int& bc, int& br, int& dist) const {
    dist = INT_MAX;
    auto consider = [&](int c, int r) {
      int x = rt.d[size_t(cellIdx(c, r))];
      if (x == INT_MAX) return;
      if (isBombTask(t.kind)) {
        if (t.kind != TaskKind::KillEnemy && t.kind != TaskKind::HurtBoss && !hasCover(n, rt, c, r)) return;  // (enemies move: the agent picks those cells)
        Harm h = harm(n, c, r);
        if (h.gate) return;
        x += h.cost;
      }
      if (x < dist) { dist = x; bc = c; br = r; }
    };
    if (!isBombTask(t.kind)) {
      if (w_.inside(t.c, t.r)) consider(t.c, t.r);
      return dist != INT_MAX;
    }
    if (t.kind == TaskKind::PushBlock) {  // in line on the far side from the hole
      for (int k = 1; k <= std::max(1, w_.p.fire); k++) {
        int c = t.c - t.dc * k, r = t.r - t.dr * k;
        if (w_.inside(c, r) && breakable(w_.at(c, r)) && !walkable(n, c, r)) {
          consider(c, r);  // the route bombs it first, then P1 stands there
          break;
        }
        if (!walkable(n, c, r)) break;
        char x = cls(n, c, r);
        if (canPlaceOn(x)) consider(c, r);
        if (!blastPasses(x)) break;
      }
      return dist != INT_MAX;
    }
    if (t.kind == TaskKind::KillEnemy && w_.inside(t.c, t.r) && walkable(n, t.c, t.r) &&
        canPlaceOn(cls(n, t.c, t.r)))
      consider(t.c, t.r);
    for (auto& dd : kDirs)
      for (int k = 1; k <= std::max(1, w_.p.fire); k++) {
        int c = t.c + dd[0] * k, r = t.r + dd[1] * k;
        if (w_.inside(c, r) && breakable(w_.at(c, r)) && !walkable(n, c, r)) {
          consider(c, r);  // the route bombs it first, then P1 stands there
          break;
        }
        if (!walkable(n, c, r)) break;
        char x = cls(n, c, r);
        if (canPlaceOn(x)) consider(c, r);
        if (!blastPasses(x) && !w_.p.pierce) break;
      }
    return dist != INT_MAX;
  }

  // A bomb at (c,r) leaves P1 somewhere to go: a cell out of its blast
  // within kCoverSteps on this node's map (3-1: the (15,8) panel's only
  // bomb cell (14,8) is a dead end, all of it in the blast; planned forever).
  bool hasCover(const Node& n, const Routes& rt, int c, int r) const {
    const int kCoverSteps = 8;
    // (soft blocks the route to (c,r) bombs on its way are open by then)
    std::vector<uint8_t> via(size_t(w_.cols * w_.rows), 0);
    for (int j = cellIdx(c, r); j >= 0; j = rt.prev[size_t(j)]) via[size_t(j)] = 1;
    std::vector<uint8_t> in(size_t(w_.cols * w_.rows), 0);
    in[size_t(cellIdx(c, r))] = 1;
    for (auto& dd : kDirs)
      for (int k = 1; k <= std::max(1, w_.p.fire); k++) {
        int cc = c + dd[0] * k, rr = r + dd[1] * k;
        if (!w_.inside(cc, rr)) break;
        char x = cls(n, cc, rr);
        in[size_t(cellIdx(cc, rr))] = 1;
        if (!blastPasses(x) && !(w_.p.pierce && breakable(x))) break;
      }
    std::vector<int> d(size_t(w_.cols * w_.rows), -1);
    std::deque<int> q{cellIdx(c, r)};
    d[size_t(cellIdx(c, r))] = 0;
    while (!q.empty()) {
      int k = q.front();
      q.pop_front();
      if (!in[size_t(k)]) return true;
      if (d[size_t(k)] >= kCoverSteps) continue;
      for (auto& dd : kDirs) {
        int cc = k % w_.cols + dd[0], rr = k / w_.cols + dd[1];
        if (!w_.inside(cc, rr) || d[size_t(cellIdx(cc, rr))] >= 0) continue;
        if (!walkable(n, cc, rr) && !(via[size_t(cellIdx(cc, rr))] && breakable(w_.at(cc, rr)))) continue;
        d[size_t(cellIdx(cc, rr))] = d[size_t(k)] + 1;
        q.push_back(cellIdx(cc, rr));
      }
    }
    return false;
  }

  // What a bomb at (c,r) spoils: a flame on the gate brings enemies back
  // (never), and one on an item burns it (the item tasks it ends, and their
  // cost when the goal wanted them). An item still in its block is only
  // uncovered by the blast that breaks the block; the next one burns it.
  struct Harm {
    bool gate = false;
    int cost = 0;
    std::vector<int> burnt;
  };
  Harm harm(const Node& n, int c, int r) const {
    Harm h;
    for (auto& dd : kDirs)
      for (int k = 1; k <= std::max(1, w_.p.fire); k++) {
        int cc = c + dd[0] * k, rr = r + dd[1] * k;
        if (!w_.inside(cc, rr)) break;
        char x = cls(n, cc, rr);
        if (w_.hasGate && cc == w_.gateC && rr == w_.gateR) h.gate = true;
        if (donePanel_.count(cellIdx(cc, rr))) h.cost += kBurnCost;  // (toggled back: todo again, unplanned)
        auto it = itemAt_.find(cellIdx(cc, rr));
        if (it != itemAt_.end() && !breakable(x)) {
          for (int i : it->second)
            if (!n.done[size_t(i)]) {
              h.burnt.push_back(i);
              if (tasks_[size_t(i)].powerup || goal_.items) h.cost += kBurnCost;
            }
          if (!w_.p.pierce) break;  // (an item stops a flame)
        }
        if (x == 'X' || x == 'x' || x == 'B') break;
        if (breakable(x)) {
          if (w_.p.pierce) continue;
          break;
        }
        if (x == 'g' && !w_.gateOpen) break;
        if (!blastPasses(x)) break;
      }
    return h;
  }

  // A bomb at (c,r) also does every static task its blast reaches: soft
  // blocks, panels, crystals (blast rules as in blast.cpp). Enemies move, so
  // they count only when targeted.
  void blastMarks(const Node& n, int c, int r, std::vector<uint8_t>& done) const {
    auto mark = [&](int cc, int rr) {
      for (int t : staticAt_[size_t(cellIdx(cc, rr))]) {
        const Task& k = tasks_[size_t(t)];
        // (a mode 1 panel done earlier in the plan toggles back)
        bool back = k.kind == TaskKind::TogglePanel && k.ref >= 0 && w_.panels[size_t(k.ref)].mode == 1 && n.done[size_t(t)];
        done[size_t(t)] = !back;
      }
    };
    for (auto& dd : kDirs)
      for (int k = 1; k <= std::max(1, w_.p.fire); k++) {
        int cc = c + dd[0] * k, rr = r + dd[1] * k;
        if (!w_.inside(cc, rr)) break;
        char x = cls(n, cc, rr);
        if (breakable(x) || x == 'y' || x == 'P' || x == 'v' || x == 'a') mark(cc, rr);
        if (x == 'X' || x == 'x') {  // pushed the blast's way, if a hole is there
          auto it = blockTasks_.find(cellIdx(cc, rr));
          if (it != blockTasks_.end())
            for (int i : it->second) {
              const Task& t = tasks_[size_t(i)];
              if (t.dc == dd[0] && t.dr == dd[1] && pushable(n, t)) done[size_t(i)] = 1;
            }
          break;
        }
        if (x == 'B') break;  // (its burst does more; not counted)
        if (x == '%' && w_.p.pierce) continue;
        if (!blastPasses(x)) break;
      }
  }

  std::vector<Node> successors(int ni) {
    std::vector<Node> out;
    const Node n = nodes_[size_t(ni)];
    Routes rt = distances(n);
    bool open = gateOpen(n);
    for (size_t i = 0; i < tasks_.size(); i++) {
      const Task& t = tasks_[i];
      if (n.done[i] || t.needsCrystals) continue;  // $8F: immune to bombs
      // Only what the goal needs (soft blocks in the way are bombed by the
      // route itself). The gate ends the stage, so it comes last.
      if (t.kind == TaskKind::EnterGate ? (!open || !goal_.exit || !restDone(n))
          : t.kind == TaskKind::PushBlock ? !pushable(n, t)
          : t.kind != TaskKind::PressButton && t.kind != TaskKind::SpawnPlatform && !required(n, t))
        continue;
      int c, r, dist;
      if (!standCell(n, rt, t, c, r, dist)) continue;
      Node s{cellIdx(c, r), n.done, 0, 0, ni, int(i), c, r, 0, {}};
      s.done[i] = 1;
      for (int j = cellIdx(c, r); j >= 0; j = rt.prev[size_t(j)])
        if (breakable(w_.map[size_t(j)])) {
          int k = softAt_[size_t(j)];
          if (k >= 0 && !s.done[size_t(k)]) {
            s.done[size_t(k)] = 1;
            s.via.push_back(j);
          }
        }
      std::reverse(s.via.begin(), s.via.end());
      if (isBombTask(t.kind)) {
        blastMarks(n, c, r, s.done);
        for (int b : harm(n, c, r).burnt) s.done[size_t(b)] = 1;  // (gone: its cost is in dist)
      }
      if (allDone(s, TaskKind::LightCrystal))
        for (size_t j = 0; j < tasks_.size(); j++)
          if (tasks_[j].needsCrystals) s.done[j] = 1;  // Check8FPanels -> KillAllType8F
      s.stepCost = dist + baseCost(t.kind);
      s.g = n.g + s.stepCost;
      s.h = heuristic(s);
      out.push_back(std::move(s));
    }
    return out;
  }

  Plan finish(Plan& out, int n, bool complete) {
    out.ok = true;
    out.complete = complete;
    out.cost = nodes_[size_t(n)].g;
    out.done = nodes_[size_t(n)].done;
    for (int i = n; nodes_[size_t(i)].parent >= 0; i = nodes_[size_t(i)].parent) {
      const Node& x = nodes_[size_t(i)];
      Step st{x.step, x.standC, x.standR, x.stepCost, {}};
      for (int v : x.via) st.via.push_back({v % w_.cols, v / w_.cols});
      out.steps.push_back(st);
    }
    std::reverse(out.steps.begin(), out.steps.end());
    return out;
  }

  // Search limit hit: from the most finished node, take the cheapest step
  // that lowers h (or any step, if none does) until the goal holds.
  Plan greedy(Plan& out, int n) {
    for (int guard = 0; guard < 4 * int(tasks_.size()) + 4; guard++) {
      if (satisfied(nodes_[size_t(n)])) return finish(out, n, false);
      auto succ = successors(n);
      if (succ.empty()) break;
      int h0 = nodes_[size_t(n)].h;
      auto score = [&](const Node& s) { return (s.h < h0 ? 0 : 1000000) + s.stepCost; };
      auto it = std::min_element(succ.begin(), succ.end(),
                                 [&](const Node& a, const Node& b) { return score(a) < score(b); });
      nodes_.push_back(std::move(*it));
      n = int(nodes_.size() - 1);
    }
    out.why = "search limit hit and the greedy finish got stuck; " + blocked();
    return out;
  }

  // Why no plan: the required tasks P1 can't reach even with every soft
  // block gone.
  std::string blocked() const {
    Node all = nodes_[0];
    for (size_t i = 0; i < tasks_.size(); i++)
      if (tasks_[i].kind == TaskKind::BombSoft) all.done[i] = 1;
    Routes d = distances(all);
    std::ostringstream s;
    s << "unreachable:";
    int n = 0;
    for (size_t i = 0; i < tasks_.size(); i++) {
      const Task& t = tasks_[i];
      Node req = nodes_[0];
      if (!required(req, t) && !(t.kind == TaskKind::EnterGate && goal_.exit)) continue;
      int c, r, dist;
      if (!standCell(all, d, t, c, r, dist)) {
        s << " " << taskName(t.kind) << "(" << t.c << "," << t.r << ")";
        n++;
      }
    }
    if (!n) s << " none (a precondition never holds)";
    return s.str();
  }

  const World& w_;
  Goal goal_;
  PlanOptions o_;
  std::vector<Task> tasks_;
  std::vector<int> softAt_;
  std::unordered_map<int, std::vector<int>> blockTasks_, holeTasks_;  // push tasks by block / hole cell
  std::unordered_map<int, std::vector<int>> itemAt_;                  // item tasks by cell
  std::set<int> donePanel_;  // cells of mode 1 panels done (a blast over one undoes it)
  std::vector<int> buttons_, spawners_;
  std::vector<std::vector<int>> staticAt_;  // static bomb tasks per cell
  bool hasPanels_ = false;
  std::vector<Node> nodes_;
};
}  // namespace

const char* taskName(TaskKind k) {
  switch (k) {
    case TaskKind::KillEnemy: return "kill";
    case TaskKind::BombSoft: return "soft";
    case TaskKind::TogglePanel: return "panel";
    case TaskKind::LightCrystal: return "crystal";
    case TaskKind::PickItem: return "item";
    case TaskKind::EnterGate: return "gate";
    case TaskKind::HurtBoss: return "boss";
    case TaskKind::PushBlock: return "push";
    case TaskKind::PressButton: return "button";
    case TaskKind::SpawnPlatform: return "spawner";
  }
  return "?";
}

bool Goal::parse(const std::string& s, Goal& g, std::string* err) {
  g = Goal{};
  std::istringstream in(s);
  std::string w;
  while (std::getline(in, w, '+')) {
    if (w == "exit") g.exit = true;
    else if (w == "blocks") g.blocks = true;
    else if (w == "enemies") g.enemies = true;
    else if (w == "items") g.items = true;
    else if (w == "powerups") g.powerups = true;
    else {
      if (err) *err = "unknown goal '" + w + "'";
      return false;
    }
  }
  return true;
}

std::string Goal::str() const {
  std::string s;
  auto add = [&](bool on, const char* n) {
    if (on) s += (s.empty() ? "" : "+") + std::string(n);
  };
  add(blocks, "blocks");
  add(enemies, "enemies");
  add(items, "items");
  add(powerups, "powerups");
  add(exit, "exit");
  return s;
}

std::string Plan::str() const {
  std::ostringstream s;
  for (const Step& st : steps) {
    const Task& t = tasks[size_t(st.task)];
    s << taskName(t.kind) << " " << t.c << "," << t.r << " from " << st.standC << "," << st.standR;
    if (!st.via.empty()) {
      s << " via";
      for (auto& v : st.via) s << " " << v.first << "," << v.second;
    }
    s << "\n";
  }
  return s.str();
}

// Cell classes P1 can stand on (docs/worldview.md). Not: walls, soft blocks
// (unless bombed or with the soft-block pass), bombs, holes, push blocks,
// water, magnets, barrels, furnaces, track blocks, springs (they throw P1:
// a jump edge, not a step; TODO model the landing cells) and unknowns.
bool blocksExit(const Enemy& e) {
  if (!e.type || (e.type & 0x40) || (e.f4 & 0x2000) || e.type == 0x96 || isThrower(e.type)) return false;
  if (e.type == 0x90 || e.type == 0x98) return !(e.be & 0x8000);
  return true;
}

// A bomb can't be placed on tiles with bit 6 set: panels, crystals, stairs,
// switches, bombs (docs/mechanics.md, "button places nothing").
bool breakable(char c) { return c == '%' || c == 'B'; }
bool wantedItem(int kind) {
  return kind == kItemBomb || kind == kItemFire || kind == kItemFullFire || kind == kItemHeart || kind == kItemRemote;
}
bool unwantedItem(int kind, const Player& p) {
  return p.remote && (kind == kItemPierce || kind == kItemBomb9 || kind == kItemBomb10);
}
bool springLanding(const World& w, int c, int r, int dc, int dr, int& lc, int& lr, bool hold) {
  for (int j = hold ? 4 : 3; j <= 9; j++) {
    int cc = c + j * dc, rr = r + j * dr;
    if (!w.inside(cc, rr)) return false;
    char t = w.at(cc, rr);
    if (t == 'w' || breakable(t) || w.bombAt(cc, rr)) return false;
    if (t == 'S' || walkableClass(t)) {
      lc = cc, lr = rr;
      return true;
    }
  }
  return false;
}

int throwerAt(const World& w, int c, int r) {
  for (const Enemy& e : w.enemies)
    if (isThrower(e.type) && e.col() == c && e.row() == r) return e.type == 0xE0 ? 1 : -1;
  return 0;
}

bool rideableAt(const World& w, int c, int r) {
  for (const Enemy& e : w.enemies)
    if (isRideable(e.type) && e.col() == c && e.row() == r) return true;
  return false;
}

bool rideDock(const World& w, int c, int r) {
  // Rides boardable now (an object on their cell), then those boardable
  // where one of those docks (taking it brings the object there), ...
  std::vector<uint8_t> ok(w.rides.size(), 0);
  for (bool more = true; more;) {
    more = false;
    for (size_t i = 0; i < w.rides.size(); i++) {
      if (ok[i]) continue;
      const Ride& a = w.rides[i];
      bool b = rideableAt(w, a.c, a.r);
      for (size_t j = 0; j < w.rides.size() && !b; j++) b = ok[j] && w.rides[j].dc == a.c && w.rides[j].dr == a.r;
      if (b) ok[i] = 1, more = true;
    }
  }
  for (size_t j = 0; j < w.rides.size(); j++)
    if (ok[j] && w.rides[j].dc == c && w.rides[j].dr == r) return true;
  return false;
}

int jumps(const World& w, int c, int r, int out[kMaxJumps][2]) {
  if (!w.inside(c, r)) return -1;
  int n = 0;
  // A rideable object's cell: left only by riding it, to where its rides
  // have gone (none known: a dead end).
  if (rideableAt(w, c, r) || rideDock(w, c, r)) {
    for (const Ride& rd : w.rides)
      if (rd.c == c && rd.r == r && n < kMaxJumps) out[n][0] = rd.tc, out[n][1] = rd.tr, n++;
    return n;
  }
  if (w.at(c, r) == 'S') {
    for (auto& d : kDirs) {
      int lc, lr, hc, hr;
      bool let = springLanding(w, c, r, d[0], d[1], lc, lr), held = springLanding(w, c, r, d[0], d[1], hc, hr, true);
      if (let) out[n][0] = lc, out[n][1] = lr, n++;
      if (held && !(let && hc == lc && hr == lr)) out[n][0] = hc, out[n][1] = hr, n++;
    }
    return n;
  }
  if (int dx = throwerAt(w, c, r)) {
    int lc = c + 8 * dx;
    if (w.inside(lc, r) && (w.at(lc, r) == 'S' || walkableClass(w.at(lc, r))) && !w.bombAt(lc, r))
      out[n][0] = lc, out[n][1] = r, n++;
    return n;
  }
  return -1;
}

int lanes(const World& w, int c, int r, int out[4][3], bool spawned) {
  if (!w.inside(c, r) || w.at(c, r) != 'L') return 0;
  int n = 0;
  for (auto& d : kDirs) {
    int k = 1;
    while (w.inside(c + k * d[0], r + k * d[1]) && w.at(c + k * d[0], r + k * d[1]) == '#') k++;
    int tc = c + k * d[0], tr = r + k * d[1];
    if (k < 2 || !w.inside(tc, tr) || w.at(tc, tr) != 'L' || w.bombAt(tc, tr)) continue;
    bool platform = spawned;  // (one may come later: World4Update spawns them)
    for (const Enemy& e : w.enemies)
      if (e.type == 0xDC && e.col() >= std::min(c, tc) && e.col() <= std::max(c, tc) && e.row() >= std::min(r, tr) &&
          e.row() <= std::max(r, tr))
        platform = true;
    if (platform) out[n][0] = tc, out[n][1] = tr, out[n][2] = k - 1, n++;
  }
  return n;
}

bool jumpHeld(const World& w, int c, int r, int lc, int lr) {
  if (!w.inside(c, r) || w.at(c, r) != 'S') return false;
  int dc = (lc > c) - (lc < c), dr = (lr > r) - (lr < r), x, y;
  return !(springLanding(w, c, r, dc, dr, x, y) && x == lc && y == lr);
}

bool canPlaceOn(char c) { return walkableClass(c) && std::string("Py^sbgvn").find(c) == std::string::npos; }

bool walkableClass(char c) { return std::string(".i^Pyspf=ktTcCg*vnL").find(c) != std::string::npos; }

std::vector<uint8_t> panelsDone(const World& w) {
  std::vector<uint8_t> hits(w.panels.size(), 0), out(w.panels.size(), 0);
  for (const Bomb& b : w.bombs)
    for (const Cell& x : blastCells(w, b.c, b.r, b.range, b.pierce))
      for (size_t i = 0; i < w.panels.size(); i++)
        if (w.panels[i].c == x.c && w.panels[i].r == x.r) hits[i]++;
  for (size_t i = 0; i < w.panels.size(); i++)
    out[i] = w.panels[i].mode == 1 ? (w.panels[i].done ^ (hits[i] & 1)) : (w.panels[i].done || hits[i]);
  return out;
}

std::vector<Task> worldTasks(const World& w) {
  std::vector<Task> out;
  for (size_t i = 0; i < w.enemies.size(); i++) {
    const Enemy& e = w.enemies[i];
    if (!e.type || (e.type & 0x40) || isThrower(e.type)) continue;  // not enemies: shots, effects, jump pads
    Task t{TaskKind::KillEnemy, e.col(), e.row(), int(i), e.type == 0x8F, blocksExit(e)};
    out.push_back(t);
  }
  for (int r = 0; r < w.rows; r++)
    for (int c = 0; c < w.cols; c++) {
      char x = w.at(c, r);
      if (breakable(x)) out.push_back({TaskKind::BombSoft, c, r, -1, false, false});
      if (x == 'y') out.push_back({TaskKind::LightCrystal, c, r, -1, false, false});
      if (x == 'v') out.push_back({TaskKind::PressButton, c, r, -1, false, false});
      if (x == 'a') out.push_back({TaskKind::SpawnPlatform, c, r, -1, false, false});
      if (x == 'X' || x == 'x')
        for (auto& d : kDirs)
          if (w.inside(c + d[0], r + d[1]) && w.at(c + d[0], r + d[1]) == 'O') {
            Task t{TaskKind::PushBlock, c, r, -1, false, false};
            t.dc = d[0], t.dr = d[1];
            out.push_back(t);
          }
    }
  std::vector<uint8_t> pd = panelsDone(w);
  for (size_t i = 0; i < w.panels.size(); i++)
    if (!pd[i]) out.push_back({TaskKind::TogglePanel, w.panels[i].c, w.panels[i].r, int(i), false, false});
  for (size_t i = 0; i < w.items.size(); i++)
    out.push_back({TaskKind::PickItem, w.items[i].c, w.items[i].r, int(i), false, false, wantedItem(w.items[i].kind), false});
  // Wanted items still in their blocks: picked up where the block was (the
  // route bombs it first). Unwanted hidden ones are no task.
  for (const Item& it : w.hidden)
    if (wantedItem(it.kind)) out.push_back({TaskKind::PickItem, it.c, it.r, -1, false, false, true, true});
  // A boss part that senses flames: hurt it where its sense points are.
  for (size_t i = 0; i < w.bosses.size(); i++) {
    const BossObj& b = w.bosses[i];
    if (b.sense.empty() || b.hp <= 0) continue;
    int sx = 0, sy = 0;
    for (auto& p : b.sense) sx += p.first, sy += p.second;
    int c = std::clamp((b.x + sx / int(b.sense.size())) / 16, 0, w.cols - 1);
    int r = std::clamp((b.y + sy / int(b.sense.size())) / 16, 0, w.rows - 1);
    out.push_back({TaskKind::HurtBoss, c, r, int(i), false, true});
  }
  if (w.hasGate) out.push_back({TaskKind::EnterGate, w.gateC, w.gateR, -1, false, false});
  return out;
}

Plan plan(const World& w, const Goal& g, const PlanOptions& o) { return Search(w, g, o).run(); }

}  // namespace sbm2
