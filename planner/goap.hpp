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

enum class TaskKind { KillEnemy, BombSoft /* or a barrel */, TogglePanel, LightCrystal, PickItem, EnterGate, HurtBoss,
                      PushBlock /* into the hole at (c+dc, r+dr) */, PressButton };
const char* taskName(TaskKind k);

struct Task {
  TaskKind kind;
  int c = 0, r = 0;   // target cell
  int ref = -1;       // index into world.enemies / panels / items / bosses, else -1
  bool needsCrystals = false;  // $8F: immune until every crystal is lit
  bool blocksExit = false;     // KillEnemy: keeps the gate shut while alive
  bool powerup = false;        // PickItem: a bomb up, fire up or heart (the "powerups" goal)
  bool hidden = false;         // PickItem: still inside the soft block at (c,r)
  int dc = 0, dr = 0;          // PushBlock: the way the blast pushes it
};
// Items worth a detour: bomb up, fire up, full fire, heart, remote.
bool wantedItem(int kind);
// Items to keep off: another bomb kind would replace the remote (+$F3 is
// set, not or'ed: C0:9965 pierce 4, C0:9B1D remote 1, C0:9C3A 2, C0:9C19 8).
bool unwantedItem(int kind, const Player& p);

// UpdateExitBlocked C1:531D: does this object keep the exit shut?
bool blocksExit(const Enemy& e);

// Goal: a set of conditions, all of which must hold.
struct Goal {
  bool exit = false;     // P1 on the open gate
  bool blocks = false;   // no soft blocks left
  bool enemies = false;  // no killable enemies left
  bool items = false;    // no items left
  bool powerups = false; // no wanted items left (bomb up, fire up, heart; hidden ones too)
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
// Spring pads ('S', docs/mechanics.md 5): stepping on one locks P1 ~20
// frames, then throws it the way held. Let go in the air (the path
// follower does) P1 lands 3 cells on, or slides on past walls to the next
// cell it can stand on (or a pad). Water, a soft block, a bomb or the map edge first: no landing
// (drowned, stuck). A pad is a node P1 leaves only by a jump.
// hold: the way kept held in the air instead (the path follower does when
// the landing waypoint says so, kJumpHold): 4 cells on, sliding the same way
// (3-6's left climb, (5,21) Up held to the (5,16) pad, let go it drowns).
bool springLanding(const World& w, int c, int r, int dc, int dr, int& lc, int& lr, bool hold = false);
constexpr int kSpringFrames = 90;  // locked + in the air
// Jump-pad objects ($E0 right, $A0/$E1 left, docs/enemies.md): P1 on the
// object's cell is caught and thrown 8 cells. The way (+1/-1), or 0.
int throwerAt(const World& w, int c, int r);
inline bool isThrower(int type) { return type == 0xE0 || type == 0xA0 || type == 0xE1; }
// Objects that carry P1 once it walks onto their cell (3-5's submarines
// $DA/$DB, docs/mechanics.md 5b). Where to is learnt (World::rides).
inline bool isRideable(int type) { return type == 0xDA || type == 0xDB; }
bool rideableAt(const World& w, int c, int r);
// (c,r) is where a ride P1 can take docks its object: boardable there later.
bool rideDock(const World& w, int c, int r);
// Where P1 stepping on (c,r) is thrown to: a spring pad's landings, or a
// jump-pad object's. -1: (c,r) throws nothing (P1 walks on); else how many
// landings are in out.
constexpr int kMaxJumps = 8;  // 4 ways, let go or held
int jumps(const World& w, int c, int r, int out[kMaxJumps][2]);
// The jump from pad (c,r) to (lc,lr) needs the way held in the air (its
// let-go landing is elsewhere).
bool jumpHeld(const World& w, int c, int r, int lc, int lr);
// Waypoint flag (on x): hold the way to this waypoint through a jump's flight.
constexpr int16_t kJumpHold = 0x4000;
// Waypoint flag (on x): a ride lets P1 off here (port/path_pad.h PP_RIDE).
constexpr int16_t kRideEnd = 0x2000;
constexpr int16_t kWpFlags = kJumpHold | kRideEnd;
// Each panel's state once the bombs already out have gone off (docs/
// mechanics.md 4): mode 1 toggles once per blast over it (parity), mode 0
// is done by any.
std::vector<uint8_t> panelsDone(const World& w);
std::vector<Task> worldTasks(const World& w);

Plan plan(const World& w, const Goal& g, const PlanOptions& o = {});

}  // namespace sbm2
