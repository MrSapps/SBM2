// Goal layer (planner/goap.cpp) on invented maps and real snapshots.
#include "goap.hpp"
#include "blast.hpp"
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
  CHECK_EQ(p.str(), std::string("kill 6,1 from 5,1 via 4,1\ngate 2,1 from 2,1\n"));  // (from 4,1 the blast reaches the gate)
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
                "###.#####",
                "#########"}, 4, 1);
  Plan p = run(w, "blocks");
  CHECK(p.ok);
  CHECK_EQ(p.steps.size(), size_t(1));  // from (4,1), range 2 reaches both (cover: (3,2))
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
// ran out. The pocket connects to the rest only by spring pads (held jumps
// among them): the goal layer routes over them to the gate.
TEST("goap: s3_36: gate open, only springs lead out of the pocket") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/s3_36_m300.txt", w, &err));
  CHECK(w.gateOpen);
  Plan p = run(w, "exit");
  CHECK(p.ok);
  CHECK_EQ(tasksOf(p), std::string("gate 5,2"));
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

// World 5 starts P1 at the slowest speed, slower than its $8A (1.25 px/f):
// a speed item lying a few cells off is worth the steps then (5-1 boxed in
// beside one), not a far one (2-7's detour lost to the $A6) or with speed.
TEST("goap: a speed item close by is a step only when P1 is slowest") {
  auto has = [](const Plan& p) {
    for (auto& st : p.steps)
      if (p.tasks[size_t(st.task)].kind == TaskKind::PickItem) return true;
    return false;
  };
  const std::vector<std::string> m = {"##########",
                                      "#........#",
                                      "#g########"};
  World w = mk(m, 1, 1, "gate 1 2 open\nitem 3 1 item kind=4");
  w.p.e0 = 1;
  CHECK(has(run(w, "powerups+exit")));
  World fast = w;
  fast.p.e0 = 2;
  CHECK(!has(run(fast, "powerups+exit")));
  World far = mk(m, 1, 1, "gate 1 2 open\nitem 7 1 item kind=4");
  far.p.e0 = 1;
  CHECK(!has(run(far, "powerups+exit")));
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
  CHECK(p.steps[0].dc == 1 && p.steps[0].dr == 0 && p.steps[0].pushes == 1 && p.steps[0].sinks);
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
  CHECK(wantedItem(kItemRemote, p) && wantedItem(kItemFullFire, p) && !wantedItem(kItemPierce, p));
  // pierce is kept off whatever P1 holds (its blasts go on to the gate:
  // 3-0, 4-3, 4-4 in the whole game), unless P1 has it already
  CHECK(unwantedItem(kItemPierce, p) && !unwantedItem(kItemBomb9, p));
  {
    Player q;
    q.pierce = true;
    CHECK(!unwantedItem(kItemPierce, q));
  }
  p.remote = true;
  CHECK(unwantedItem(kItemPierce, p) && unwantedItem(kItemBomb9, p) && unwantedItem(kItemBomb10, p));
  CHECK(!unwantedItem(kItemBomb, p) && !unwantedItem(kItemRemote, p));
}

// Soft pass (kind 12, +$FB) is wanted until held (3-6's was left lying).
TEST("goap: items: kick and glove wanted until held; soft pass not fetched") {
  Player p;
  CHECK(!wantedItem(kItemSoftPass, p) && wantedItem(kItemKick, p) && wantedItem(kItemGlove, p));
  p.softpass = 1, p.kick = 1, p.glove = 1;
  CHECK(!wantedItem(kItemSoftPass, p) && !wantedItem(kItemKick, p) && !wantedItem(kItemGlove, p));
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
  PlanOptions noWay;  // (openWay would push it for the one panel all the same)
  noWay.openWay = false;
  Goal ex;
  Goal::parse("exit", ex);
  CHECK(!plan(w, ex, noWay).ok);
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

// The game won't take a bomb on a done panel ('n'): the push is done from
// the other cell in reach (2-5: (5,5) from (4,5), not the panel at (3,5)).
TEST("goap: a push not from a cell that takes no bomb") {
  World w = mk({"########",
                "#.n.XOP#",
                "#g######"}, 1, 1, "gate 1 2 closed\npanel 6 1 mode=0");
  w.p.fire = 2;
  Plan p = run(w, "exit");
  CHECK(p.ok);
  CHECK_EQ(tasksOf(p), std::string("push 4,1; panel 6,1; gate 1,2"));
  CHECK(p.ok && p.steps[0].standC == 3);
}

// Search limit hit: the greedy finish still finds a chain of pushes and a
// button press that no single step of which brings a task in reach (2-6:
// before, it gave up and bombed every block, which grow back in world 2).
TEST("goap: greedy finish: a chain of pushes and the button that opens the way") {
  // Fill (5,1) then (7,1) in that order, press the button (9,1), then push
  // the woken block (8,2) down into (8,3): the panel (8,4) comes in reach.
  World w = mk({"###########",
                "#...XOXO.v#",
                "#g######x##",
                "########O##",
                "########P##",
                "###########"}, 2, 1, "gate 1 2 closed\npanel 8 4 mode=0");
  PlanOptions o;
  o.maxNodes = 1;
  Goal g;
  g.exit = true;
  Plan p = plan(w, g, o);
  CHECK(p.ok);
  CHECK_EQ(tasksOf(p), std::string("push 4,1; push 6,1; button 9,1; push 8,2; panel 8,4; gate 1,2"));
}

// A flame burns an item it reaches (the user saw a heart blown up): the
// bomb for the block goes where its blast misses the item, or the item is
// picked up first.
TEST("goap: a bomb's blast never burns a wanted item") {
  World w = mk({"#######",
                "#....%#",
                "###i###",
                "#######"}, 1, 1, "item 3 2 heart kind=11");
  Plan p = run(w, "blocks+powerups");
  CHECK(p.ok);
  // ((3,1)'s blast catches every cell P1 could go, the item's too: (4,1))
  CHECK_EQ(p.str(), std::string("soft 5,1 from 4,1\nitem 3,2 from 3,2\n"));
  // ...and an item still in its block is uncovered by the blast that breaks
  // it; the next blast over it burns it.
  World h = mk({"#######",
                "#.%...#",
                "#.##%##",
                "#.#####",
                "#######"}, 1, 1, "hidden 2 1 kind=11");
  h.p.fire = 1;  // ((1,3): out of the first bomb's blast)
  p = run(h, "blocks+powerups");
  CHECK(p.ok);
  CHECK_EQ(tasksOf(p), std::string("soft 2,1; item 2,1; soft 4,2"));
}

// A flame on the exit brings enemies back (seen in world 2): no bomb
// whose blast reaches the gate cell.
TEST("goap: no bomb's blast reaches the gate") {
  World w = mk({"#######",
                "#g...%#",
                "#######"}, 3, 1, "gate 1 1 closed");
  Plan p = run(w, "blocks");
  CHECK(p.ok);
  CHECK_EQ(p.str(), std::string("soft 5,1 from 4,1\n"));
}

// Spring pads (docs/mechanics.md 5): held through the flight P1 lands 4
// cells on, sliding past walls to the next cell it can stand on; water or
// a soft block there is no landing. A pad is passed, never stood on.
TEST("goap: spring pads: landings, and a room reached only by a jump") {
  World w = mk({"###########",
                "#...S..##.#",
                "###########",
                "#.........#",
                "#.........#",
                "#...i.....#",
                "###########"}, 1, 1, "item 4 5 item kind=2");
  int c, r;
  CHECK(springLanding(w, 4, 1, 0, 1, c, r));
  CHECK_EQ(c, 4);
  CHECK_EQ(r, 4);
  CHECK(!springLanding(w, 4, 1, 0, -1, c, r));  // (off the map)
  CHECK(springLanding(w, 4, 1, -1, 0, c, r));
  CHECK_EQ(c, 1);
  CHECK(springLanding(w, 4, 1, 1, 0, c, r));    // (3 on is a wall: slides to the next cell)
  CHECK_EQ(c, 9);
  World wet = w;
  wet.map[size_t(4 * w.cols + 4)] = 'w';
  CHECK(!springLanding(wet, 4, 1, 0, 1, c, r));
  Plan p = run(w, "items");
  CHECK(p.ok);
  CHECK_EQ(tasksOf(p), std::string("item 4,5"));
  World flat = w;
  flat.map[size_t(1 * w.cols + 4)] = '.';
  CHECK(!run(flat, "items").ok);
}

// Jump-pad objects (docs/enemies.md): $E0 throws P1 8 cells right, $A0/$E1
// 8 left; its cell is left only that way.
TEST("goap: jump-pad objects throw P1 8 cells") {
  World w = mk({"#############",
                "#...#.......#",
                "#############"}, 1, 1, enemy(2, 1, "E0") + "item 10 1 item kind=2");
  CHECK_EQ(throwerAt(w, 2, 1), 1);
  int l[4][2];
  CHECK_EQ(jumps(w, 2, 1, l), 1);
  CHECK_EQ(l[0][0], 10);
  CHECK_EQ(jumps(w, 1, 1, l), -1);
  CHECK(run(w, "items").ok);
  World none = mk({"#############",
                   "#...#.......#",
                   "#############"}, 1, 1, "item 10 1 item kind=2");
  CHECK(!run(none, "items").ok);
  World back = mk({"#############",
                   "#...#.......#",
                   "#############"}, 10, 1, enemy(10, 1, "E1") + "item 2 1 item kind=2");
  CHECK_EQ(throwerAt(back, 10, 1), -1);
  CHECK(run(back, "items").ok);
  // ($A0 too; none of them is an enemy to kill, nor keeps the gate shut)
  World a0 = mk({"#############",
                 "#...#.......#",
                 "#############"}, 10, 1, enemy(10, 1, "A0") + "gate 1 1 closed");
  CHECK_EQ(throwerAt(a0, 10, 1), -1);
  CHECK(!blocksExit(a0.enemies[0]));
  for (const Task& t : worldTasks(a0)) CHECK(t.kind != TaskKind::KillEnemy);
}

// Mode 1 panels (docs/mechanics.md 4) toggle once per blast: a bomb out
// whose blast covers one is about to flip it (3-1: bombed (9,9) from (9,8),
// the move ended before it went off, then from (8,9) too: undone, forever).
TEST("goap: mode 1 panels: bombs out count, and a done one is not bombed again") {
  World w = mk({"#######",
                "#.....#",
                "#.#P#.#",
                "#.....#",
                "#######"}, 1, 1, "panel 3 2 todo mode=1\nbomb c=3 r=1 fuse=100 range=1 owner=p1");
  CHECK(panelsDone(w)[0]);
  for (const Task& t : worldTasks(w)) CHECK(t.kind != TaskKind::TogglePanel);
  World two = w;  // (two blasts: back to todo)
  two.bombs.push_back(two.bombs[0]);
  two.bombs.back().r = 3;
  CHECK(!panelsDone(two)[0]);
  World m0 = w;  // (mode 0: any blast does it)
  m0.panels[0].mode = 0;
  m0.bombs = two.bombs;
  CHECK(panelsDone(m0)[0]);
  // A done panel: the soft block below it is bombed from a side, not from
  // P1's own cell (3,1), whose blast passes the panel (undoing it).
  World d = mk({"#######",
                "#.....#",
                "#.#P#.#",
                "#..%..#",
                "#######"}, 3, 1, "panel 3 2 done mode=1");
  Plan p = run(d, "blocks");
  CHECK(p.ok);
  CHECK(!p.steps.empty() && p.steps[0].standR == 3);
  // Todo: from (3,1) both at once.
  World t = d;
  t.panels[0].done = false;
  Plan q = run(t, "blocks");
  CHECK(q.ok && q.steps.size() == 1 && q.steps[0].standR == 1);
  // Two planned blasts over a todo panel: done, then undone again.
  World v = mk({"######",
                "#....#",
                "##P#.#",
                "##%#.#",
                "##%#.#",
                "######"}, 2, 1, "panel 2 2 todo mode=1");
  Plan b = run(v, "blocks");
  CHECK(b.ok);
  CHECK_EQ(b.steps.size(), size_t(2));
  for (size_t i = 0; i < b.tasks.size(); i++)
    if (b.tasks[i].kind == TaskKind::TogglePanel) CHECK(!b.done[i]);
}

// A blast pushes a block one cell onto floor too: a hole down a line of
// floor takes a push per cell (5-2: (20,8) goes down three into the hole
// at (20,11), the only way east).
TEST("goap: a block pushed down a line of floor into a hole") {
  World w = mk({"##########",
                "#..X..OP##",
                "#.########",
                "#g########"}, 1, 1, "gate 1 3 closed\npanel 7 1 mode=0");
  Plan p = run(w, "exit");
  CHECK(p.ok && !p.steps.empty());
  if (p.steps.empty()) return;
  CHECK_EQ(tasksOf(p), std::string("push 3,1; panel 7,1; gate 1,3"));
  CHECK(p.steps[0].dc == 1 && p.steps[0].pushes == 3 && p.steps[0].sinks);
  // Something in the line: no push.
  World v = mk({"##########",
                "#..X.%OP##",
                "#.########",
                "#g########"}, 1, 1, "gate 1 3 closed\npanel 7 1 mode=0");
  CHECK(!run(v, "exit").ok);
}

// Softpass (P1 walks through soft blocks) doesn't make one floor for a
// push: the game pushes only onto $00 or a hole (5-5 played on with
// softpass: (21,12) planned pushed over the soft (22,12); 5000 frames of
// the same bomb, the timer).
TEST("goap: softpass: a soft block still stops a push") {
  World v = mk({"##########",
                "#..X.%OP##",
                "#.########",
                "#g########"}, 1, 1, "gate 1 3 closed\npanel 7 1 mode=0");
  v.p.softpass = 1;
  Plan p = run(v, "exit");
  std::string ts = tasksOf(p);
  size_t push = ts.find("push 3,1"), soft = ts.find("soft 5,1");
  CHECK(!p.ok || (push != std::string::npos && soft != std::string::npos && soft < push));
}

// C0:30C8 refuses a bomb on a tile with bit 6: no bomb cell on the track
// $4D 'k' (4-6: the panel at (7,23) planned from the track at (5,23); the
// bomb never went down, the $DD on the track changed the map, and it was
// tried again for 18000 frames).
TEST("goap: no bomb on a track cell") {
  CHECK(!canPlaceOn('k') && !canPlaceOn('C'));
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/4-6_track_panel.txt", w, &err));
  Goal g;
  Goal::parse("powerups+exit", g);
  Plan p = plan(w, g);
  CHECK(p.ok);
  for (const Step& s : p.steps)
    if (p.tasks[size_t(s.task)].kind != TaskKind::PickItem && p.tasks[size_t(s.task)].kind != TaskKind::EnterGate)
      CHECK(w.at(s.standC, s.standR) != 'k');
}

// 5-2 from its start: blocks pushed down lines of floor, and mode 1 panels
// side by side ((26,3) and (28,3), one blast from (27,3) over both): a
// blast over a panel the plan has lit undoes it, so it costs as much as
// one lit at the start (the greedy tail toggled the two by turns forever).
TEST("goap: 5-2 from its start has a plan") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/5-2_start.txt", w, &err));
  Goal g;
  Goal::parse("powerups+exit", g);
  Plan p = plan(w, g);
  CHECK(p.ok);
}

// Nothing plans: openWay() pushes blocks along the floor (a Sokoban) till a
// task's cell joins P1's region; plan() hands back its first move.
TEST("goap: open the way: a block pushed along the floor") {
  World w = mk({"#######",
                "#.X...#",
                "#.##g##",
                "#.#####",
                "#.#####",
                "#######"}, 1, 1, "gate 4 2 open");
  int gate = 2 * w.cols + 4;
  std::vector<int> way = openWay(w, {{gate}});
  CHECK_EQ(way.size(), size_t(3));
  if (!way.empty()) CHECK_EQ(way[0], 1 * w.cols + 1);  // (fire 2: the next from (1,1) or (2,1))
  Plan p = run(w, "exit");
  CHECK(p.ok && p.why == "open way" && p.steps.size() == 1);
  if (!p.steps.empty()) CHECK(p.steps[0].standC == 1 && p.steps[0].standR == 1);
  PlanOptions no;
  no.openWay = false;
  Goal ex;
  Goal::parse("exit", ex);
  CHECK(!plan(w, ex, no).ok);
}

// 5-5 from its start: blocks in two corridors, holes at column 25, the rest
// of the stage beyond (bot.lua's stagePlan took 323 states).
TEST("goap: 5-5 from its start opens a way east") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/5-5_start.txt", w, &err));
  Goal g;
  Goal::parse("powerups+exit", g);
  Plan p = plan(w, g);
  CHECK(p.ok && p.why == "open way");
  std::vector<int> way = openWay(w, {{9 * w.cols + 28}});  // (28,9): the way up to row 6
  CHECK(!way.empty());
}

// openWay() opens a way to every goal or none: 2-2 at 1990, the block just
// pushed into the hole at (4,11) is still moving ($C5 on the hole: a wall
// for ~160 frames), so the panel at (3,10) is shut off a while. A way to
// the two east panels alone began with the soft block (6,7), which grows
// back: bombed again and again for 14000 frames.
TEST("goap: open the way to every goal or none") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/2-2_push_settling.txt", w, &err));
  CHECK(!run(w, "powerups+exit").ok);
}

// Held in the air a jump lands 4 cells on (sliding past walls): 3-6's left
// climb, (5,21) Up let go lands in water, held reaches the (5,16) pad.
TEST("goap: spring pads: held jumps, and a climb only they make") {
  World w = mk({"#####",
                "#...#",
                "##w##",
                "#####",
                "#####",
                "##S##",
                "#...#",
                "#####"}, 1, 6);
  int c, r;
  CHECK(!springLanding(w, 2, 5, 0, -1, c, r));  // (let go: 3 up is water)
  CHECK(springLanding(w, 2, 5, 0, -1, c, r, true));
  CHECK_EQ(r, 1);  // (4 up is (2,1))
  CHECK(jumpHeld(w, 2, 5, 2, 1));
  CHECK(!jumpHeld(w, 1, 6, 2, 6));  // (not a pad)
  int l[kMaxJumps][2];
  int n = jumps(w, 2, 5, l);
  bool up = false;
  for (int i = 0; i < n; i++) up |= l[i][0] == 2 && l[i][1] == 1;
  CHECK(up);
  World g = w;
  g.hasGate = true, g.gateOpen = true, g.gateC = 1, g.gateR = 1;
  g.map[size_t(1 * w.cols + 1)] = 'g';
  CHECK(run(g, "exit").ok);
}

// Rideable objects (3-5's subs, docs/mechanics.md 5b): their cell leads
// only where a learnt ride went; none known, a dead end.
TEST("goap: rides: a learnt ride is a route, an unknown one a dead end") {
  std::vector<std::string> m{"###########",
                             "#...^#....#",
                             "###########"};
  World w = mk(m, 1, 1, enemy(4, 1, "DA") + "item 8 1 item kind=2\nride 4 1 8 1");
  CHECK(rideableAt(w, 4, 1));
  int l[kMaxJumps][2];
  CHECK_EQ(jumps(w, 4, 1, l), 1);
  CHECK_EQ(l[0][0], 8);
  CHECK(run(w, "items").ok);
  World none = mk(m, 1, 1, enemy(4, 1, "DA") + "item 8 1 item kind=2");
  CHECK_EQ(jumps(none, 4, 1, l), 0);
  CHECK(!run(none, "items").ok);
  World gone = mk(m, 1, 1, "item 8 1 item kind=2\nride 4 1 8 1");  // (the sub is elsewhere now)
  CHECK(!run(gone, "items").ok);
}

// A shuttle (3-5's subs): ridden over, it docks on the far side, where it
// takes P1 back. Its rides chain: the dock of one boardable ride is
// boardable too.
TEST("goap: rides: there and back by the ride's dock") {
  std::vector<std::string> m{"###########",
                             "#g..^#^...#",
                             "###########"};
  // (4,1) carries P1to (8,1), the sub then at (6,1); (6,1) back to (2,1), the sub at (4,1).
  std::string rides = "ride 4 1 8 1 dock_c=6 dock_r=1\nride 6 1 2 1 dock_c=4 dock_r=1\n";
  World w = mk(m, 1, 1, enemy(4, 1, "DA") + rides + "gate 1 1 open\nitem 9 1 item kind=2");
  CHECK(rideDock(w, 6, 1));
  CHECK(!rideDock(w, 4, 1) || rideableAt(w, 4, 1));
  Plan p = run(w, "items+exit");
  CHECK(p.ok);
  CHECK_EQ(tasksOf(p), std::string("item 9,1; gate 1,1"));
  World nodock = mk(m, 1, 1, enemy(4, 1, "DA") + "ride 4 1 8 1\nride 6 1 2 1\ngate 1 1 open\nitem 9 1 item kind=2");
  CHECK(!rideDock(nodock, 6, 1));
  CHECK(!run(nodock, "items+exit").ok);
}

// A bomb cell is only good with somewhere out of its blast to go. 3-1
// (frame 4723): the (15,8) panel's only bomb cell (14,8), fire 1, is a
// pocket reached by a jump, all of it in the blast; the plan used it
// forever. Now a soft block by it is opened first (here (14,10), on the
// way to the (15,10) panel).
TEST("goap: 3-1: no bomb from a pocket with no cover") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/3-1_panel_pocket.txt", w, &err));
  Plan p = run(w, "exit");
  CHECK(p.ok);
  bool opened = false;
  for (auto& st : p.steps) {
    const Task& t = p.tasks[size_t(st.task)];
    if (t.kind == TaskKind::BombSoft && ((t.c == 13 && t.r == 8) || (t.c == 14 && t.r == 7) || (t.c == 14 && t.r == 10))) opened = true;
    for (auto& v : st.via) opened |= (v.first == 13 && v.second == 8) || (v.first == 14 && v.second == 7) || (v.first == 14 && v.second == 10);
    if (t.kind == TaskKind::TogglePanel && t.c == 15 && t.r == 8) {
      CHECK(opened || !(st.standC == 14 && st.standR == 8));
      break;
    }
  }
}

// World 4 platform lanes (docs/mechanics.md 7): board cells 'L' at both
// ends of a run of walls, joined while a $DC platform is on the run (4-2:
// (12,9) to (16,9) over walls (13..15,9), the only way to the gate side).
TEST("goap: platform lanes join their ends, while a platform runs there") {
  World w = mk({"###########",
                "#..L###L.g#",
                "###########"}, 1, 1, "gate 9 1 open\nenemy slot=5 type=DC x=88 y=24 hp=0 f4=0C00");
  int ln[4][3];
  CHECK_EQ(lanes(w, 3, 1, ln), 1);
  CHECK_EQ(ln[0][0], 7);
  CHECK_EQ(ln[0][1], 1);
  CHECK_EQ(ln[0][2], 3);
  CHECK_EQ(lanes(w, 7, 1, ln), 1);
  CHECK_EQ(lanes(w, 2, 1, ln), 0);
  CHECK_EQ(tasksOf(run(w, "exit")), std::string("gate 9,1"));
  World none = w;
  none.enemies.clear();  // (no platform on it yet)
  CHECK_EQ(lanes(none, 3, 1, ln), 0);
  CHECK(!run(none, "exit").ok);
}

// A $6A spawner ('a'): a flame on it spawns one more $DC, so a lane with no
// platform yet is ridden only after bombing it (4-3: the lane (4,25) to
// (9,25) to the gate side had none; "no plan" for 20000 frames).
TEST("goap: a platform spawner opens the lanes that have no platform") {
  World w = mk({"###########",
                "#..L###L.g#",
                "#.#########",
                "#.a########",
                "###########"}, 1, 1, "gate 9 1 open");
  int ln[4][3];
  CHECK_EQ(lanes(w, 3, 1, ln), 0);
  CHECK_EQ(lanes(w, 3, 1, ln, true), 1);
  CHECK_EQ(tasksOf(run(w, "exit")), std::string("spawner 2,3; gate 9,1"));
  World none = w;
  none.map[size_t(3 * w.cols + 2)] = '#';
  CHECK(!run(none, "exit").ok);
}

// Mode 1 panels toggle per blast: paritySpots gives spots whose blasts,
// XORed, light exactly the panels left (4-3 played on from 4-2 with fire 3:
// every spot by (3,24) also covered a done panel).
// A panel still to light that no spot reaches yet (behind soft blocks)
// waits: the set lights the rest and undoes nothing (4-2 played on with
// fire 10: (24,5)/(12,5) out of reach left no exact set, 12000 frames).
TEST("goap: parity spots: a panel no spot reaches waits") {
  World w = mk({"#######",
                "##...g#",
                "##.#P##",
                "##.P.##",
                "##P#P##",
                "##.P.##",
                "##P#.##",
                "##.P.##",
                "#######"}, 2, 1, "gate 5 1 closed\npanel 4 2 done mode=1\npanel 3 3 done mode=1\npanel 2 4 done mode=1\npanel 4 4 done mode=1\npanel 3 5 todo mode=1\npanel 2 6 done mode=1\npanel 3 7 todo mode=1\npanel 0 0 todo mode=1");
  w.p.fire = 3;
  std::vector<int> spots;
  for (int k = 0; k < w.cols * w.rows; k++)
    if (canPlaceOn(w.map[size_t(k)])) spots.push_back(k);
  std::vector<int> set = paritySpots(w, spots);
  CHECK(!set.empty());
  if (set.empty()) return;
  std::vector<int> hits(w.panels.size(), 0);
  for (int k : set)
    for (const Cell& x : blastCells(w, k % w.cols, k / w.cols, w.p.fire, false))
      for (size_t i = 0; i < w.panels.size(); i++)
        if (w.panels[i].c == x.c && w.panels[i].r == x.r) hits[i]++;
  for (size_t i = 0; i + 1 < w.panels.size(); i++) CHECK_EQ(int(w.panels[i].done ^ (hits[i] & 1)), 1);
  CHECK_EQ(hits.back(), 0);
}

TEST("goap: parity spots light exactly the panels left") {
  World w = mk({"#######",
                "##...g#",
                "##.#P##",
                "##.P.##",
                "##P#P##",
                "##.P.##",
                "##P#.##",
                "##.P.##",
                "#######"}, 2, 1, "gate 5 1 closed\npanel 4 2 done mode=1\npanel 3 3 done mode=1\npanel 2 4 done mode=1\npanel 4 4 done mode=1\npanel 3 5 todo mode=1\npanel 2 6 done mode=1\npanel 3 7 todo mode=1");
  w.p.fire = 3;
  std::vector<int> spots;
  for (int k = 0; k < w.cols * w.rows; k++)
    if (canPlaceOn(w.map[size_t(k)])) spots.push_back(k);
  std::vector<int> set = paritySpots(w, spots);
  CHECK(!set.empty());
  if (set.empty()) return;
  std::vector<int> hits(w.panels.size(), 0);
  for (int k : set)
    for (const Cell& x : blastCells(w, k % w.cols, k / w.cols, w.p.fire, false))
      for (size_t i = 0; i < w.panels.size(); i++)
        if (w.panels[i].c == x.c && w.panels[i].r == x.r) hits[i]++;
  for (size_t i = 0; i < w.panels.size(); i++) CHECK_EQ(int(w.panels[i].done ^ (hits[i] & 1)), 1);
  // after one of them is bombed, the rest is the answer
  World after = w;
  for (const Cell& x : blastCells(w, set[0] % w.cols, set[0] / w.cols, w.p.fire, false))
    for (Panel& p : after.panels)
      if (p.c == x.c && p.r == x.r) p.done = !p.done;
  std::vector<int> rest(set.begin() + 1, set.end());
  CHECK(paritySpots(after, spots) == rest);
}

// The glove is a power-up to fetch in 5-7 (5-8's form 2 is hurt only by
// thrown bombs; 5-7 hides one at (13,10)), and only there.
// (Since throwing: a glove is wanted on every stage. 5-6 once stalled on
// a detour for one; a power-up that fails 3 times is now left, agent.cpp.)
TEST("goap: a hidden glove is fetched before the exit, unless held") {
  World w = mk({"#######",
                "#..%..#",
                "#.#.#.#",
                "#....g#",
                "#######"}, 1, 1, "gate 5 3 open\nhidden 3 1 kind=7");
  w.stage = 0x56;
  Plan p = run(w, "powerups+exit");
  CHECK(p.ok);
  CHECK(tasksOf(p).find("item 3,1") != std::string::npos);
  World h = w;  // (one held already)
  h.p.glove = 1;
  CHECK(tasksOf(run(h, "powerups+exit")).find("item 3,1") == std::string::npos);
  World o = w;  // (another stage too: thrown bombs)
  o.stage = 0x55;
  CHECK(tasksOf(run(o, "powerups+exit")).find("item 3,1") != std::string::npos);
}


// World 5's light switch: while it makes most of the live $8A/$97 fast
// chasers, a flame on it is worth a detour (5-1, seed 2: three fast $8A,
// 2 HP each, boxed P1 into column 14 till the timer). Right as it is, it is
// left alone; with none of them about, it is nothing.
TEST("goap: the light switch is flipped while it makes the $8A fast") {
  const std::string e8A = "enemy type=8A x=104 y=24 hp=2\n";
  World w = mk({"#########",
                "#.......#",
                "#.#.#.#.#",
                "#..s#..g#",
                "#########"}, 1, 1, "gate 7 3 open\nlight off\nswitch 3 3\n" + e8A);
  CHECK_EQ(switchFlip(w), 1);
  Plan p = run(w, "powerups+exit");
  CHECK(p.ok);
  CHECK(tasksOf(p).find("switch 3,3") != std::string::npos);
  World on = w;
  on.light = "on";  // (odd: the $8A wander)
  CHECK_EQ(switchFlip(on), 0);
  CHECK(tasksOf(run(on, "powerups+exit")).find("switch") == std::string::npos);
  World v = w;  // (a $97 is the other way round)
  v.enemies[0].type = 0x97;
  CHECK_EQ(switchFlip(v), 0);
  World none = w;
  none.enemies.clear();
  CHECK_EQ(switchFlip(none), -1);
}

// A power-up P1 is at the cap of does nothing (C0:98CF bombs stop at 9,
// C0:992B fire at 10): not wanted. 4-2: back over the platform for a fire
// up at fire 10.
TEST("goap: items: a bomb up or fire up at its cap is not wanted") {
  Player p;
  p.fire = 9, p.maxbombs = 8;
  CHECK(wantedItem(kItemFire, p) && wantedItem(kItemFullFire, p) && wantedItem(kItemBomb, p));
  p.fire = 10, p.maxbombs = 9;
  CHECK(!wantedItem(kItemFire, p) && !wantedItem(kItemFullFire, p) && !wantedItem(kItemBomb, p));
  CHECK(wantedItem(kItemHeart, p) && wantedItem(kItemRemote, p));
  // and the powerups goal is met with only those left
  World w = mk({"#######",
                "#.....#",
                "#######"}, 1, 1, "item 5 1 item kind=2");
  w.p.fire = 10;
  CHECK(run(w, "powerups").ok);
  CHECK(run(w, "powerups").steps.empty());
}

// A pierce blast goes on through soft blocks: a panel behind two of them is
// lit from the cell past them (3-0 played on with pierce, fire 10: (15,10)
// behind the softs (14,10), (13,10); the stand search stopped at (14,10):
// no plan from the start, 16000 frames to the timer).
TEST("goap: pierce: a panel behind soft blocks is lit from past them") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/3-0_pierce_panels.txt", w, &err));
  CHECK(w.p.pierce);
  CHECK(run(w, "exit").ok);
}

// With pierce and fire 10 every blast that breaks the soft (11,4) under the
// gate (11,3) goes on to the gate: nothing plans without gate hits, with
// them (a high cost) it does (4-3 played on: no plan, the timer).
TEST("goap: pierce: a plan with gate hits when none without") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/4-3_pierce_gate.txt", w, &err));
  Goal g;
  CHECK(Goal::parse("exit", g, &err));
  Plan p = plan(w, g);
  CHECK(!p.ok && !p.gateHits);
  PlanOptions og;
  og.gateHits = true;
  Plan q = plan(w, g, og);
  CHECK(q.ok && q.gateHits);
}

// The glove: a soft block on an island past a hole is bombed by a throw
// from 3 cells back (user: "throw bombs over holes to the 2nd island").
// No glove: no way there.
TEST("goap: throw: a block on an island, thrown to over the hole") {
  World w = mk({"##########",
                "#...OO.%.#",
                "##########"}, 3, 1);
  w.p.fire = 1;
  CHECK(!run(w, "blocks").ok);
  w.p.glove = 1;
  Plan p = run(w, "blocks");
  CHECK(p.ok);
  CHECK(p.str().find("soft 7,1 from 3,1 thrown r to 6,1") != std::string::npos);
  // From (1,1) a throw left wraps round to (8,1), the island's far side.
  World v = w;
  v.p.x = 1 * 16 + 8;
  v.map[size_t(1 * v.cols + 3)] = '#';
  Plan q = run(v, "blocks");
  CHECK(q.str().find("soft 7,1 from 1,1 thrown l to 8,1") != std::string::npos);
}

// A throw off the map's edge comes back in on the far side.
TEST("goap: throw landing wraps at the map's edges") {
  int c, r;
  throwLand(10, 6, 1, 2, 2, c, r);  // left 3 from column 1
  CHECK(c == 8 && r == 2);
  throwLand(10, 6, 4, 1, 0, c, r);  // up 5 from row 1: off the top, no wrap (5-4: planned up through the HUD)
  CHECK(c == 4 && r == -4);
  throwFrom(10, 6, 8, 2, 2, c, r);
  CHECK(c == 1 && r == 2);
}

// A throw that comes down on a wall bounces on to the next free cell
// (user: "empty, solid, empty ... the 2nd will bounce over it"): from
// (1,1) right it lands on (4,1), a wall, and bounces to (5,1).
TEST("goap: throw: bounces off a wall onto the cell past it") {
  World w = mk({"#########",
                "#.###.%##",
                "#########"}, 1, 1);
  w.p.fire = 1;
  w.p.glove = 1;
  Plan p = run(w, "blocks");
  CHECK(p.ok);
  CHECK(p.str().find("soft 6,1 from 1,1 thrown r to 5,1") != std::string::npos);
  int n = 0;
  throwSources(9, 3, 5, 1, 3, [&](int c, int r) { return w.at(c, r) == '.'; }, [&](int xc, int xr, int hops) {
    if (hops == 1) CHECK(xc == 1 && xr == 1);
    n++;
  });
  CHECK(n >= 2);
}

