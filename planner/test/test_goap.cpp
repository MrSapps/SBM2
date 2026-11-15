// Goal layer (planner/goap.cpp) on invented maps and real snapshots.
#include "goap.hpp"
#include "test.hpp"

using namespace sbm2;

// A world from map rows with P1 at cell (pc,pr) plus extra header lines.
static World mk(const std::vector<std::string>& rows, int pc, int pr, const std::string& extra = "") {
  std::string s = "worldview 1\nsize " + std::to_string(rows[0].size()) + " " + std::to_string(rows.size()) +
                  "\nplayer x=" + std::to_string(pc * 16 + 8) + " y=" + std::to_string(pr * 16 + 8) +
                  " fire=2\n" + extra + "\nmap\n";
  for (auto& r : rows) s += r + "\n";
  World w;
  std::string err;
  if (!parseWorld(s, w, &err)) std::printf("  mk: %s\n", err.c_str());
  return w;
}
static std::string enemy(int c, int r, const char* type = "80") {
  return "enemy type=" + std::string(type) + " x=" + std::to_string(c * 16 + 8) + " y=" + std::to_string(r * 16 + 8) + "\n";
}
// The plan as task names and target cells only ("crystal 3,1; gate 1,1").
static std::string tasksOf(const Plan& p) {
  std::string s;
  for (auto& st : p.steps) {
    const Task& t = p.tasks[size_t(st.task)];
    s += (s.empty() ? "" : "; ") + std::string(taskName(t.kind)) + " " + std::to_string(t.c) + "," + std::to_string(t.r);
  }
  return s;
}
// Every task of this kind done by the end of the plan?
static bool allDone(const Plan& p, TaskKind k) {
  for (size_t i = 0; i < p.tasks.size(); i++)
    if (p.tasks[i].kind == k && !p.done[i]) return false;
  return true;
}
static int count(const Plan& p, TaskKind k) {
  int n = 0;
  for (auto& s : p.steps) n += p.tasks[size_t(s.task)].kind == k;
  return n;
}
static Plan run(const World& w, const char* goal) {
  Goal g;
  std::string err;
  if (!Goal::parse(goal, g, &err)) std::printf("  goal: %s\n", err.c_str());
  return plan(w, g);
}

TEST("goal: parse and print") {
  Goal g;
  CHECK(Goal::parse("blocks+exit", g));
  CHECK(g.blocks && g.exit && !g.enemies);
  CHECK_EQ(g.str(), std::string("blocks+exit"));
  std::string err;
  CHECK(!Goal::parse("exit+win", g, &err));
  CHECK_EQ(err, std::string("unknown goal 'win'"));
}

TEST("goap: open gate: just walk in") {
  World w = mk({"#######",
                "#.....#",
                "#..g..#",
                "#######"}, 1, 1, "gate 3 2 open");
  Plan p = run(w, "exit");
  CHECK(p.ok && p.complete);
  CHECK_EQ(p.str(), std::string("gate 3,2 from 3,2\n"));
}

TEST("goap: closed gate: kill the enemy behind a soft block first") {
  World w = mk({"#########",
                "#...%...#",
                "#########"}, 1, 1, "gate 2 1 closed\n" + enemy(6, 1));
  Plan p = run(w, "exit");
  CHECK(p.ok);
  CHECK_EQ(p.str(), std::string("kill 6,1 from 4,1 via 4,1\ngate 2,1 from 2,1\n"));
}

TEST("goap: exit does not bomb blocks it doesn't need") {
  World w = mk({"#######",
                "#..%..#",
                "#.#.#.#",
                "#...g.#",
                "#######"}, 1, 1, "gate 4 3 open");
  Plan p = run(w, "exit");
  CHECK_EQ(p.steps.size(), size_t(1));
}

TEST("goap: blocks goal bombs every soft block and ignores the gate") {
  World w = mk({"#######",
                "#..%..#",
                "#.#.#%#",
                "#%..g.#",
                "#######"}, 1, 1, "gate 4 3 open");
  Plan p = run(w, "blocks");
  CHECK(p.ok);
  CHECK(allDone(p, TaskKind::BombSoft));
  CHECK_EQ(count(p, TaskKind::BombSoft), int(p.steps.size()));
  Plan p2 = run(w, "blocks+exit");
  CHECK(allDone(p2, TaskKind::BombSoft));
  CHECK(p2.tasks[size_t(p2.steps.back().task)].kind == TaskKind::EnterGate);
}

TEST("goap: with gold panels the exit needs the panels and the enemies") {
  World w = mk({"#########",
                "#.P...P.#",
                "#.......#",
                "#########"}, 4, 2, "gate 4 1 closed\npanel 2 1 mode=0\npanel 6 1 mode=0\n" + enemy(7, 2));
  Plan p = run(w, "exit");
  CHECK(p.ok);
  CHECK(allDone(p, TaskKind::TogglePanel));
  CHECK_EQ(count(p, TaskKind::KillEnemy), 1);
  CHECK(p.tasks[size_t(p.steps.back().task)].kind == TaskKind::EnterGate);
}

TEST("goap: what keeps the exit shut (UpdateExitBlocked C1:531D)") {
  Enemy e;
  e.type = 0x80;
  CHECK(blocksExit(e));
  e.f4 = 0x8000;  // dormant still counts
  CHECK(blocksExit(e));
  e.f4 = 0x2000;
  CHECK(!blocksExit(e));
  e.f4 = 0;
  e.type = 0x96;
  CHECK(!blocksExit(e));
  e.type = 0xC2;
  CHECK(!blocksExit(e));
  e.type = 0x90;
  CHECK(blocksExit(e));
  e.be = 0x8000;
  CHECK(!blocksExit(e));
  // A $96 alone doesn't keep the gate shut: straight to the exit.
  World w = mk({"#######",
                "#..g..#",
                "#######"}, 1, 1, "gate 3 1 closed\n" + enemy(5, 1, "96"));
  CHECK_EQ(tasksOf(run(w, "exit")), std::string("gate 3,1"));
}

TEST("goap: $8F enemies die only when every crystal is lit") {
  World w = mk({"###########",
                "#g.y.....y#",
                "#.........#",
                "###########"}, 1, 2, "gate 1 1 closed\n" + enemy(5, 2, "8F"));
  Plan p = run(w, "exit");
  CHECK(p.ok);
  CHECK_EQ(tasksOf(p), std::string("crystal 3,1; crystal 9,1; gate 1,1"));
  CHECK_EQ(count(p, TaskKind::KillEnemy), 0);  // immune to bombs
}

TEST("goap: one bomb does every block, panel and crystal its blast reaches") {
  World w = mk({"#########",
                "#.%...%.#",
                "#########"}, 4, 1);
  Plan p = run(w, "blocks");
  CHECK(p.ok);
  CHECK_EQ(p.steps.size(), size_t(1));  // from (4,1), range 2 reaches both
  CHECK(allDone(p, TaskKind::BombSoft));
}

TEST("goap: nearest first when the order is free") {
  World w = mk({"###########",
                "#.........#",
                "###########"}, 1, 1, "gate 1 1 closed\n" + enemy(9, 1) + enemy(4, 1));
  Plan p = run(w, "enemies");
  CHECK(p.ok);
  CHECK_EQ(p.tasks[size_t(p.steps[0].task)].c, 4);
}

TEST("goap: walled-off gate: no plan, and says why") {
  World w = mk({"#######",
                "#..#..#",
                "#..#g.#",
                "#######"}, 1, 1, "gate 4 2 open");
  Plan p = run(w, "exit");
  CHECK(!p.ok);
  CHECK_EQ(p.why, std::string("unreachable: gate(4,2)"));
}

TEST("goap: many blocks: search limit, greedy finish still clears them") {
  std::vector<std::string> rows = {"#################"};
  for (int r = 0; r < 7; r++) rows.push_back(r % 2 ? "#.%.%.%.%.%.%.%.#" : "#%.%.%.%.%.%.%.%#");
  rows.push_back("#################");
  rows[1][1] = '.';
  World w = mk(rows, 1, 1);
  Goal g;
  Goal::parse("blocks", g);
  PlanOptions o;
  o.maxNodes = 200;
  Plan p = plan(w, g, o);
  CHECK(p.ok);
  CHECK(!p.complete);
  CHECK(allDone(p, TaskKind::BombSoft));
}

// Seed 3, 3-6: the gate was open, P1 sat in the bottom pocket until time
// ran out. The pocket connects to the rest only by spring pads, which the
// goal layer does not model yet, so today it must say the gate is out of
// reach (not invent a walking route). Flip this when springs land.
TEST("goap: s3_36: gate open but only springs lead out of the pocket") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/s3_36_m300.txt", w, &err));
  CHECK(w.gateOpen);
  Plan p = run(w, "exit");
  CHECK(!p.ok);
  CHECK_EQ(p.why, std::string("unreachable: gate(5,2)"));
}

// 1-1 (frame 110): three $94 block the exit and there is one $69 panel, so
// the gate needs all four (sub_C194F6), P1 starts boxed in by soft blocks
// and the route bombs its way out. The $C0 is not an enemy.
TEST("goap: 1-1: kill the three, light the panel, then the gate") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/1-1_bomb.txt", w, &err));
  Plan p = run(w, "exit");
  CHECK(p.ok && p.complete);
  CHECK_EQ(count(p, TaskKind::KillEnemy), 3);
  CHECK_EQ(count(p, TaskKind::TogglePanel), 1);
  CHECK_EQ(count(p, TaskKind::BombSoft), 0);  // only those in the way, via routes
  CHECK(!p.steps.empty() && p.tasks[size_t(p.steps.back().task)].kind == TaskKind::EnterGate);
  CHECK(p.expanded < 500);
}

// Items: kind from the $3C48 list. Bomb ups, fire ups and hearts are worth
// a detour, even still inside their block (the route bombs it open); other
// kinds (score) are not.
TEST("goap: powerups: open the block hiding a bomb up, pick up a fire up, then the exit") {
  World w = mk({"#########",
                "#...%.%.#",
                "#.......#",
                "#.i.i...#",
                "#########"}, 1, 2,
               "gate 7 2 open\nhidden 4 1 kind=0\nhidden 6 1 kind=14\nitem 2 3 item kind=2\nitem 4 3 item kind=15");
  CHECK_EQ(w.hidden.size(), size_t(2));
  Plan p = run(w, "powerups+exit");
  CHECK(p.ok);
  std::string s = tasksOf(p);
  CHECK(s.find("item 4,1") != std::string::npos);
  CHECK(s.find("item 2,3") != std::string::npos);
  CHECK(s.find("4,3") == std::string::npos);  // score: no detour
  CHECK(s.find("6,1") == std::string::npos);
  CHECK(s.rfind("gate 7,2") == s.size() - 8);  // last
  // The hidden one: its own block is bombed on the way.
  for (auto& st : p.steps)
    if (p.tasks[size_t(st.task)].kind == TaskKind::PickItem && p.tasks[size_t(st.task)].c == 4 && p.tasks[size_t(st.task)].r == 1) {
      CHECK(p.tasks[size_t(st.task)].hidden);
      bool bombed = false;
      for (auto& v : st.via) bombed |= v.first == 4 && v.second == 1;
      CHECK(bombed);
    }
  // Exit only: straight to the gate.
  CHECK_EQ(tasksOf(run(w, "exit")), std::string("gate 7,2"));
  Goal g;
  CHECK(Goal::parse("powerups+exit", g) && g.powerups && g.str() == "powerups+exit");
}

// Push blocks (docs/mechanics.md 1): a blast pushes an X one cell its way;
// into a hole, both cells become floor. The planner pushes only to open a
// way (here the panel), from the far side, in line.
TEST("goap: push the block into the hole, then the panel behind it") {
  World w = mk({"#######",
                "#..XOP#",
                "#g#####"}, 1, 1, "gate 1 2 closed\npanel 5 1 mode=0");
  Plan p = run(w, "exit");
  CHECK(p.ok);
  CHECK_EQ(tasksOf(p), std::string("push 3,1; panel 5,1; gate 1,2"));
  const Task& t = p.tasks[size_t(p.steps[0].task)];
  CHECK(t.dc == 1 && t.dr == 0);
  CHECK(p.steps[0].standR == 1 && p.steps[0].standC < 3);  // the far side
  // No hole: no push (and no way to the panel).
  World v = mk({"#######",
                "#..X.P#",
                "#g#####"}, 1, 1, "gate 1 2 closed\npanel 5 1 mode=0");
  CHECK(!run(v, "exit").ok);
  // Nothing behind it needed: no push.
  World u = mk({"#######",
                "#..XO.#",
                "#g#####"}, 1, 1, "gate 1 2 open");
  CHECK_EQ(tasksOf(run(u, "exit")), std::string("gate 1,2"));
}

// World 2's $F5 (x) is a wall until a flame has been on the $6B button (v)
// (C1:4897); the same blast doesn't count for both.
TEST("goap: a dormant block needs the button first") {
  World w = mk({"#######",
                "#..xOP#",
                "#gv####"}, 1, 1, "gate 1 2 closed\npanel 5 1 mode=0");
  Plan p = run(w, "exit");
  CHECK(p.ok);
  CHECK_EQ(tasksOf(p), std::string("button 2,2; push 3,1; panel 5,1; gate 1,2"));
  World v = mk({"#######",
                "#..xOP#",
                "#g#####"}, 1, 1, "gate 1 2 closed\npanel 5 1 mode=0");
  CHECK(!run(v, "exit").ok);
}

// +$F3 is set, not or'ed: another bomb kind costs the remote.
TEST("goap: items: the remote is wanted; with it, other bomb kinds are not") {
  Player p;
  CHECK(wantedItem(kItemRemote) && wantedItem(kItemFullFire) && !wantedItem(kItemPierce));
  CHECK(!unwantedItem(kItemPierce, p));
  p.remote = true;
  CHECK(unwantedItem(kItemPierce, p) && unwantedItem(kItemBomb9, p) && unwantedItem(kItemBomb10, p));
  CHECK(!unwantedItem(kItemBomb, p) && !unwantedItem(kItemRemote, p));
}

// A blast pushes the block its own way only: a block between two holes
// fills one of them, so of the panels behind them one stays out of reach.
TEST("goap: a block pushed one way can't fill the other hole") {
  World w = mk({"#######",
                "#....g#",
                "#..XOP#",
                "###O###",
                "###P###",
                "#######"}, 1, 1, "gate 5 1 closed\npanel 5 2 mode=0\npanel 3 4 mode=0");
  CHECK(!run(w, "exit").ok);
  w.panels.pop_back();  // one panel: fine
  w.map[size_t(4 * w.cols + 3)] = '#';
  Plan p = run(w, "exit");
  CHECK(p.ok);
  CHECK(tasksOf(p).find("push 3,2") == 0);
}

// A push done from a soft block's cell: the route bombs it, then P1 stands
// there (2-6: (13,23) is pushed from the soft block at (13,22)).
TEST("goap: a push from behind a soft block") {
  World w = mk({"#######",
                "#.%XOP#",
                "#g#####"}, 1, 1, "gate 1 2 closed\npanel 5 1 mode=0");
  Plan p = run(w, "exit");
  CHECK(p.ok);
  CHECK_EQ(tasksOf(p), std::string("push 3,1; panel 5,1; gate 1,2"));
  CHECK(p.ok && p.steps[0].standC == 2);
}

// Search limit hit: the greedy finish still finds a chain of pushes and a
// button press that no single step of which brings a task in reach (2-6:
// before, it gave up and bombed every block, which grow back in world 2).
TEST("goap: greedy finish: a chain of pushes and the button that opens the way") {
  // Fill (5,1) then (7,1) in that order, press the button (9,1), then push
  // the woken block (8,2) down into (8,3): the panel (8,4) comes in reach.
  World w = mk({"###########",
                "#g..XOXO.v#",
                "########x##",
                "########O##",
                "########P##",
                "###########"}, 2, 1, "gate 1 1 closed\npanel 8 4 mode=0");
  PlanOptions o;
  o.maxNodes = 1;
  Goal g;
  g.exit = true;
  Plan p = plan(w, g, o);
  CHECK(p.ok);
  CHECK_EQ(tasksOf(p), std::string("push 4,1; push 6,1; button 9,1; push 8,2; panel 8,4; gate 1,1"));
}
