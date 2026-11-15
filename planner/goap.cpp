#include "goap.hpp"

#include <algorithm>
#include <climits>
#include <deque>
#include <queue>
#include <sstream>
#include <unordered_map>

namespace sbm2 {

namespace {
constexpr int kFPT = 20;          // frames per cell walked (bot.lua FPT)
constexpr int kBombCost = 40;     // place a bomb and step out of its line
constexpr int kEnemyCost = 120;   // line up on a moving enemy
constexpr int kSoftCross = 190;   // bomb a soft block in the way: fuse 150 + flames
const int kDirs[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};

int baseCost(TaskKind k) {
  switch (k) {
    case TaskKind::KillEnemy: return kEnemyCost;
    case TaskKind::BombSoft:
    case TaskKind::TogglePanel:
    case TaskKind::LightCrystal: return kBombCost;
    default: return 0;
  }
}

bool isBombTask(TaskKind k) { return k != TaskKind::PickItem && k != TaskKind::EnterGate; }

// A bomb can't be placed on tiles with bit 6 set: panels, crystals, stairs,
// switches, bombs (docs/mechanics.md, "button places nothing").
bool canPlaceOn(char c) { return walkableClass(c) && std::string("Py^sbg").find(c) == std::string::npos; }
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
      if (t.kind == TaskKind::BombSoft || t.kind == TaskKind::TogglePanel || t.kind == TaskKind::LightCrystal)
        staticAt_[size_t(t.r * w.cols + t.c)].push_back(int(i));
      if (t.kind == TaskKind::BombSoft) softAt_[size_t(t.r * w.cols + t.c)] = int(i);
      if (t.kind == TaskKind::TogglePanel) hasPanels_ = true;
    }
    for (const Panel& p : w.panels)
      if (!p.done) hasPanels_ = true;
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

  bool exited(const Node& n) const {
    for (size_t i = 0; i < tasks_.size(); i++)
      if (tasks_[i].kind == TaskKind::EnterGate) return n.done[i];
    return false;
  }

  // Everything but the exit (entering the gate ends the stage, so it's last).
  bool restDone(const Node& n) const {
    if (goal_.blocks && !allDone(n, TaskKind::BombSoft)) return false;
    if (goal_.enemies && !allDone(n, TaskKind::KillEnemy)) return false;
    if (goal_.items && !allDone(n, TaskKind::PickItem)) return false;
    return true;
  }

  bool satisfied(const Node& n) const {
    if (goal_.blocks && !allDone(n, TaskKind::BombSoft)) return false;
    if (goal_.enemies && !allDone(n, TaskKind::KillEnemy)) return false;
    if (goal_.items && !allDone(n, TaskKind::PickItem)) return false;
    if (goal_.exit && !exited(n)) return false;
    return true;
  }

  // Is the task needed by the goal (directly or to open the gate)?
  bool required(const Node& n, const Task& t) const {
    switch (t.kind) {
      case TaskKind::BombSoft: return goal_.blocks;
      case TaskKind::PickItem: return goal_.items;
      case TaskKind::EnterGate: return goal_.exit;
      case TaskKind::KillEnemy: return goal_.enemies || (goal_.exit && t.blocksExit && !gateOpen(n));
      case TaskKind::TogglePanel: return goal_.exit && !gateOpen(n);
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

  bool walkable(const Node& n, int c, int r) const {
    if (!w_.inside(c, r)) return false;
    char t = w_.at(c, r);
    if (t == '%') {
      int s = softAt_[size_t(cellIdx(c, r))];
      return w_.p.softpass || (s >= 0 && n.done[size_t(s)]);
    }
    return walkableClass(t);
  }
  char cls(const Node& n, int c, int r) const {
    char t = w_.at(c, r);
    if (t == '%' && walkable(n, c, r)) return '.';
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
      for (auto& dd : kDirs) {
        int cc = c + dd[0], rr = r + dd[1];
        if (!w_.inside(cc, rr)) continue;
        int step = kFPT;
        if (!walkable(n, cc, rr)) {
          if (w_.at(cc, rr) != '%') continue;
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
      if (x < dist) { dist = x; bc = c; br = r; }
    };
    if (!isBombTask(t.kind)) {
      if (w_.inside(t.c, t.r)) consider(t.c, t.r);
      return dist != INT_MAX;
    }
    if (t.kind == TaskKind::KillEnemy && w_.inside(t.c, t.r) && walkable(n, t.c, t.r) &&
        canPlaceOn(cls(n, t.c, t.r)))
      consider(t.c, t.r);
    for (auto& dd : kDirs)
      for (int k = 1; k <= std::max(1, w_.p.fire); k++) {
        int c = t.c + dd[0] * k, r = t.r + dd[1] * k;
        if (w_.inside(c, r) && w_.at(c, r) == '%' && !walkable(n, c, r)) {
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

  // A bomb at (c,r) also does every static task its blast reaches: soft
  // blocks, panels, crystals (blast rules as in blast.cpp). Enemies move, so
  // they count only when targeted.
  void blastMarks(const Node& n, int c, int r, std::vector<uint8_t>& done) const {
    auto mark = [&](int cc, int rr) {
      for (int t : staticAt_[size_t(cellIdx(cc, rr))]) done[size_t(t)] = 1;
    };
    for (auto& dd : kDirs)
      for (int k = 1; k <= std::max(1, w_.p.fire); k++) {
        int cc = c + dd[0] * k, rr = r + dd[1] * k;
        if (!w_.inside(cc, rr)) break;
        char x = cls(n, cc, rr);
        if (x == '%' || x == 'y' || x == 'P') mark(cc, rr);
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
      if (t.kind == TaskKind::EnterGate ? (!open || !goal_.exit || !restDone(n)) : !required(n, t)) continue;
      int c, r, dist;
      if (!standCell(n, rt, t, c, r, dist)) continue;
      Node s{cellIdx(c, r), n.done, 0, 0, ni, int(i), c, r, 0, {}};
      s.done[i] = 1;
      for (int j = cellIdx(c, r); j >= 0; j = rt.prev[size_t(j)])
        if (w_.map[size_t(j)] == '%') {
          int k = softAt_[size_t(j)];
          if (k >= 0 && !s.done[size_t(k)]) {
            s.done[size_t(k)] = 1;
            s.via.push_back(j);
          }
        }
      std::reverse(s.via.begin(), s.via.end());
      if (isBombTask(t.kind)) blastMarks(n, c, r, s.done);
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
  if (!e.type || (e.type & 0x40) || (e.f4 & 0x2000) || e.type == 0x96) return false;
  if (e.type == 0x90 || e.type == 0x98) return !(e.be & 0x8000);
  return true;
}

bool walkableClass(char c) { return std::string(".i^Pyspf=ktTcCg*").find(c) != std::string::npos; }

std::vector<Task> worldTasks(const World& w) {
  std::vector<Task> out;
  for (size_t i = 0; i < w.enemies.size(); i++) {
    const Enemy& e = w.enemies[i];
    if (!e.type || (e.type & 0x40)) continue;  // not enemies: shots, effects
    Task t{TaskKind::KillEnemy, e.col(), e.row(), int(i), e.type == 0x8F, blocksExit(e)};
    out.push_back(t);
  }
  for (int r = 0; r < w.rows; r++)
    for (int c = 0; c < w.cols; c++) {
      char x = w.at(c, r);
      if (x == '%') out.push_back({TaskKind::BombSoft, c, r, -1, false, false});
      if (x == 'y') out.push_back({TaskKind::LightCrystal, c, r, -1, false, false});
    }
  for (size_t i = 0; i < w.panels.size(); i++)
    if (!w.panels[i].done) out.push_back({TaskKind::TogglePanel, w.panels[i].c, w.panels[i].r, int(i), false, false});
  for (size_t i = 0; i < w.items.size(); i++) out.push_back({TaskKind::PickItem, w.items[i].c, w.items[i].r, int(i), false, false});
  if (w.hasGate) out.push_back({TaskKind::EnterGate, w.gateC, w.gateR, -1, false, false});
  return out;
}

Plan plan(const World& w, const Goal& g, const PlanOptions& o) { return Search(w, g, o).run(); }

}  // namespace sbm2
