// Goal layer (GOAP): what to do next, never how to move frame by frame.
//
// The world gives a list of tasks (kill this enemy, bomb this soft block,
// toggle this panel, light this crystal, pick up this item, enter the gate).
// A goal is a condition over which tasks are done and where P1 stands
// ("exit", "blocks", "enemies", "items", or several joined with '+', e.g.
// "blocks+exit"). A* over (P1 cell, tasks done) finds the cheapest order.
// Costs are frame estimates from walking distances on the current map, where
// soft blocks already bombed in the plan count as floor.
//
// Nothing here names a stage: every rule is a game rule (docs/mechanics.md,
// docs/enemies.md) applied to whatever the world view holds. The timed
// safety search (next layer down) turns the first step into inputs.
#pragma once
#include <string>
#include <vector>

#include "world.hpp"

namespace sbm2 {

enum class TaskKind { KillEnemy, BombSoft /* or a barrel */, TogglePanel, LightCrystal, PickItem, EnterGate };
const char* taskName(TaskKind k);

struct Task {
  TaskKind kind;
  int c = 0, r = 0;   // target cell
  int ref = -1;       // index into world.enemies / panels / items, else -1
  bool needsCrystals = false;  // $8F: immune until every crystal is lit
  bool blocksExit = false;     // KillEnemy: keeps the gate shut while alive
};

// UpdateExitBlocked C1:531D: does this object keep the exit shut?
bool blocksExit(const Enemy& e);

// Goal: a set of conditions, all of which must hold.
struct Goal {
  bool exit = false;     // P1 on the open gate
  bool blocks = false;   // no soft blocks left
  bool enemies = false;  // no killable enemies left
  bool items = false;    // no items left
  // "exit", "blocks+exit", ... Returns false on an unknown word.
  static bool parse(const std::string& s, Goal& g, std::string* err = nullptr);
  std::string str() const;
};

struct Step {
  int task;          // index into Plan::tasks
  int standC, standR;  // where P1 does it from
  int cost;          // frames, estimated
  std::vector<std::pair<int, int>> via;  // soft blocks bombed on the way there
};

struct Plan {
  bool ok = false;
  bool complete = false;  // false: search hit its limit, the tail is greedy
  std::string why;        // when !ok: what blocks the goal
  std::vector<Task> tasks;
  std::vector<Step> steps;
  std::vector<uint8_t> done;  // per task, after the last step
  int cost = 0;
  int expanded = 0;
  std::string str() const;  // one step per line, for tests and logs
};

struct PlanOptions {
  int maxNodes = 3000;  // then a greedy finish
};

// Rules shared with the lower layers.
bool walkableClass(char c);  // P1 can stand here (soft blocks excepted)
bool canPlaceOn(char c);     // ...and drop a bomb here
// A blast clears it: soft blocks and barrels (a barrel bursts, C1:26AA).
bool breakable(char c);
std::vector<Task> worldTasks(const World& w);

Plan plan(const World& w, const Goal& g, const PlanOptions& o = {});

}  // namespace sbm2
