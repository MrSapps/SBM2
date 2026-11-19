// The agent (planner/agent.cpp) with a fake oracle, and the snapshot text
// (planner/capture.cpp). No emulator: the oracle's answers are made up per
// test, so these pin the decision rules, not the game.
#include <fstream>
#include <functional>
#include <sstream>

#include "agent.hpp"
#include "blast.hpp"
#include "capture.hpp"
#include "enemies.hpp"
#include "agent_api.h"
#include "test.hpp"

using namespace sbm2;

namespace {

struct FakeOracle : Oracle {
  std::function<void(const SimReq&, SimOut&)> answer;
  std::vector<SimReq> seen;
  void run(const std::vector<SimReq>& reqs, std::vector<SimOut>& out) override {
    out.clear();
    for (const SimReq& r : reqs) {
      seen.push_back(r);
      SimOut o;
      o.pads.assign(size_t(r.frames + 1), 0);
      o.trace.assign(size_t(2 * (r.frames + 1)), 0);
      // P1 ends where the move ends.
      const auto& last = !r.path2.empty() ? r.path2 : r.path;
      if (last.size() >= 2) o.px = last[last.size() - 2] & ~kWpFlags, o.py = last.back();
      answer(r, o);
      out.push_back(o);
    }
  }
};

World mk(const std::vector<std::string>& rows, int pc, int pr, const std::string& extra = "") {
  std::string s = "worldview 1\nsize " + std::to_string(rows[0].size()) + " " + std::to_string(rows.size()) +
                  "\nplayer x=" + std::to_string(pc * 16 + 8) + " y=" + std::to_string(pr * 16 + 8) + " fire=2\n" +
                  extra + "\nmap\n";
  for (auto& r : rows) s += r + "\n";
  World w;
  std::string err;
  if (!parseWorld(s, w, &err)) std::printf("  mk: %s\n", err.c_str());
  return w;
}

Live liveOf(const World& w) {
  Live l;
  l.inStage = true;
  l.px = w.p.x;
  l.py = w.p.y;
  l.enemies = int(w.enemies.size());
  l.hp = l.enemies;
  l.mapSig = 1;
  return l;
}

Agent agentFor(const char* goal) {
  Goal g;
  Goal::parse(goal, g);
  return Agent(g);
}

// The bomb cell and the cover cell of a bomb move.
void bombAndCover(const SimReq& r, int& bc, int& br, int& yc, int& yr) {
  bc = r.path[r.path.size() - 2] / 16;
  br = r.path.back() / 16;
  yc = r.path2[r.path2.size() - 2] / 16;
  yr = r.path2.back() / 16;
}

}  // namespace

TEST("agent: open gate: walk in, and only leaving by it counts") {
  World w = mk({"#######",
                "#.....#",
                "#..g..#",
                "#######"}, 1, 1, "gate 3 2 open");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; };  // lives but never leaves
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  CHECK_EQ(a.what(), std::string("survive"));
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -2; };
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK_EQ(a.what(), std::string("gate 3,2: walk to (3,2)"));
  // P1's own cell first (line up), then cell by cell to the gate.
  const SimReq& r = o.seen.back();
  CHECK_EQ(r.path.size(), size_t(2 * 4));
  CHECK_EQ(r.path[0], int16_t(24));
  CHECK_EQ(r.path.back(), int16_t(40));
}

// A bomb move: the cover is out of the new bomb's blast, and a move that
// lives but changes nothing (the block still there) is not taken.
TEST("agent: soft block: bomb next to it, cover out of the blast") {
  World w = mk({"#########",
                "#...%...#",
                "#.#.#.#.#",
                "#########"}, 1, 1);
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 1; };
  Agent a = agentFor("blocks");
  a.decide(w, liveOf(w), o);
  CHECK_EQ(a.what(), std::string("survive"));
  o.seen.clear();
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(!o.seen.empty());
  const SimReq& r = o.seen[0];
  CHECK(r.post.size() == 2 && r.post[0] == kPadA);
  int bc, br, yc, yr;
  bombAndCover(r, bc, br, yc, yr);
  CHECK(bc == 2 && br == 1);  // the nearest cell whose blast (range 2) reaches it
  for (const Cell& x : blastCells(w, bc, br, w.p.fire, false)) CHECK(!(x.c == yc && x.r == yr));
}

TEST("agent: every move dies: take the one that lives longest") {
  World w = mk({"#######",
                "#.....#",
                "#######"}, 1, 1, "gate 5 1 closed\nenemy type=80 x=88 y=24");
  FakeOracle o;
  o.answer = [](const SimReq& q, SimOut& r) { r.dead = 10 + int(q.path.size()) * 4; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK_EQ(a.what(), std::string("survive"));
}

// 1-1, frame 110: P1's bomb is ticking. No second bomb by it: P1 waits it
// out, and the first cell it tries is out of the blast.
TEST("agent: 1-1: own bomb ticking: wait in cover") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/1-1_bomb.txt", w, &err));
  CHECK(!w.bombs.empty());
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; r.enemies = 0; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK_EQ(a.what(), std::string("survive"));
  CHECK(!o.seen.empty());
  for (const SimReq& r : o.seen) CHECK(r.post.empty());
  const SimReq& r = o.seen[0];
  int yc = r.path[r.path.size() - 2] / 16, yr = r.path.back() / 16;
  CHECK(!DangerMap(w).deadly(yc, yr, 0, 400));
}

// 1-2: P1 starts in a pocket shut by a barrel ($D6). Before barrels were
// breakable the planner found no plan and P1 waited out the clock.
TEST("agent: 1-2 start: boxed in by a barrel, the plan bombs through it") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/1-2_barrel.txt", w, &err));
  Plan p = plan(w, Goal{true, false, false, false});
  CHECK(p.ok);
  CHECK(!p.steps.empty() && p.tasks[size_t(p.steps.back().task)].kind == TaskKind::EnterGate);
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; r.enemies = 0; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("bomb at") != std::string::npos);
}

// 1-8: the boss body (11 HP, 5x5 sense points) swings along row 4. The
// plan's step is to hurt it; the bomb never goes on a sense cell (eaten)
// and its blast reaches one when it goes off.
TEST("agent: 1-8 boss: bomb next to the sense points, not on them") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/1-8_boss.txt", w, &err));
  CHECK_EQ(w.bosses.size(), size_t(3));
  Plan p = plan(w, Goal{true, false, false, false});
  CHECK(p.ok && !p.steps.empty() && p.tasks[size_t(p.steps[0].task)].kind == TaskKind::HurtBoss);
  const BossObj* body = nullptr;
  for (const BossObj& b : w.bosses)
    if (!b.sense.empty()) body = &b;
  CHECK(body != nullptr);
  if (!body) return;
  FakeOracle o;
  o.answer = [&](const SimReq& q, SimOut& r) {
    r.dead = -1;
    r.bossHp = q.wantBoss ? 11 : 10;  // (every option hits: the agent's pick is checked below)
    if (q.wantBoss) {  // the body stays put
      r.btrace.assign(size_t(2 * q.bossBase.size() * size_t(q.frames + 1)), 0);
      for (size_t f = 0; f <= size_t(q.frames); f++)
        for (size_t b = 0; b < q.bossBase.size(); b++) {
          r.btrace[2 * (f * q.bossBase.size() + b)] = int16_t(w.bosses[b].x);
          r.btrace[2 * (f * q.bossBase.size() + b) + 1] = int16_t(w.bosses[b].y);
        }
    }
  };
  Agent a = agentFor("exit");
  Live l = liveOf(w);
  l.bossHp = 11;
  a.decide(w, l, o);
  CHECK(a.committed());
  CHECK(a.what().find("boss") != std::string::npos);
  const SimReq& r = o.seen.back();
  CHECK(!r.post.empty());
  int bc = r.path[r.path.size() - 2] / 16, br = r.path.back() / 16;
  bool onSense = false, reaches = false;
  auto blast = blastCells(w, bc, br, w.p.fire, w.p.pierce);
  for (auto& sp : body->sense) {
    int c = (body->x + sp.first) >> 4, rr = (body->y + sp.second) >> 4;
    if (c == bc && rr == br) onSense = true;
    for (const Cell& x : blast)
      if (x.c == c && x.r == rr) reaches = true;
  }
  CHECK(!onSense);
  CHECK(reaches);
}

TEST("snapshot text: encode(parse(file)) gives the file back (worldview.lua's format)") {
  for (const char* name : {"1-1_bomb.txt", "1-2_barrel.txt", "s3_36_m300.txt", "1-8_boss.txt"}) {
    std::string path = sbm2test::worldsDir + "/" + name, err;
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    World w;
    CHECK(parseWorld(ss.str(), w, &err));
    std::string text = encodeWorld(w);
    if (text != ss.str()) std::printf("  %s: re-encoded text differs\n", name);
    CHECK(text == ss.str());
  }
}

namespace {
// 1-8's boss, standing still; every run that isn't the boss probe ends with
// the boss at `hpAfter`.
World boss18() {
  World w;
  std::string err;
  loadWorld(sbm2test::worldsDir + "/1-8_boss.txt", w, &err);
  return w;
}
void bossAnswers(FakeOracle& o, const World& w, int hpNow, int hpAfter) {
  o.answer = [&w, hpNow, hpAfter](const SimReq& q, SimOut& r) {
    r.dead = -1;
    r.bossHp = q.wantBoss ? hpNow : hpAfter;
    if (q.wantBoss) {
      r.btrace.assign(size_t(2 * q.bossBase.size() * size_t(q.frames + 1)), 0);
      for (size_t f = 0; f <= size_t(q.frames); f++)
        for (size_t b = 0; b < q.bossBase.size(); b++) {
          r.btrace[2 * (f * q.bossBase.size() + b)] = int16_t(w.bosses[b].x);
          r.btrace[2 * (f * q.bossBase.size() + b) + 1] = int16_t(w.bosses[b].y);
        }
    }
  };
}
}  // namespace

// 1-7: P1 is frozen ($7E5E38 PlayerCutsceneLock) for ~200 frames while the
// $A5 walks in. Plans made then were all mistimed (the A press came 200
// frames late); the agent now sits the lock out.
TEST("agent: the cutscene lock ($5E38) counts as locked") {
  std::vector<uint8_t> wram(0x20000, 0);
  wram[0x1C000] = 0xC3;                     // a stage tile set
  wram[0xBC] = 1;
  wram[0x8000] = 0x00, wram[0x8001] = 0x90;  // P1 live
  CHECK(readLive(wram.data()).inStage);
  CHECK(!readLive(wram.data()).locked);
  wram[0x5E38] = 1;
  CHECK(readLive(wram.data()).locked);
}

// 1-8: while the body leaps it senses nothing and its boss line has no
// sense points. Before, the goal vanished ("no plan") and P1 wandered.
TEST("agent: boss sensing nothing: last points kept, P1 only gets in place") {
  World w = boss18();
  Agent a = agentFor("exit");
  a.noteBosses(w);
  CHECK(!a.senseStale());
  World leap = w;
  for (BossObj& b : leap.bosses) b.sense.clear();
  a.noteBosses(leap);
  CHECK(a.senseStale());
  CHECK_EQ(leap.bosses[1].sense.size(), w.bosses[1].sense.size());
  Plan p = plan(leap, Goal{true, false, false, false});
  CHECK(p.ok && !p.steps.empty() && p.tasks[size_t(p.steps[0].task)].kind == TaskKind::HurtBoss);
  FakeOracle o;
  bossAnswers(o, leap, 11, 10);  // (a hit would "work": it must not be tried)
  Live l = liveOf(leap);
  l.bossHp = 11;
  a.decide(leap, l, o);
  for (const SimReq& r : o.seen) CHECK(r.post.empty());  // no bombs
  CHECK(a.what().find("get in place") != std::string::npos);
}

// 4-8 in the world 4 run: P1 (fire 6, the remote) pinned on the top row,
// the robot below; no timed hit lives. With the remote: lay a trap (no B
// in the run) and take cover; frame() sets it off on a sense point.
TEST("agent: boss with no timed hit and the remote: lay a trap") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/4-8_robot_pinned.txt", w, &err));
  CHECK(w.p.remote && !w.bosses.empty());
  Plan p = plan(w, Goal{true, false, false, false});
  CHECK(p.ok && !p.steps.empty() && p.tasks[size_t(p.steps[0].task)].kind == TaskKind::HurtBoss);
  FakeOracle o;
  bossAnswers(o, w, 3, 3);  // (no hit ever: the boss stays put)
  auto inner = o.answer;
  o.answer = [inner](const SimReq& q, SimOut& r) {
    inner(q, r);
    if (q.detEnd > 0) r.dead = 5;  // (a B in the run: the blast is mistimed)
  };
  Agent a = agentFor("exit");
  Live l = liveOf(w);
  l.bossHp = 3;
  a.decide(w, l, o);
  CHECK(a.committed());
  CHECK(a.what().find("trap for") != std::string::npos);
  CHECK(!o.seen.empty() && !o.seen.back().post.empty() && o.seen.back().detEnd == 0);
}

// 4-8 in the whole game (speed 5): P1 coasts ~3 px past the cover's centre
// (a press counts a frame late). Every hit lived and hurt the robot, but
// "P1 ends at the cover" wanted 2 px: no hit for 340000 frames.
TEST("agent: boss hit whose run ends 3 px off the cover's centre") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/4-8_robot_fast.txt", w, &err));
  CHECK(w.p.e0 == 5 && !w.bosses.empty());
  FakeOracle o;
  bossAnswers(o, w, 8, 7);
  auto inner = o.answer;
  o.answer = [inner](const SimReq& q, SimOut& r) {
    inner(q, r);
    r.px -= 3, r.py -= 1;
  };
  Agent a = agentFor("exit");
  Live l = liveOf(w);
  l.bossHp = 8;
  a.decide(w, l, o);
  CHECK(a.committed());
  CHECK(a.what().find("boss") != std::string::npos);
  CHECK(a.what().find("bomb at") != std::string::npos);
}

// Soft blocks grow back (C0:D8E8): with the countdown out, the first empty
// listed cell with nothing on or next to it grows at once. A cover whose
// walk crosses such a gap after P1 has left its side goes last (5-1: P1
// beside (4,4) held it open, walked off to push (8,6), and (4,4) grew back
// between P1 and every cover).
TEST("agent: a cover past a gap that grows back goes last") {
  World w = mk({"##########",
                "#.......%#",
                "####.#####",
                "##########"}, 6, 1, "regrow in=0 cells=3,1;8,1");
  CHECK_EQ(w.regrowCells.size(), size_t(2));
  CHECK_EQ(w.regrowIn, 0);
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; r.enemies = 0; };
  Agent a = agentFor("blocks");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(!o.seen.empty());
  int bc, br, yc, yr;
  bombAndCover(o.seen.front(), bc, br, yc, yr);
  CHECK_EQ(yc, 4);
  CHECK_EQ(yr, 2);
  // No countdown due (a long one): the nearer index comes first again.
  World l = w;
  l.regrowIn = 2000;
  FakeOracle o2;
  o2.answer = o.answer;
  Agent b = agentFor("blocks");
  b.decide(l, liveOf(l), o2);
  CHECK(!o2.seen.empty());
  bombAndCover(o2.seen.front(), bc, br, yc, yr);
  CHECK_EQ(yc, 3);
  CHECK_EQ(yr, 1);
}

// With a bomb of P1's already out, a new bomb counts only if the run shows
// both hits; and the move is played only until P1 is in cover (the next
// bomb is planned while this one ticks).
TEST("agent: boss: a bomb already out must hit too; play to cover only") {
  World w = boss18();
  Bomb b;
  b.c = 2, b.r = 2, b.fuse = 100, b.range = 5;
  w.bombs.push_back(b);
  Live l = liveOf(w);
  l.bossHp = 11;
  {
    FakeOracle o;
    bossAnswers(o, w, 11, 10);  // one hit: the bomb already out's
    Agent a = agentFor("exit");
    a.decide(w, l, o);
    CHECK(a.what().find("(+1 out)") == std::string::npos || !a.committed());
    CHECK(a.what().find("bomb at") == std::string::npos);
  }
  {
    FakeOracle o;
    bossAnswers(o, w, 11, 9);  // both hit
    Agent a = agentFor("exit");
    a.decide(w, l, o);
    CHECK(a.committed());
    CHECK(a.what().find("(+1 out)") != std::string::npos);
    const SimReq* hit = nullptr;
    for (const SimReq& r : o.seen)
      if (!r.post.empty()) hit = &r;
    CHECK(hit != nullptr);
    if (hit) {
      int cells = int(hit->path.size() / 2 + hit->path2.size() / 2) - 2;
      CHECK(a.pending() <= 22 * cells + 2 * 20 + int(hit->pre.size()));  // to cover, not through the blast
      CHECK(a.pending() < hit->frames - 150);
    }
  }
}

// 1-5 in the full world-1 run: the soft block before panel (28,3) can only
// be bombed from (24,3), and the game won't take a bomb there (an object
// has it as its target: ObjectOnPlayerCell). Every run lived and changed
// nothing, 271 times. Now a cell that fails 3 times is dropped and the
// route goes another way.
TEST("agent: a bomb cell the game refuses: dropped after 3 tries, routed around") {
  // P1 (1,1) must reach the gate (7,1); soft blocks at (3,1) and (3,3).
  // The game refuses every bomb on row 1 (the cells that reach (3,1)), so
  // the way is through (3,3), bombed from (2,3).
  World w = mk({"#########",
                "#..%...g#",
                "#.##.####",
                "#..%.####",
                "#########"}, 1, 1, "gate 7 1 open");
  FakeOracle o;
  int refused = 0;
  o.answer = [&](const SimReq& q, SimOut& r) {
    r.dead = -1;
    r.mapSig = 1;
    if (q.post.empty()) return;
    if (q.path.back() / 16 == 1) refused++;
    else r.mapSig = 2;
  };
  Agent a = agentFor("exit");
  for (int k = 0; k < 6; k++) {
    a.decide(w, liveOf(w), o);
    if (a.what().find("bomb at") != std::string::npos) break;
  }
  CHECK(refused > 0);
  CHECK(a.committed());
  CHECK(a.what().find("bomb at (2,3)") != std::string::npos);
}

// Bomb (1,2) is out and will break (1,1). With a bomb to spare P1 goes on
// to the next block at once (not (1,1) again) and plays only up to cover;
// with none left it waits.
TEST("agent: a bomb out, one to spare: the next block, played to cover only; none left: wait") {
  World w = mk({"###########",
                "#%........#",
                "#b#.#.#.#.#",
                "#.....%...#",
                "###########"}, 3, 1, "bomb c=1 r=2 fuse=100 range=2 owner=p1");
  w.p.bombs = 2;
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("blocks");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("soft 6,3") == 0);
  CHECK(a.pending() < o.seen.back().frames - 150);  // not the fuse and flames
  // No spare bomb: the whole blast is played.
  w.p.bombs = 1;
  Agent b = agentFor("blocks");
  b.decide(w, liveOf(w), o);
  CHECK(b.what().find("soft 6,3") == 0);
  CHECK(b.pending() > 150);
  // None left: wait, no bomb pressed.
  w.p.bombs = 0;
  o.seen.clear();
  Agent c = agentFor("blocks");
  c.decide(w, liveOf(w), o);
  CHECK_EQ(c.what(), std::string("survive"));
  for (const SimReq& r : o.seen) CHECK(r.post.empty());
}

// A hidden bomb up: the agent bombs its block open (powerups goal).
TEST("agent: powerups: bomb open the block hiding a bomb up") {
  World w = mk({"#######",
                "#..%..#",
                "#.#.#.#",
                "#######"}, 1, 1, "hidden 3 1 kind=0");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("clear the way to item 3,1") == 0);
}

// A power-up whose moves keep failing (P1 can't get there) is optional:
// after 3 failed decisions it is left and the goal goes on (1-2: before,
// P1 waited out the whole clock for a bomb up behind (8,3)).
TEST("agent: a power-up it can't get: left after 3 tries") {
  World w = mk({"#######",
                "#..%..#",
                "#.#.#.#",
                "#.....g",
                "#######"}, 1, 1, "hidden 3 1 kind=0\ngate 6 3 open");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -2; };  // walks still end by the gate; bombs change nothing
  Agent a = agentFor("powerups+exit");
  for (int i = 0; i < 3; i++) {
    a.decide(w, liveOf(w), o);
    CHECK(a.what() == "survive");
  }
  a.decide(w, liveOf(w), o);
  CHECK_EQ(a.what(), std::string("gate 6,3: walk to (6,3)"));
}

// With the remote, a pierce item on the floor is a wall to the routes.
TEST("agent: holding the remote: walks around a pierce item") {
  World w = mk({"#######",
                "#..i.g#",
                "#.###.#",
                "#.....#",
                "#######"}, 1, 1, "gate 5 1 open\nitem 3 1 item kind=1");
  auto through = [](const SimReq& r) {
    for (size_t k = 0; k + 1 < r.path.size(); k += 2)
      if (r.path[k] / 16 == 3 && r.path[k + 1] / 16 == 1) return true;
    return false;
  };
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -2; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed() && !through(o.seen.back()));  // no remote: still around (pierce is kept off)
  w.p.pierce = true;
  o.seen.clear();
  Agent p = agentFor("exit");
  p.decide(w, liveOf(w), o);
  CHECK(p.committed() && through(o.seen.back()));  // holding pierce: straight on
  w.p.pierce = false;
  w.p.remote = true;
  o.seen.clear();
  Agent b = agentFor("exit");
  b.decide(w, liveOf(w), o);
  CHECK(b.committed() && !o.seen.empty() && !through(o.seen.back()));
}

// Every bomb move ranks its covers by the enemy model: of two covers as
// near, the one an enemy is walking into goes last.
TEST("agent: a block's bomb: the cover an enemy is heading for goes last") {
  World w = mk({"#########",
                "#...%...#",
                "#.#.#####",
                "#.#.....#",
                "#########"}, 3, 1,
               "enemy slot=4 type=94 x=24 y=55 vx=0 vy=-0.5 step=u target=1,2 wait=0 hp=1 f4=0C00 c0=8000");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("blocks");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  int bc, br, yc, yr;
  bombAndCover(o.seen[0], bc, br, yc, yr);
  CHECK(bc == 3 && br == 1);
  CHECK(yr == 3 && yc >= 4);  // along row 3, not up column 1 where it walks
}

// World 2: a blown block grows back within the run, so the end map can
// match the start; the oracle's "changed on the way" still counts.
TEST("agent: a block that grows back in the run still counts as done") {
  World w = mk({"#########",
                "#...%...#",
                "#.#.#.#.#",
                "#########"}, 1, 1);
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 1; r.mapChanged = true; };
  Agent a = agentFor("blocks");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed() && a.what().find("soft 4,1") == 0);
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 1; };
  Agent b = agentFor("blocks");
  b.decide(w, liveOf(w), o);
  CHECK_EQ(b.what(), std::string("survive"));
}

// World 2's blocks grow back ~190 frames after they break: a block's bomb
// is played to the flames' end by the run's real press (A), or the remote's
// set-off (B), not by the pessimistic walk budget (it waited in cover so
// long the block it opened was back).
TEST("agent: a bomb move ends when its flames do, by the real press") {
  World w = mk({"###########",
                "#........%#",
                "#.#.#.#.#.#",
                "###########"}, 1, 1);
  FakeOracle o;
  o.answer = [](const SimReq& q, SimOut& r) {
    r.dead = -1;
    r.mapSig = 2;
    if (!q.post.empty()) r.pads[10] |= kPadA;
  };
  Agent a = agentFor("blocks");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK_EQ(a.pending(), 10 + 2 + 150 + kFlameFrames + 4);
  // The remote: 30 frames past the set-off, then the flames.
  w.p.remote = true;
  o.answer = [](const SimReq& q, SimOut& r) {
    r.dead = -1;
    r.mapSig = 2;
    if (!q.post.empty()) r.pads[10] |= kPadA, r.pads[40] |= kPadB, r.pads[41] |= kPadB;
  };
  Agent b = agentFor("blocks");
  b.decide(w, liveOf(w), o);
  CHECK(b.committed());
  CHECK_EQ(b.pending(), 41 + 30 + kFlameFrames);
}

// A flame on the gate brings enemies back; one on an item burns it. No
// bomb cell whose blast reaches either (a kill near the gate, a heart).
TEST("agent: no bomb whose blast reaches the gate, a reachable item or a done panel") {
  World w = mk({"###########",
                "#g...P....#",
                "#.#.#.#.#.#",
                "#.........#",
                "###########"}, 1, 3, "gate 1 1 closed\nitem 3 3 heart kind=11\npanel 5 1 done mode=1\n" +
                std::string("enemy type=80 x=") + std::to_string(3 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8) + "\n");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 1; r.enemies = r.hp = 1; };  // (no kill: every option is tried)
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  int bombs = 0, onItem = 0;
  for (const SimReq& r : o.seen) {
    if (r.post.empty()) continue;
    bombs++;
    int bc, br, yc, yr;
    bombAndCover(r, bc, br, yc, yr);
    // (on the item's own cell is fine: P1 picks it up placing the bomb)
    onItem += bc == 3 && br == 3;
    for (const Cell& x : blastCells(w, bc, br, w.p.fire, false)) {
      CHECK(!(x.c == 1 && x.r == 1));
      CHECK(!(x.c == 3 && x.r == 3) || (bc == 3 && br == 3));
      CHECK(!(x.c == 5 && x.r == 1));  // (the done panel)
    }
  }
  CHECK(bombs > 0);
  CHECK(onItem > 0);
}

// B sets off the oldest remote bomb only (docs/mechanics.md 10): press when
// an enemy is in its blast (and its chain's), P1 out of it.
TEST("agent: remote trap: set off the oldest when an enemy is in its blast") {
  World w = mk({"#########",
                "#.......#",
                "#.#.#.#.#",
                "#.......#",
                "#########"}, 1, 3,
               "bomb c=3 r=1 fuse=0 range=2 owner=p1 remote=1\nbomb c=7 r=3 fuse=0 range=2 owner=p1 remote=1\n"
               "enemy type=80 x=" + std::to_string(5 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8));
  std::vector<int> order;
  noteRemotes(w, order);
  CHECK_EQ(order.size(), size_t(2));
  CHECK(remoteTrigger(w, order));  // (5,1) is 2 right of (3,1)
  // The newer one first in line: its blast doesn't reach (5,1).
  std::vector<int> rev{order[1], order[0]};
  CHECK(!remoteTrigger(w, rev));
  // P1 in the blast: no.
  World p = w;
  p.p.x = 3 * 16 + 8, p.p.y = 2 * 16 + 8;
  CHECK(!remoteTrigger(p, order));
  // Nobody in it: no. The gate in it: no.
  World e = w;
  e.enemies[0].x = 7 * 16 + 8, e.enemies[0].y = 1 * 16 + 8;
  CHECK(!remoteTrigger(e, order));
  World gte = w;
  gte.hasGate = true, gte.gateC = 1, gte.gateR = 1;
  CHECK(!remoteTrigger(gte, order));
  World jp = w;  // (a jump pad is no enemy)
  jp.enemies[0].type = 0xA0;
  CHECK(!remoteTrigger(jp, order));
  // Without an enemy (a stale trap): only P1, the gate and items clear.
  CHECK(remoteTrigger(e, order, nullptr, false));
  CHECK(!remoteTrigger(p, order, nullptr, false));
  CHECK(!remoteTrigger(gte, order, nullptr, false));
  // A bomb gone (set off) leaves the order; a new one goes last.
  World x = w;
  x.bombs.erase(x.bombs.begin());
  noteRemotes(x, order);
  CHECK_EQ(order.size(), size_t(1));
  CHECK_EQ(order[0], 3 * w.cols + 7);
}

// A boss is a trap's target too: a point it senses flames under (this
// frame) in the oldest remote's blast. Not while it is hurt (no flame is
// sensed) or senses nothing (4-8's robot with fire 6: no fused bomb's
// timing lived, a trap set off on it did).
TEST("agent: remote trap: set off when a boss sense point is in its blast") {
  World w = mk({"#########",
                "#.......#",
                "#.#.#.#.#",
                "#.......#",
                "#########"}, 1, 3, "bomb c=3 r=1 fuse=0 range=2 owner=p1 remote=1");
  std::vector<int> order;
  noteRemotes(w, order);
  CHECK(!remoteTrigger(w, order));
  BossObj b;
  b.x = 5 * 16 + 8, b.y = 1 * 16 + 8, b.hp = 3;
  b.sense = {{0, 0}};
  w.bosses.push_back(b);
  CHECK(remoteTrigger(w, order));
  World h = w;
  h.bosses[0].hurt = true;
  CHECK(!remoteTrigger(h, order));
  World n = w;
  n.bosses[0].sense.clear();
  CHECK(!remoteTrigger(n, order));
  World f = w;  // (out of reach: (7,1) is 4 from (3,1))
  f.bosses[0].x = 7 * 16 + 8;
  CHECK(!remoteTrigger(f, order));
}

// With the remote and a bomb to spare, a kill lays a trap (no B in the run)
// where the model says enemies walk; a bomb move with traps out presses B
// once per older bomb, so its own goes off.
TEST("agent: remote: lay a trap for a kill; later moves press B past the traps") {
  World w = mk({"###########",
                "#.........#",
                "#.#.#.#.#.#",
                "#.........#",
                "###########"}, 1, 3,
               "enemy slot=5 type=80 x=" + std::to_string(7 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8) +
                   " vx=-0.5 vy=0 step=l target=6,1 hp=1 spd=0.5");
  w.p.remote = true;
  w.p.bombs = 3;
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 1; r.enemies = r.hp = 1; };
  Agent a = agentFor("enemies");
  a.decide(w, liveOf(w), o);
  CHECK(a.what().find("trap for kill") == 0);
  bool sawTrap = false;
  for (const SimReq& r : o.seen)
    if (!r.post.empty()) {
      CHECK_EQ(r.detEnd, 0);
      sawTrap = true;
    }
  CHECK(sawTrap);
  // The trap is out, none to spare: wait for it (no bomb pressed, no B).
  World t = w;
  t.bombs.push_back({});
  t.bombs.back().c = 3, t.bombs.back().r = 3, t.bombs.back().remote = true, t.bombs.back().range = 2;
  t.p.bombs = 1;
  t.frame = 100;
  o.seen.clear();
  a.decide(t, liveOf(t), o);
  CHECK_EQ(a.what(), std::string("survive"));
  for (const SimReq& r : o.seen) CHECK(r.post.empty() && r.detEnd == 0);
  // Waiting is no reason to set it off; past kTrapWait it goes (P1 clear).
  World tq = t;
  tq.p.x = 1 * 16 + 8, tq.p.y = 1 * 16 + 8;
  Agent idle = agentFor("enemies");
  CHECK_EQ(idle.trapCheck(tq), 2);  // (never laid one: stale)
  CHECK_EQ(a.trapCheck(tq), 0);
  tq.frame = 100 + kTrapWait;
  CHECK_EQ(a.trapCheck(tq), 2);
  // Walled off from the enemy, a trap out: nothing to do but survive, and
  // the trap may be in the way: set it off as soon as P1 is clear.
  World s2 = mk({"###########",
                 "#.....#.#.#",
                 "#.#.#.###.#",
                 "#.........#",
                 "###########"}, 1, 3,
                "enemy slot=5 type=80 x=" + std::to_string(7 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8) +
                    " vx=0 vy=0 hp=1 spd=0.5\nbomb c=3 r=3 fuse=0 range=2 owner=p1 remote=1");
  s2.p.remote = true;
  s2.p.bombs = 1;
  s2.frame = 100;
  Agent st = agentFor("enemies");
  o.seen.clear();
  st.decide(s2, liveOf(s2), o);
  // P1 starts in the trap's blast (row 3): the first way out tried leaves it.
  CHECK(!o.seen.empty() && o.seen[0].path.size() >= 2);
  int ex = o.seen[0].path[o.seen[0].path.size() - 2] / 16, ey = o.seen[0].path.back() / 16;
  auto inB = [](int c, int r) { return (r == 3 && c >= 1 && c <= 5) || (c == 3 && r >= 1 && r <= 3); };
  CHECK(!inB(ex, ey) && !inB(ex - 1, ey) && !inB(ex + 1, ey) && !inB(ex, ey - 1) && !inB(ex, ey + 1));  // (nor next to it)
  CHECK_EQ(st.what(), std::string("survive"));
  CHECK(st.takeLog().find("traps out") == std::string::npos);
  CHECK(st.trapStuck());
  CHECK(!a.trapStuck());
  // Two traps out (5-6, seed 1): the way out leaves both blasts, not just
  // the oldest's (in the newer one's, B is never pressed).
  World s3 = s2;
  s3.bombs.push_back({});
  s3.bombs.back().c = 9, s3.bombs.back().r = 3, s3.bombs.back().remote = true, s3.bombs.back().range = 2;
  s3.p.x = 8 * 16 + 8;
  Agent st3 = agentFor("enemies");
  st3.trapCheck(s2);  // (the (3,3) one is the oldest)
  o.seen.clear();
  st3.decide(s3, liveOf(s3), o);
  CHECK(!o.seen.empty() && o.seen[0].path.size() >= 2);
  int fx = o.seen[0].path[o.seen[0].path.size() - 2] / 16, fy = o.seen[0].path.back() / 16;
  auto inB2 = [&](int c, int r) { return inB(c, r) || (r == 3 && c >= 7 && c <= 9) || (c == 9 && r >= 1 && r <= 3); };
  CHECK(!inB2(fx, fy));
  // A block with one trap out: B twice.
  World b = mk({"#########",
                "#...%...#",
                "#.#.#.#.#",
                "#.......#",
                "#########"}, 1, 1, "bomb c=7 r=3 fuse=0 range=2 owner=p1 remote=1");
  b.p.remote = true;
  b.p.bombs = 1;
  o.seen.clear();
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent c = agentFor("blocks");
  c.decide(b, liveOf(b), o);
  CHECK(c.committed());
  CHECK(!o.seen.empty() && o.seen[0].detEnd == 2);
}

// A route over a spring pad: walk onto the pad, then the landing (the path
// follower holds that way through the lock and the flight). Held by the
// pad, P1 is not "locked": the agent decides (nothing held bounces in place).
TEST("agent: spring pads: a walk jumps; held by a pad is no lock") {
  World w = mk({"#########",
                "#...S...#",
                "#########",
                "#.......#",
                "#.......#",
                "#...i...#",
                "#########"}, 1, 1, "item 4 5 item kind=2");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; };
  Agent a = agentFor("items");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  bool jumped = false;
  for (const SimReq& q : o.seen)
    for (size_t i = 0; i + 3 < q.path.size(); i += 2)
      jumped |= q.path[i] == 4 * 16 + 8 && q.path[i + 1] == 1 * 16 + 8 && (q.path[i + 2] & ~kJumpHold) == 4 * 16 + 8 &&
                q.path[i + 3] >= 4 * 16 + 8;  // (let go to (4,4) or held to (4,5))
  CHECK(jumped);
  // Nothing to do (no gate): staying alive never ends a move on the pad.
  World nx = w;
  nx.p.x = 3 * 16 + 8;
  Agent b = agentFor("exit");
  o.seen.clear();
  o.answer = [](const SimReq&, SimOut& r) { r.dead = 5; };  // (every way tried)
  b.decide(nx, liveOf(nx), o);
  CHECK(!o.seen.empty());
  for (const SimReq& q : o.seen)
    CHECK(!(q.path.size() >= 2 && q.path[q.path.size() - 2] == 4 * 16 + 8 && q.path.back() == 1 * 16 + 8));
  std::vector<uint8_t> wram(size_t(kWramSize), 0);
  wram[0x1C000] = 0xC3;  // (a stage: hard wall at (0,0), P1 object live)
  wram[0xBC] = 1;
  wram[0x8001] = 0x90;
  wram[0x8010] = 4 * 16 + 8, wram[0x8018] = 1 * 16 + 8;
  wram[0x8052] = 0x01, wram[0x8053] = 0x08;
  wram[0x1C000 + 1 * 0x80 + 4 * 2] = 0x4C;
  CHECK(readLive(wram.data()).inStage);
  CHECK(!readLive(wram.data()).locked);
  wram[0x1C000 + 1 * 0x80 + 4 * 2] = 0x00;
  CHECK(readLive(wram.data()).locked);
  wram[0x1C000 + 1 * 0x80 + 4 * 2] = 0x4C, wram[0x8053] = 0x18;  // (in the air)
  CHECK(readLive(wram.data()).locked);
}

// A trap on a gap a soft block left (it may grow back; the bomb keeps it
// open) ranks over an equally likely cell nearer P1.
TEST("agent: remote trap: a gap a block left ranks higher") {
  auto world = [](char gap) {
    World w = mk({"###########",
                  "#.........#",
                  "#.#######.#",
                  "#.........#",
                  "###########"}, 1, 1,
                 "enemy slot=5 type=80 x=" + std::to_string(5 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8) +
                     " vx=0 vy=0 hp=1 spd=0");
    w.map[size_t(1 * w.cols + 7)] = gap;
    w.p.remote = true;
    w.p.bombs = 3;
    return w;
  };
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 1; r.enemies = r.hp = 1; };
  Agent plain = agentFor("enemies");
  plain.decide(world('.'), liveOf(world('.')), o);
  CHECK(plain.what().find("trap for kill") == 0);
  CHECK(plain.what().find("bomb at (7,1)") == std::string::npos);
  Agent seen = agentFor("enemies");
  World soft = world('%');
  seen.decide(soft, liveOf(soft), o);
  World w = world('.');
  w.frame = 200;
  seen.decide(w, liveOf(w), o);
  CHECK(seen.what().find("trap for kill 5,1: bomb at (7,1)") == 0);
}

// A route over a held jump flags the landing waypoint (kJumpHold), a let-go one not.
TEST("agent: a held jump's landing waypoint says hold") {
  World w = mk({"#####",
                "#.i.#",
                "##w##",
                "#####",
                "#####",
                "##S##",
                "#...#",
                "#####"}, 1, 6, "item 2 1 item kind=2");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; };
  Agent a = agentFor("items");
  a.decide(w, liveOf(w), o);
  bool held = false, plain = false;
  for (const SimReq& q : o.seen)
    for (size_t i = 0; i + 1 < q.path.size(); i += 2) {
      if (q.path[i] & kJumpHold) held |= (q.path[i] & ~kJumpHold) == 2 * 16 + 8 && q.path[i + 1] == 1 * 16 + 8;
      else plain |= q.path[i] == 2 * 16 + 8 && q.path[i + 1] == 5 * 16 + 8;
    }
  CHECK(held);
  CHECK(plain);
}

// A short walk (closing in on an enemy) is never cut to end on a pad: P1
// would be thrown on with no plan for it (3-6, cut on (5,13): bounced).
TEST("agent: a cut walk never ends on a pad") {
  World w = mk({"###########",
                "#...S.....#",
                "###########"}, 1, 1,
               "enemy slot=5 type=80 x=" + std::to_string(9 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8) + " hp=1");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 1; r.enemies = r.hp = 1; };  // (no kill)
  Agent a = agentFor("enemies");
  a.decide(w, liveOf(w), o);
  CHECK(a.what().find("closing in") != std::string::npos);
  for (const SimReq& q : o.seen) {
    const auto& p = q.path2.empty() ? q.path : q.path2;
    if (p.size() >= 2) CHECK(!((p[p.size() - 2] & ~kJumpHold) == 4 * 16 + 8 && p.back() == 1 * 16 + 8));
  }
}

// The oracle's enemy count (agent_enemies): a $90/$98 counts until a blast
// makes it a mount (+$BE bit 15, bit 14 clear; 3-1 never saw a $98 hit).
TEST("agent: enemy count: $90/$98 until they are mounts") {
  std::vector<uint8_t> w(size_t(kWramSize), 0);
  auto put = [&](int slot, int ty, int be) {
    int b = 0x8400 + slot * 0x100;
    w[size_t(b + 1)] = 0x90, w[size_t(b + 0x7E)] = uint8_t(ty), w[size_t(b + 0x43)] = 1;
    w[size_t(b + 0xBE)] = uint8_t(be), w[size_t(b + 0xBF)] = uint8_t(be >> 8);
  };
  put(0, 0x98, 0);
  put(1, 0x98, 0x8000);  // (mounts)
  put(2, 0x90, 0);
  put(3, 0x90, 0x8000);
  put(4, 0x81, 0);
  put(5, 0x98, 0xC000);  // (bit 14 too: not a mount)
  int en, hp;
  agent_enemies(w.data(), &en, &hp);
  CHECK_EQ(en, 4);
}

// No plan, a rideable object not tried: board it and see; the ride is kept
// and the next decision routes over it (flagging where it lets P1 off).
TEST("agent: rides: probe an unknown one, then use it") {
  World w = mk({"###########",
                "#...^#....#",
                "###########"}, 1, 1,
               "enemy slot=5 type=DA x=" + std::to_string(4 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8) +
                   "\nitem 8 1 item kind=2");
  FakeOracle o;
  // (walks 3 cells, boards, carried to (8,1), stands there)
  auto ride = [](const SimReq&, SimOut& r) {
    r.dead = -1;
    for (size_t f = 0; 2 * f + 1 < r.trace.size(); f++) {
      int c = f < 40 ? 1 + int(f) / 13 : f < 100 ? 4 : 8;
      r.trace[2 * f] = int16_t(c * 16 + 8), r.trace[2 * f + 1] = 1 * 16 + 8;
    }
  };
  o.answer = ride;
  Agent a = agentFor("items");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.takeLog().find("ride: (4,1) carries P1 to (8,1)") != std::string::npos);
  CHECK(!o.seen.empty() && o.seen[0].frames > 300);
  CHECK_EQ(a.pending(), 101);  // (to the landing, not the probe's whole run)
  // Dead before the ride let P1 off: no ride; dead later (idle at the
  // landing, 3-5's $8B): the ride stands.
  SimOut x;
  x.trace.assign(2 * 400, 0);
  SimReq q;
  q.frames = 399;
  ride(q, x);
  CHECK_EQ(rideEnd(x, 4, 1), 100);
  x.dead = 110;
  CHECK_EQ(rideEnd(x, 4, 1), -1);
  x.dead = 300;
  CHECK_EQ(rideEnd(x, 4, 1), 100);
  CHECK_EQ(rideEnd(x, 6, 1), -1);  // (never on that cell)
  // Off the ride, the sub beside P1 is its dock (boarded back from there).
  World off = w;
  off.p.x = 8 * 16 + 8;
  off.enemies[0].x = 7 * 16 + 8;
  off.frame = 200;
  a.decide(off, liveOf(off), o);
  CHECK(a.worldText().find("ride 4 1 8 1 dock_c=7 dock_r=1") != std::string::npos);
  o.seen.clear();
  World w2 = w;
  w2.frame = 400;
  o.answer = [](const SimReq& q, SimOut& r) {
    r.dead = -1;
    r.px = q.path[q.path.size() - 2] & ~kWpFlags, r.py = q.path.back();
  };
  a.decide(w2, liveOf(w2), o);
  bool rode = false;
  for (const SimReq& q : o.seen)
    for (size_t i = 0; i + 1 < q.path.size(); i += 2) rode |= (q.path[i] & kRideEnd) && (q.path[i] & ~kWpFlags) == 8 * 16 + 8;
  CHECK(rode);
}

// Back by a ride's dock (the sub P1 came on sits there): the landing
// waypoint says where the ride lets P1 off.
TEST("agent: rides: a dock's ride is flagged too") {
  World w = mk({"###########",
                "#i..^#^...#",
                "###########"}, 8, 1,
               "enemy slot=5 type=DA x=" + std::to_string(6 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8) +
                   "\nride 4 1 8 1 dock_c=6 dock_r=1\nride 6 1 2 1 dock_c=4 dock_r=1\nitem 1 1 item kind=2");
  FakeOracle o;
  o.answer = [](const SimReq& q, SimOut& r) {
    r.dead = -1;
    r.px = q.path[q.path.size() - 2] & ~kWpFlags, r.py = q.path.back();
  };
  Agent a = agentFor("items");
  a.decide(w, liveOf(w), o);
  bool rode = false;
  for (const SimReq& q : o.seen)
    for (size_t i = 0; i + 1 < q.path.size(); i += 2) rode |= (q.path[i] & kRideEnd) && (q.path[i] & ~kWpFlags) == 2 * 16 + 8;
  CHECK(rode);
}

// A short move over a pad (closing in on an enemy) is not cut to 40 frames:
// a jump takes ~90, and a move ending in the air leaves P1 landing blind
// (3-1: by the $98 it then landed beside).
TEST("agent: a short move over a pad lasts the jump") {
  World w = mk({"###########",
                "#.S...#...#",
                "###########"}, 1, 1,
               "enemy slot=5 type=80 x=" + std::to_string(9 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8) + " hp=1");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 1; r.enemies = r.hp = 1; };  // (no kill)
  Agent a = agentFor("enemies");
  a.decide(w, liveOf(w), o);
  CHECK(a.what().find("closing in") != std::string::npos);
  CHECK(a.pending() > 90);
}

// A kill counts when the target's slot stops holding an enemy, even if the
// count stays (a $98 hit: a mount, and a new $99 rider elsewhere).
TEST("agent: a kill is the target's slot gone, whatever the count") {
  World w = mk({"#######",
                "#.....#",
                "#.#.#.#",
                "#.....#",
                "#######"}, 1, 3,
               "enemy slot=6 type=98 x=" + std::to_string(5 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8) +
                   " vx=0 vy=0 hp=1 spd=0.5");
  Live l = liveOf(w);
  l.enemyMask = 1 << 2;
  FakeOracle o;
  // (no run lowers the count; the slot's bit goes, a rider's comes)
  o.answer = [](const SimReq& q, SimOut& r) {
    r.dead = -1, r.mapSig = 2, r.enemies = r.hp = 1;
    r.enemyMask = q.post.empty() ? 1 << 2 : 1 << 3;
  };
  Agent a = agentFor("enemies");
  a.decide(w, l, o);
  CHECK_EQ(a.killSlot(), 2);
  CHECK(a.what().find("kill 5,1") == 0);
  CHECK(a.what().find("closing in") == std::string::npos);
}

// Every option dies (something passing by): the best ones again after
// standing still a while (3-2 at fire 1: an immune $8F patrolled the only
// way; P1 survived in place a whole stage).
TEST("agent: all options die: try them again after a wait") {
  World w = mk({"#######",
                "#...%.#",
                "#.###.#",
                "#.....#",
                "#######"}, 1, 1);
  FakeOracle o;
  o.answer = [](const SimReq& q, SimOut& r) {
    r.dead = q.pre.size() >= 64 ? -1 : 20;
    r.mapSig = 2;
  };
  Agent a = agentFor("blocks");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("(wait 64)") != std::string::npos);
}

// 2-3: a "clear the way" with no bomb cell this decision (here two bombs
// out leave no cover for either cell that reaches the block) marks it; the mark must
// not outlive the reason, or the route is shut for the rest of the stage.
TEST("agent: a soft block no cell reaches now: routed around for a while, not for good") {
  std::vector<std::string> rows = {"#######",
                                   "#...%g#",
                                   "#.#.###",
                                   "#...###",
                                   "#######"};
  const char* bombs = "gate 5 1 open\n"
                      "bomb c=1 r=3 fuse=150 range=2 owner=enemy remote=0 pierce=0 magnet=0 slot=-1\n"
                      "bomb c=3 r=3 fuse=150 range=2 owner=enemy remote=0 pierce=0 magnet=0 slot=-1";
  World w1 = mk(rows, 1, 1, bombs);
  w1.frame = 1000;
  w1.p.bombs = 1;
  FakeOracle o;
  o.answer = [&](const SimReq& q, SimOut& r) {
    r.dead = -1;
    r.mapSig = q.post.empty() ? 1 : 2;
  };
  Agent a = agentFor("exit");
  a.decide(w1, liveOf(w1), o);
  CHECK(a.what() == "survive");
  CHECK(a.takeLog().find("survive (way to gate 5,1)") != std::string::npos);
  World w2 = mk(rows, 1, 1, "gate 5 1 open");
  w2.frame = 1000 + kBadSoftTTL;
  w2.p.bombs = 1;
  a.decide(w2, liveOf(w2), o);
  CHECK(a.what().find("clear the way to gate 5,1: bomb at") == 0);
}

// 2-5: the cells next to the block on the way to the button failed 3 times
// (every option ran into an enemy) and were dropped for the rest of the
// stage: P1 waited out the clock. A drop wears off.
TEST("agent: a bomb cell dropped after 3 fails is tried again later") {
  World w = mk({"#########",
                "#..%...g#",
                "#.##.####",
                "#..%.####",
                "#########"}, 1, 1, "gate 7 1 open");
  w.frame = 1000;
  FakeOracle o;
  bool refuse = true;
  o.answer = [&](const SimReq& q, SimOut& r) {
    r.dead = -1;
    r.mapSig = 1;
    if (q.post.empty()) return;
    if (!(refuse && q.path.back() / 16 == 1)) r.mapSig = 2;
  };
  Agent a = agentFor("exit");
  for (int k = 0; k < 6; k++) {
    a.decide(w, liveOf(w), o);
    if (a.what().find("bomb at") != std::string::npos) break;
  }
  CHECK(a.what().find("bomb at (2,3)") != std::string::npos);
  refuse = false;
  w.frame += 2 * kBadSoftTTL;
  a.decide(w, liveOf(w), o);
  CHECK(a.what().find("bomb at (2,1)") != std::string::npos);
}

// 2-5: a bomb under a furnace lights the fuse lines; every cover on one
// died. The cover is off the fuse lines, though farther.
TEST("agent: a bomb under a furnace: no cover on a fuse line") {
  World w = mk({"########",
                "###F####",
                "#ff.%.g#",
                "#f######",
                "#.######",
                "########"}, 1, 2, "gate 6 2 open");
  w.p.bombs = 1;
  FakeOracle o;
  o.answer = [&](const SimReq& q, SimOut& r) {
    r.dead = -1;
    r.mapSig = q.post.empty() ? 1 : 2;
  };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.what().find("bomb at (3,2), cover (1,4)") != std::string::npos);
}

// 3-6: survive parked P1 on a spring pad, and on a pad B is the held jump:
// the trap's B presses did nothing for 20000 frames. No waiting on a pad,
// no trap presses from one.
TEST("agent: spring pads: survive doesn't wait on one, traps don't fire from one") {
  World w = mk({"###########",
                "#S........#",
                "###########"}, 1, 1, "bomb c=8 r=1 fuse=0 range=1 owner=p1 remote=1");
  w.p.remote = 1;
  FakeOracle o;
  o.answer = [&](const SimReq&, SimOut& r) { r.dead = -1; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("survive (") != std::string::npos);
  CHECK(lg.find("toward (1,1)") == std::string::npos);
  // An enemy in the trap's blast, P1 well clear: fired off the floor, not
  // off the pad.
  World t = mk({"###########",
                "#S........#",
                "###########"}, 1, 1,
               "bomb c=8 r=1 fuse=0 range=1 owner=p1 remote=1\nenemy type=80 x=" + std::to_string(9 * 16 + 8) +
                   " y=" + std::to_string(16 + 8));
  Agent b = agentFor("exit");
  CHECK_EQ(b.trapCheck(t), 0);
  t.p.x = 3 * 16 + 8;
  CHECK_EQ(b.trapCheck(t), 1);
}

// Riding, an idle mount's cell is a wall ($C9, C1:C75A); off the mount it
// is floor again. A flame on P1 costs the mount, not a heart (C0:A630): no
// plan but one through an idle mount's cell -> bomb P1's own cell and stay
// (3-3: a $90 parked at (6,4) shut the corridor to the last $99).
TEST("agent: riding, an idle mount in the way: get off by a bomb on P1") {
  const std::string mount = "enemy slot=5 type=90 x=" + std::to_string(3 * 16 + 8) + " y=24 be=8001";
  World w = mk({"########",
                "#..#..g#",
                "########"}, 1, 1, "gate 6 1 open\n" + mount);
  w.p.f60 = 0x8000;
  w.p.bombs = 1;
  FakeOracle o;
  o.answer = [&](const SimReq&, SimOut& r) { r.dead = -1, r.px = 24, r.py = 24; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("do get off the mount: bomb at (1,1)") != std::string::npos);
  CHECK(!o.seen.empty() && !o.seen[0].pre.empty() && (o.seen[0].pre[0] & kPadA));
  // Not riding (the mount's cell would read floor then), or nothing past
  // it: no.
  World v = w;
  v.p.f60 = 0;
  Agent b = agentFor("exit");
  b.decide(v, liveOf(v), o);
  CHECK(b.takeLog().find("get off the mount") == std::string::npos);
  World u = mk({"########",
                "#..#..##",
                "#g######"}, 1, 1, "gate 1 2 open\n" + mount);
  u.p.f60 = 0x8000, u.p.bombs = 1;
  Agent c = agentFor("exit");
  c.decide(u, liveOf(u), o);
  CHECK(c.takeLog().find("get off the mount") == std::string::npos);
}

// Riding, no spring pad launches P1 (3-1: the route over the pad at (4,6)
// to the east half never went): the pad is floor, and a plan that needs it
// means getting off the mount first.
TEST("agent: riding, pads don't throw: get off for a plan that needs one") {
  World w = mk({"#########",
                "#..S#..g#",
                "#########"}, 1, 1, "gate 7 1 open");
  w.p.f60 = 0x8000;
  w.p.bombs = 1;
  FakeOracle o;
  o.answer = [&](const SimReq&, SimOut& r) { r.dead = -1, r.px = 24, r.py = 24; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("do get off the mount: bomb at (1,1)") != std::string::npos);
  // Not riding: over the pad.
  World v = w;
  v.p.f60 = 0;
  Agent b = agentFor("exit");
  b.decide(v, liveOf(v), o);
  CHECK(b.takeLog().find("get off the mount") == std::string::npos);
}

// B sets off the oldest remote bomb, but one already going off still
// reads as the oldest: the press then sets off the next. P1 has to be clear
// of all its remote bombs (2-3: a second press for (8,6) blew up P1 on its
// new bomb at (7,8)).
TEST("agent: trap: no B while P1 is in any of its remote bombs' blasts") {
  std::string en = "enemy type=80 x=" + std::to_string(9 * 16 + 8) + " y=" + std::to_string(16 + 8);
  World t = mk({"###########",
                "#.........#",
                "###########"}, 2, 1,
               "bomb c=8 r=1 fuse=0 range=1 owner=p1 remote=1\nbomb c=2 r=1 fuse=0 range=1 owner=p1 remote=1\n" + en);
  Agent a = agentFor("exit");
  CHECK_EQ(a.trapCheck(t), 0);
  // Off the newer bomb's blast: fire.
  t.p.x = 4 * 16 + 8;
  Agent b = agentFor("exit");
  CHECK_EQ(b.trapCheck(t), 1);
}

// Riding a $98, its $99 rider is parked dormant and the gate stays shut
// (3-1: 10000 frames by the gate): get off; not riding: no.
TEST("agent: riding, gate shut, only the parked rider left: get off") {
  World w = mk({"########",
                "#.....g#",
                "########"}, 1, 1,
               "gate 6 1 closed\nenemy slot=9 type=99 x=56 y=0 f4=AC00\nenemy slot=8 type=98 x=24 y=24 be=C001");
  w.p.f60 = 0x8000;
  w.p.bombs = 1;
  FakeOracle o;
  o.answer = [&](const SimReq&, SimOut& r) { r.dead = -1, r.px = 24, r.py = 24; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.takeLog().find("do get off the mount: bomb at (1,1)") != std::string::npos);
  World v = w;
  v.p.f60 = 0;
  Agent b = agentFor("exit");
  b.decide(v, liveOf(v), o);
  CHECK(b.takeLog().find("get off the mount") == std::string::npos);
}

// A power-up no plan reaches doesn't hold up the exit (3-4: a fire-up in
// a soft block ringed by pads: 1400 "no plan" decisions to time-up).
TEST("agent: an unreachable power-up is left, the rest goes on") {
  World w = mk({"#######",
                "#....##",
                "###g#%#",
                "#######"}, 1, 1, "gate 3 2 open\nhidden 5 2 kind=2");
  FakeOracle o;
  o.answer = [&](const SimReq&, SimOut& r) { r.dead = -2; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("do gate 3,2") != std::string::npos);
  CHECK(lg.find("no plan") == std::string::npos);
}

// The flame kills by the cell under P1's point: P1 1 px off centre beside
// the trap's blast is clear (3-6: x=41 on (2,4) beside (3,4), boxed in by
// its own trap at (3,6) for 40000 frames).
TEST("agent: trap: P1 a pixel off centre beside the blast is clear") {
  World t = mk({"#######",
                "#.....#",
                "#.#.#.#",
                "#.....#",
                "#######"}, 2, 1, "bomb c=3 r=3 fuse=0 range=2 owner=p1 remote=1");
  t.p.x = 2 * 16 + 9;  // (1 px right of centre; (3,1) is in the blast)
  Agent a = agentFor("exit");
  CHECK_EQ(a.trapCheck(t), 2);
  t.p.x = 2 * 16 + 14;  // (about to step into it: not clear)
  Agent b = agentFor("exit");
  CHECK_EQ(b.trapCheck(t), 0);
}

// A dormant enemy sits in a soft block: the move is to bomb the block (it
// wakes, flashing and immune), judged by the map changing, not by a kill
// (4-1: every enemy starts dormant; a kill check never passed, and the
// agent closed in on (6,6) for 20000 frames).
TEST("agent: a dormant enemy in a soft block: bomb the block to wake it") {
  World w = mk({"#########",
                "#....%..#",
                "#.#.#.#.#",
                "#.......#",
                "###g#####",
                "#########"}, 1, 1, "gate 3 4 closed\nenemy slot=4 type=87 x=88 y=24 hp=1 f4=CC00");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("wake") != std::string::npos);
  CHECK(a.committed());
  int bc, br, yc, yr;
  bombAndCover(o.seen.back(), bc, br, yc, yr);
  bool hits = false;
  for (const Cell& x : blastCells(w, bc, br, w.p.fire, false)) hits |= x.c == 5 && x.r == 1;
  CHECK(hits);
}

// A dormant enemy's x/y don't follow the scroll: where it was first seen
// dormant is where it is (4-6: the $93 in the (3,7) block read as in the
// wall over the gate after the view scrolled, and no task woke it).
TEST("agent: a dormant enemy stays where first seen while the view scrolls") {
  World w = mk({"#########",
                "#....%..#",
                "#.#.#.#.#",
                "#.......#",
                "###g#####",
                "#########"}, 1, 1, "gate 3 4 closed\nenemy slot=4 type=87 x=88 y=24 hp=1 f4=CC00");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  a.takeLog();
  World w2 = w;
  w2.enemies[0].y = 72;  // (5,4): the wall, by screen + the new origin
  w2.frame = w.frame + 600;
  o.seen.clear();
  a.decide(w2, liveOf(w2), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("wake") != std::string::npos);
  CHECK(!o.seen.empty());
  int bc, br, yc, yr;
  bombAndCover(o.seen.back(), bc, br, yc, yr);
  bool hits = false;
  for (const Cell& x : blastCells(w, bc, br, w.p.fire, false)) hits |= x.c == 5 && x.r == 1;
  CHECK(hits);
}

// A walk over a platform lane: its far end's waypoint carries kLaneEnd (the
// path follower holds toward it), and the move lasts the ride.
TEST("agent: a lane ride's far end is flagged and the move lasts the ride") {
  World w = mk({"###########",
                "#..L###L.g#",
                "###########"}, 1, 1, "gate 9 1 open\nenemy slot=5 type=DC x=88 y=24 hp=0 f4=0C00");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -2; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  CHECK(!o.seen.empty());
  const SimReq& q = o.seen[0];
  bool flagged = false;
  for (size_t i = 0; i + 1 < q.path.size(); i += 2)
    if (q.path[i] & kLaneEnd) flagged = (q.path[i] & ~kWpFlags) / 16 == 7;
  CHECK(flagged);
  CHECK(q.frames >= laneFrames(3));
}

// Sealed in a pocket whose every way out undoes a done mode-1 panel: undo
// it (the plan redoes it later) rather than wait for good (4-6: 20000
// frames in (7..9,21..24) by its four done panels).
TEST("agent: every way out undoes a done panel: undo one") {
  World w = mk({"########",
                "#P..%.g#",
                "#.######",
                "#.######",
                "########"}, 2, 1, "gate 6 1 open\npanel 1 1 done mode=1");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("clear the way to gate 6,1") != std::string::npos);
  CHECK(a.committed());
}

// Every spot that lights the panel undoes a done one (fire 3): bomb a
// parity spot rather than wait for good (4-3 from 4-2: 13000 frames).
TEST("agent: every spot by a panel undoes a done one: bomb a parity spot") {
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
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("(parity)") != std::string::npos);
  CHECK(a.committed());
}

// The blocks before the open gate break only under a blast that goes on to
// the gate (pierce): bomb anyway, the enemies it brings back get planned
// (4-4 played on from 4-3 with a pierce bomb: 18000 frames by (7,6)).
TEST("agent: only a blast that reaches the gate opens the way to it: bomb") {
  World w = mk({"#######",
                "###g###",
                "###%###",
                "###%###",
                "#.....#",
                "#.###.#",
                "#######"}, 1, 4, "gate 3 1 open");
  w.p.fire = 4;
  w.p.pierce = 1;
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("clear the way to gate 3,1") != std::string::npos);
  CHECK(a.committed());
}

// The nearest spots to wake a dormant enemy burn an item: try further ones
// (4-6 played on, frame 42222: a heart at (11,25) between P1 and the $89
// in (12,25); "closing in" for 22000 frames).
TEST("agent: wake spots that burn an item are passed over for the next") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/4-6_wake_by_item.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("wake kill 12,25") != std::string::npos);
}

// A first plan step with no move that lives gives way to a later one P1
// can walk to (5-1, snapshot at 1336: P1 boxed in the top-left corner by a
// regrown (4,4), a $8A patrolling column 2: step 1 "kill 12,4" needs (4,4)
// broken and every cover is down column 2. P1 sat 600 frames in survive
// and died; the $8A itself was step 6).
TEST("agent: a first step that only dies gives way to a later one") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/5-1_corner.txt", w, &err));
  Goal gl;
  Goal::parse("powerups+exit", gl);
  Plan p = plan(w, gl);
  CHECK(p.ok && p.steps.size() > 3);
  CHECK(p.tasks[size_t(p.steps[0].task)].kind == TaskKind::KillEnemy && p.tasks[size_t(p.steps[0].task)].c == 12);
  FakeOracle o;
  o.answer = [](const SimReq& q, SimOut& r) {
    r.dead = 40;
    r.mapSig = 2;
    r.enemies = 3;
    if (q.path.size() >= 2 && q.path2.size() >= 2 && q.path[q.path.size() - 2] / 16 != 3) r.dead = -1;  // (any bomb not at (3,4) lives)
  };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what() != "survive");
}

// A trap ($4B 'T') with no heart is the life (PlayerOnTrap C1:40B9): P1
// bombs it like a soft block rather than walk over it (5-1: every move to
// the panel at (4,11) crossed the $4B at (2,11) and died there). While P1
// flashes it is free.
TEST("agent: a trap is bombed, not walked over") {
  World w = mk({"#######",
                "#..T.g#",
                "#.#####",
                "#.#####",
                "#######"}, 1, 1, "gate 5 1 open");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; r.enemies = 0; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("bomb at (2,1)") != std::string::npos);
  w.p.flash = 100;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -2; };  // (the walk ends on the gate)
  Agent b = agentFor("exit");
  b.decide(w, liveOf(w), o);
  CHECK(b.what().find("walk to (5,1)") != std::string::npos);
}

// A trap's B mid-move: the oracle plays B then the rest of the move first
// (world 3, 3-7: B on the (4,11) trap mid-move stirred the RNG; the $B0
// came on where the move's run had it turn, and P1 died). Dies: the trap
// waits, the move goes on; lives: P1 plays that run, B first.
TEST("agent: a trap mid-move goes off only if the rest of the move lives") {
  World w = mk({"#######",
                "#.....#",
                "#.....g#",
                "#######"}, 1, 1, "gate 5 2 open");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -2; };
  Agent a = agentFor("exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  std::vector<uint16_t> before = a.pads();
  int left = a.pending();
  o.answer = [](const SimReq&, SimOut& r) { r.dead = 9; };
  CHECK(!a.trapMidMove(o));
  CHECK(a.pads() == before && a.pending() == left);
  CHECK(!o.seen.empty() && o.seen.back().pre.size() == size_t(4 + left) && o.seen.back().pre[0] == kPadB);
  o.answer = [](const SimReq& q, SimOut& r) {
    r.dead = -1;
    for (size_t i = 0; i < q.pre.size(); i++) r.pads[i] = q.pre[i];
  };
  CHECK(a.trapMidMove(o));
  CHECK(a.committed() && a.pending() == 4 + left && a.pads()[0] == kPadB);
}

// Survive keeping P1 on one cell kStuckFrames moves it off (5-3: P1 stood 9000
// frames two cells from a $91 waiting to flame it, till the timer ran out).
TEST("agent: survive stuck on one cell moves off it") {
  World w = mk({"#######",
                "#.....#",
                "#######"}, 3, 1, "gate 5 1 closed");
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; };
  Agent a = agentFor("exit");
  std::string last;
  for (int f = 0; f <= 600; f += 60) {
    w.frame = f;
    a.decide(w, liveOf(w), o);
    last = a.takeLog();
  }
  CHECK(last.find("survive") != std::string::npos && last.find("toward (3,1)") == std::string::npos);
  Agent b = agentFor("exit");
  w.frame = 0;
  b.decide(w, liveOf(w), o);
  CHECK(b.takeLog().find("toward (3,1)") != std::string::npos);  // (at first it stays)
}

// A boss fight: a move's run must live kBossTail (180) frames past its end,
// not 60 (5-8: a trap's cover at (2,5) lived its 60; the body's sweep over
// rows 2-7 came 200 frames on, and no way out was left).
TEST("agent: in a boss fight a move must live longer past its end") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/4-8_robot_pinned.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("exit");
  Live l = liveOf(w);
  l.bossHp = 3;
  a.decide(w, l, o);
  CHECK(a.committed() && !o.seen.empty());
  if (!o.seen.empty()) CHECK(o.seen.back().frames >= a.pending() + 180);
  World n = w;  // (no boss: 60)
  n.bosses.clear();
  FakeOracle p;
  p.answer = o.answer;
  Agent b = agentFor("exit");
  b.decide(n, liveOf(n), p);
  CHECK(b.committed() && !p.seen.empty());
  if (!p.seen.empty()) CHECK(p.seen.back().frames < b.pending() + 180);
}

// 5-8 form 2 (CC:D139) takes no flame: only a thrown bomb in its +-24 box.
// With the glove the agent walks to a cell, places a bomb, lifts it (A
// held), faces the way and lets go, where the flight meets the body's path.
TEST("agent: final boss form 2: throw a bomb at it") {
  World w = mk({"###########",
                "#.........#",
                "#.........#",
                "#.........#",
                "#.........#",
                "#.........#",
                "###########"}, 2, 3, "");
  BossObj b;
  b.base = 0x1000, b.state = 0xCCD139, b.hp = 6;
  b.x = 7 * 16 + 8, b.y = 3 * 16 + 8;
  b.hasBox = true;
  b.box[0] = -24, b.box[1] = 24, b.box[2] = -24, b.box[3] = 24;
  b.sense = {{0, 0}};
  w.bosses.push_back(b);
  w.p.glove = 1;
  w.p.bombs = 1;
  FakeOracle o;
  bossAnswers(o, w, 6, 5);
  Agent a = agentFor("exit");
  Live l = liveOf(w);
  l.bossHp = 6;
  World seen = w;
  a.noteBosses(seen);
  CHECK(seen.bosses[0].sense.empty());  // (no flame hurts it)
  a.decide(seen, l, o);
  CHECK(a.committed());
  CHECK(a.what().find("(throw") != std::string::npos);
  CHECK(!o.seen.empty() && o.seen.back().post.size() > 10 && o.seen.back().post[0] == kPadA &&
        (o.seen.back().post[o.seen.back().post.size() - 2] & kPadA));
  // No glove: no throw.
  World ng = seen;
  ng.p.glove = 0;
  Agent n = agentFor("exit");
  n.decide(ng, l, o);
  CHECK(n.what().find("(throw") == std::string::npos);
}

// 5-5 played on from 5-4 (frame 60 of the cut): the plan's one step is the
// open-way push of (8,10) from (7,10), whose blast (fire 4) reaches the
// fire-up at (3,10). No bomb burns a wanted item, so the step had no move
// at all: 22000 frames in survive, then the timer. Pick the item up first.
TEST("agent: a step's blast would burn a wanted item: pick it up first") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/5-5_item_blast.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("walk to (3,10)") != std::string::npos);
}

// 5-2 played on from 5-1 (a world 5 run, frame 16860): the plan's first
// step pushes (16,6) from (16,4), a soft block. The cheapest way in is from
// (16,3), a panel: no bomb goes there, and no other cell P1 can reach now
// has (16,4) in its blast, so the block was routed around for good and the
// step had no move (10000 frames, the timer). Come in from (16,5) instead:
// break (17,5) first.
TEST("agent: a soft block no cell reaches from the route's side: come at it from another") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/5-2_way_push.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(!o.seen.empty());
  int bc, br, yc, yr;
  bombAndCover(o.seen.back(), bc, br, yc, yr);
  bool hits = false;
  for (const Cell& x : blastCells(w, bc, br, w.p.fire, false)) hits |= x.c == 17 && x.r == 5;
  CHECK(hits);
}

// 4-5 played on from 4-4 (a world 4 run, frame 596 of the cut): the last
// panel (29,10) is lit cleanly only from the soft (28,10), and only a bomb on
// the panel itself would break that. Every reachable spot in column 29 also
// undoes (29,8)/(29,6) (fire 5). The way to the stand had no move: 15000
// frames in survive, the timer. Bomb a parity spot instead.
TEST("agent: no way to a panel's stand: bomb a parity spot") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/4-5_panel_parity.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("(parity)") != std::string::npos);
}

// 5-5 played on from 5-4 with fire 6 (a world 5 run, frame 8456 of the
// cut): the push of (4,4) into the hole (3,4) is from (5,4), and its blast
// also reaches the done panel (10,4). No bomb undoes a done panel, so the
// step had no move: 14000 frames in survive, the timer. Undo it; the plan
// lights it again.
TEST("agent: every spot for a push undoes a done panel: push anyway") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/5-5_push_undo.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("push 4,4") != std::string::npos);
}

// 5-6 played on from 5-5 (a world 5 run): the gate (9,3) is open over the
// hole (9,4); the plan pushes (11,4) in from (13,4), a soft block bombed
// from (12,4), and that blast reaches the fire-up at (12,5). The way had no
// move: 17000 frames in survive, the timer. Pick the item up first.
TEST("agent: the way's bomb would burn a wanted item: pick it up first") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/5-6_way_item.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("walk to (12,5)") != std::string::npos);
}

// 4-4 played on from 4-3 (a full-game run, frame 198109): P1 holds the
// remote, and the pierce-up at (7,5) (which would cost it) is the one way
// up to the gate (7,3). Kept off as a wall, the gate was out of reach: 20000
// frames of no plan, the timer. Kept off as a soft block, the way bombs it.
TEST("agent: an unwanted item in the only way is bombed, not a wall") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/4-4_unwanted_item.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("gate 7,3") != std::string::npos);
  int bc, br, yc, yr;
  if (o.seen.empty() || o.seen.back().path2.empty()) return;
  bombAndCover(o.seen.back(), bc, br, yc, yr);
  bool hits = false;
  for (const Cell& x : blastCells(w, bc, br, w.p.fire, false)) hits |= x.c == 7 && x.r == 5;
  CHECK(hits);
}

// 4-2 played on from 4-1 (a full-game run, frame 166259, fire 10): (26,5)
// is lit from column 26, which also undoes (26,10)/(26,12); the other
// todo panels (24,5)/(12,5) are behind soft blocks. Bomb a parity spot.
TEST("agent: a panel whose every spot undoes others, more panels out of reach: parity") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/4-2_panel_stall.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("(parity)") != std::string::npos);
}

// 2-2 (seed 1): a way's own blast still burning when its commit ended; a
// 60-frame survive let the gap it opened at (3,11) grow back (regrow in=81)
// before P1 got there, and that looped till the timer. With flames out,
// survive plays a short slice and looks again.
TEST("agent: flames in the way: survive briefly, then look again") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/2-2_flame_wait.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("survive (way to panel 3,10)") != std::string::npos);
  CHECK(a.what() == "survive" && a.pending() <= 12);
}

// 2-4 (seed 0, after the flame fix): a trap's cover at (3,9), in the
// column-3 pocket whose ways out, (3,7) and (4,8), are gaps that grow back.
// They did, and with fire 4 every bomb there reached the whole pocket: P1
// sat sealed in till the timer. Covers regrowth can seal in go last: here
// (5,2) is the nearest cover for the bomb on (7,3), but once the gap (5,3)
// grows back P1 can't bomb out of (5,1)-(5,2).
TEST("agent: no cover in a pocket regrowth can seal") {
  World w = mk({"#########",
                "#####.###",
                "#####.###",
                "#......%#",
                "#########"}, 1, 3, "regrow in=2000 cells=5,3");
  w.p.fire = 1;  // (the bomb goes on (6,3); covers (5,2) and (4,3) are 2 steps)
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapChanged = true; };
  Agent a = agentFor("blocks");
  a.decide(w, liveOf(w), o);
  a.takeLog();
  CHECK(!o.seen.empty() && o.seen[0].path2.size() >= 2);
  int bc, br, yc, yr;
  bombAndCover(o.seen[0], bc, br, yc, yr);
  CHECK(!(yc == 5 && yr <= 2));
}

// 3-5 (seed 0): the sub for the way back sat docked at the far end, where
// only a ride from there brings it over. P1 had no way there, but "ride it
// over first" counted it: 20000 frames on a plan through it, the timer ran
// out. A ride counts only if P1 can get to it (then an unknown ride is
// probed: the sub P1 came on).
TEST("agent: rides: a sub docked out of reach is no way back") {
  World w = mk({"###########",
                "#i..^#^...#",
                "###########"}, 8, 1,
               "enemy slot=5 type=DA x=" + std::to_string(4 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8) +
                   "\nride 4 1 8 1 dock_c=6 dock_r=1\nride 6 1 2 1 dock_c=4 dock_r=1\nitem 1 1 item kind=2");
  CHECK(!rideDock(w, 6, 1));
  World l = w;  // (P1 on the sub's side: ride it over, then back from the dock)
  l.p.x = 2 * 16 + 8;
  CHECK(rideDock(l, 6, 1));
}

// 3-5 (seed 0), live: below with the way-back sub docked up top, the sub P1
// came down on is probed for where it goes.
TEST("agent: rides: the sub P1 came on is probed for the way back") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/3-5_sub_back.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  a.takeLog();
  bool probed = false;
  for (const SimReq& q : o.seen)
    if (q.path.size() >= 2 && (q.path[q.path.size() - 2] & ~kWpFlags) == 18 * 16 + 8 && q.path.back() == 10 * 16 + 8) probed = true;
  CHECK(probed);
}

// 4-4 (seed 2): a move ended with P1 on a platform out over its lane
// ((4,10), between the ends (2,10) and (5,10)). The step off to (5,10) was a
// plain step, and riding nothing was pressed: P1 shuttled 18000 frames. It
// is a lane step: held toward the end, P1 steps off when the platform waits.
TEST("agent: on a platform mid-lane, the step off is a lane step") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/4-4_on_lane.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  a.takeLog();
  CHECK(!o.seen.empty() && o.seen[0].path.size() >= 4);
  const SimReq& q = o.seen[0];
  CHECK((q.path[2] & ~kWpFlags) == 5 * 16 + 8 && q.path[3] == 10 * 16 + 8);
  CHECK((q.path[2] & kLaneEnd) != 0);
}

// 5-7 (seed 2): the glove came out of its block after the bomb meant to
// break it was laid; the trap then set that bomb off on an enemy, over the
// bare glove (5-8's form 2 is hurt only by thrown bombs). Traps hold off
// wanted items, and in 5-7 the glove is one.
TEST("agent: trap: no B over the glove where it is wanted") {
  World w = mk({"#########",
                "#.......#",
                "#.#.#.#.#",
                "#.......#",
                "#########"}, 1, 1,
               "enemy slot=5 type=80 x=" + std::to_string(6 * 16 + 8) + " y=" + std::to_string(3 * 16 + 8) + " hp=1 spd=0.5");
  w.p.remote = true;
  w.stage = 0x56;
  w.bombs.push_back({});
  w.bombs.back().c = 5, w.bombs.back().r = 3, w.bombs.back().remote = true, w.bombs.back().range = 2;
  w.items.push_back({});
  w.items.back().c = 3, w.items.back().r = 3, w.items.back().kind = kItemGlove;
  Agent a = agentFor("enemies");
  CHECK_EQ(a.trapCheck(w), 0);
  World h = w;  // (one held already: no reason to wait)
  h.p.glove = 1;
  Agent b = agentFor("enemies");
  CHECK_EQ(b.trapCheck(h), 1);
  // The glove in a newer bomb's blast, not the oldest's: B may set off the
  // newer one (one going off still reads as the oldest).
  World n = w;
  n.items.back().c = 2, n.items.back().r = 1;
  n.bombs.push_back({});
  n.bombs.back().c = 3, n.bombs.back().r = 1, n.bombs.back().remote = true, n.bombs.back().range = 1;
  Agent c = agentFor("enemies");
  CHECK_EQ(c.trapCheck(n), 0);
}

// 5-7 (seed 2): the glove came out of its block 5 frames into a move to
// bomb that block open; the move's own B set the bomb off over it. A press
// whose blast would now burn a wanted item P1 can reach stops the move.
TEST("agent: a move's B over an item that came out since: the move stops") {
  World w = mk({"#########",
                "#.......#",
                "#.#.#.#.#",
                "#.......#",
                "#########"}, 1, 1);
  w.stage = 0x56;
  w.p.remote = true;
  w.bombs.push_back({});
  w.bombs.back().c = 5, w.bombs.back().r = 3, w.bombs.back().remote = true, w.bombs.back().range = 2;
  Agent a = agentFor("exit");
  CHECK(!a.moveBurns(w, kPadB));
  World g = w;
  g.items.push_back({});
  g.items.back().c = 3, g.items.back().r = 3, g.items.back().kind = kItemGlove;
  CHECK(a.moveBurns(g, kPadB));
  CHECK(!a.moveBurns(g, kPadRight));
  World o2 = g;  // (an older bomb, going off: B may set off the next one)
  o2.bombs.insert(o2.bombs.begin(), Bomb{});
  o2.bombs.front().c = 7, o2.bombs.front().r = 1, o2.bombs.front().remote = true, o2.bombs.front().range = 1;
  Agent a2 = agentFor("exit");
  a2.moveBurns(o2, 0);
  CHECK(a2.moveBurns(o2, kPadB));
  World u = g;  // (one P1 can't reach: the move goes on)
  u.map[size_t(1 * u.cols + 2)] = '#', u.map[size_t(2 * u.cols + 1)] = '#';
  CHECK(!a.moveBurns(u, kPadB));
  // A (no remote): the bomb on P1's cell, an item in its blast.
  World p = mk({"#######",
                "#.....#",
                "#######"}, 1, 1);
  p.p.bombs = 1;
  p.items.push_back({});
  p.items.back().c = 3, p.items.back().r = 1, p.items.back().kind = kItemFire;
  CHECK(a.moveBurns(p, kPadA));
  p.items.back().c = 5;  // (out of its reach, fire 2)
  CHECK(!a.moveBurns(p, kPadA));
}

// 5-7 (seed 2): a remote bomb of P1's out beside the glove. Any B may set
// it off, so every move pressing one stopped (moveBurns), and the bombs ran
// out. The item by the bomb is fetched first.
TEST("agent: a wanted item by P1's remote bomb is fetched first") {
  World w = mk({"#########",
                "#.......#",
                "#.#.#.#.#",
                "#.......#",
                "#########"}, 1, 1,
               "enemy slot=5 type=80 x=" + std::to_string(7 * 16 + 8) + " y=" + std::to_string(1 * 16 + 8) + " hp=1 spd=0.5");
  w.stage = 0x56;
  w.p.remote = true;
  w.p.bombs = 2;
  w.bombs.push_back({});
  w.bombs.back().c = 5, w.bombs.back().r = 3, w.bombs.back().remote = true, w.bombs.back().range = 2;
  w.items.push_back({});
  w.items.back().c = 3, w.items.back().r = 3, w.items.back().kind = kItemGlove;
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapChanged = true; };
  Agent a = agentFor("enemies");
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("do item 3,3 (by a remote bomb)") != std::string::npos);
}

// 5-7, seeds 3 and 4: at its patrol's left end the boss covers column 2,
// rows 2..7. Survive tried the 16 nearest cells, all in that strip, all
// dying; the safe row below was never tried. With every near cell dying,
// farther ones are tried too.
TEST("agent: survive: every near cell dies: farther cells are tried") {
  World w = mk({"#########",
                "#.......#",
                "#.#.#.#.#",
                "#.......#",
                "#.#.#.#.#",
                "#.......#",
                "#.#.#.#.#",
                "#.......#",
                "#.#.#.#.#",
                "#.......#",
                "#########"}, 1, 1, "gate 7 9 closed");
  FakeOracle o;
  // only (1,9), 8 steps away, lives
  o.answer = [](const SimReq&, SimOut& r) { r.dead = r.px / 16 == 1 && r.py / 16 == 9 ? -1 : 100; };
  Agent a = agentFor("exit");
  w.frame = 0;
  a.decide(w, liveOf(w), o);
  std::string lg = a.takeLog();
  CHECK(lg.find("toward (1,9), lives") != std::string::npos);
  CHECK(o.seen.size() > 16);
}

// 3-1 (the whole game, seed 0): the stand for the panel (15,8) is the soft
// block (13,8); its one other side, (14,8), is in a pocket the spring (12,8)
// lands in, where no bomb has cover. The way into the pocket by the soft
// (14,7) above (from (14,6), or (14,9) in the pocket, cover (15,8)) wasn't
// tried: survive till the timer, 3 times.
TEST("agent: way to a stand: a pocket a spring lands in has no cover: in another way") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/3-1_soft_stand.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("clear the way to panel 15,8") != std::string::npos);
  CHECK(!o.seen.empty());
  if (!o.seen.empty()) {
    int bc, br, yc, yr;
    bombAndCover(o.seen[0], bc, br, yc, yr);
    CHECK(bc == 14 && (br == 6 || br == 9));   // (both break (14,7))
  }
}

// 3-1 (the whole game), after the pocket fix: P1 in the pocket at (15,8),
// (14,7) open; the one bomb spot for the stand (13,8) is (14,8), whose
// blast lights the gate (14,2) up column 14 (fire 10). A gate hit was a
// last resort for the gate's task only: survive till the timer.
TEST("agent: way to a stand: the one bomb spot lights the gate: bomb anyway") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/3-1_soft_stand2.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("clear the way to panel 15,8: bomb at (14,8)") != std::string::npos);
}

// A $93 and P1 walking head-on swap cells on one frame (4-2, frame 119361):
// the game sees no shared cell and nobody dies. The sims must count it.
static void putEnemy(std::vector<uint8_t>& w, int slot, int type, int x, int y) {
  int b = 0x8400 + 0x100 * slot;
  w[b + 1] = 0x90; w[b + 5] = 0; w[b + 0x7E] = uint8_t(type);
  w[b + 0x10] = uint8_t(x); w[b + 0x11] = uint8_t(x >> 8);
  w[b + 0x18] = uint8_t(y); w[b + 0x19] = uint8_t(y >> 8);
}
static void putP1(std::vector<uint8_t>& w, int x, int y) {
  w[0x8010] = uint8_t(x); w[0x8011] = uint8_t(x >> 8);
  w[0x8018] = uint8_t(y); w[0x8019] = uint8_t(y >> 8);
}
TEST("agent_api: P1 and an enemy swapping cells is a pass-through") {
  std::vector<uint8_t> a(0x20000), b(0x20000);
  int16_t pa, pb, ea[AG_ESLOTS], eb[AG_ESLOTS];
  // P1 at (22,5) going down, the $93 in (22,6) going up; one frame on
  // both are past the half-way line: P1 in (22,6), the $93 in (22,5)
  putP1(a, 22 * 16 + 8, 5 * 16 + 15); putEnemy(a, 2, 0x93, 22 * 16 + 8, 6 * 16 + 1);
  putP1(b, 22 * 16 + 8, 6 * 16 + 0);  putEnemy(b, 2, 0x93, 22 * 16 + 8, 5 * 16 + 15);
  pa = agent_p1_cell(a.data()); pb = agent_p1_cell(b.data());
  CHECK_EQ(pa, int16_t(22 | 5 << 8));
  agent_contact_cells(a.data(), ea); agent_contact_cells(b.data(), eb);
  CHECK_EQ(agent_passed_through(pa, pb, ea, eb), 3);
  // a type that never hurts on contact ($24, C5:D850) may be walked through
  putEnemy(a, 2, 0x24, 22 * 16 + 8, 6 * 16 + 1); putEnemy(b, 2, 0x24, 22 * 16 + 8, 5 * 16 + 15);
  agent_contact_cells(a.data(), ea); agent_contact_cells(b.data(), eb);
  CHECK_EQ(agent_passed_through(pa, pb, ea, eb), 0);
  // following an enemy into the cell it leaves is no pass-through
  putEnemy(a, 2, 0x93, 22 * 16 + 8, 6 * 16 + 15); putEnemy(b, 2, 0x93, 22 * 16 + 8, 7 * 16 + 0);
  agent_contact_cells(a.data(), ea); agent_contact_cells(b.data(), eb);
  CHECK_EQ(agent_passed_through(pa, pb, ea, eb), 0);
  // standing still: nothing to swap
  CHECK_EQ(agent_passed_through(pa, pa, ea, ea), 0);
}

// A chaser near P1: a move must live 180 frames past its end, not 60 (4-2,
// seed 0: trap covers at dead ends lived 60 frames, then the $93 came up
// the one way out; every move died). Same world, same move: a $93 (may
// chase on sight) or a $83 (never chases), walled in at (9,3).
static int firstFrames(int type) {
  char e[200];
  std::snprintf(e, sizeof e, "enemy slot=0 type=%02X x=152 y=56 vx=0 vy=0 step=- target=0,0 wait=0 hp=1 f4=0C00 "
                "c0=0000 d0=0000 be=0000 bc=0000 ba=0000 off=0 spd=0.5", type);
  World w = mk({"###########",
                "#...%.....#",
                "#.#.#.#.###",
                "#########.#",
                "###########"}, 1, 1, e);
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 1; };
  Agent a = agentFor("blocks");
  a.decide(w, liveOf(w), o);
  o.seen.clear();
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  a.decide(w, liveOf(w), o);
  return o.seen.empty() ? -1 : o.seen[0].frames;
}
TEST("agent: a chaser near: moves must live 180 frames past their end") {
  int walker = firstFrames(0x83), chaser = firstFrames(0x93);
  CHECK(walker > 0);
  CHECK_EQ(chaser - walker, 120);
}

// A walk to an item ends once P1 is on it, not at the commit's budget, when
// that would stand more than 120 frames (4-2: "item 10,5" over the platform
// played 988 frames, P1 was on the item at 340 and stood there 650). Short
// ones stay as they were.
static int walkLeft(int itemC) {
  World w = mk({"###############",
                "#.............#",
                "###############"}, 1, 1, "item " + std::to_string(itemC) + " 1 item kind=0");
  FakeOracle o;
  // (2 px a frame to the item, then stands)
  o.answer = [itemC](const SimReq&, SimOut& r) {
    r.dead = -1, r.mapSig = 2;
    for (size_t f = 0; 2 * f + 1 < r.trace.size(); f++) {
      int x = std::min(1 * 16 + 8 + 2 * int(f), itemC * 16 + 8);
      r.trace[2 * f] = int16_t(x), r.trace[2 * f + 1] = 1 * 16 + 8;
    }
  };
  Agent a = agentFor("items");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("item " + std::to_string(itemC) + ",1") == 0);
  return a.pending();
}
TEST("agent: a walk to an item ends when P1 is there") {
  // 12 cells: budget 22 * 11 + 20 = 262, within 4 px of it at frame 86
  CHECK_EQ(walkLeft(12), 86 + 20);
  // 5 cells: budget 22 * 4 + 20 = 108, there at 30: 78 frames left, kept
  CHECK_EQ(walkLeft(5), 108);
}

// A chaser near: a move that hits an enemy lives if its run lives kTail (60)
// past its end, not the long tail (5-1, fire 1: in a pocket with a 2 HP
// $8A, every attack's run died standing still in the 180-frame tail; P1
// only dodged until it was cornered).
TEST("agent: a chaser near: a hit needs only the short tail") {
  World w = mk({"###########",
                "#.........#",
                "#.#.#.#.#.#",
                "#.........#",
                "###########"}, 1, 1,
               "enemy slot=0 type=93 x=120 y=24 vx=0 vy=0 step=- target=0,0 wait=0 hp=2 f4=0C00 "
               "c0=0000 d0=0000 be=0000 bc=0000 ba=0000 off=0 spd=0.5");
  FakeOracle o;
  // (every run: the enemy hit, P1 where the move ends, dead 100 frames
  // after the move's end: inside the 180-frame tail, past the 60)
  o.answer = [](const SimReq& q, SimOut& r) {
    r.hp = 0, r.enemies = 1, r.mapSig = 1;
    r.dead = std::max(1, q.frames - 180 + 100);
    for (size_t f = 0; 2 * f + 1 < r.trace.size(); f++) r.trace[2 * f] = int16_t(r.px), r.trace[2 * f + 1] = int16_t(r.py);
  };
  Agent a = agentFor("enemies");
  a.decide(w, liveOf(w), o);
  CHECK(!o.seen.empty() && o.seen[0].frames > 180);
  CHECK(a.committed());
  CHECK(a.what().find("survive") == std::string::npos && a.what().find("kill") != std::string::npos);
}

// Cracked floor ('C') the way to the bomb crosses is a hole once P1 is off
// it: no cover back over it (5-3: the way to (8,5) crossed (9,7), every
// cover was back over it; each died by P1's own bomb, 7000 frames).
static bool crosses(const std::vector<int16_t>& wp, int c, int r) {
  for (size_t i = 2; i + 1 < wp.size(); i += 2) {
    int x0 = (wp[i - 2] & ~kWpFlags) / 16, y0 = wp[i - 1] / 16, x1 = (wp[i] & ~kWpFlags) / 16, y1 = wp[i + 1] / 16;
    if (std::min(x0, x1) <= c && c <= std::max(x0, x1) && std::min(y0, y1) <= r && r <= std::max(y0, y1)) return true;
  }
  return false;
}
TEST("agent: no cover back over cracked floor the way crossed") {
  World w = mk({"#######",
                "#..C..#",
                "#%.####",
                "#######"}, 5, 1);
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = 5; };  // (every option dies: all are tried)
  Agent a = agentFor("blocks");
  a.decide(w, liveOf(w), o);
  int bombAt1 = 0, over = 0;
  for (const SimReq& q : o.seen) {
    if (q.post.empty() || q.path.size() < 2) continue;
    if ((q.path[q.path.size() - 2] & ~kWpFlags) / 16 != 1 || q.path.back() / 16 != 1) continue;
    bombAt1++;
    over += crosses(q.path2, 3, 1);
  }
  CHECK(bombAt1 > 0);  // (the cover in (2,2) is still tried)
  CHECK_EQ(over, 0);
}

// Closing in on a $91: not to a cell it sees (5-3: P1 two cells below one
// for 9000 frames; it stood at hp $FF the whole time, then got P1).
TEST("agent: closing in on a $91: out of its sight") {
  World w = mk({"#########",
                "#.......#",
                "#.#####.#",
                "#.......#",
                "#########"}, 1, 1,
               "enemy slot=0 type=91 x=120 y=24 vx=0 vy=0 step=- target=7,1 wait=0 hp=255 f4=0C00 "
               "c0=0000 d0=0000 be=0000 bc=0000 ba=0000 off=0 spd=0.5");
  FakeOracle o;
  o.answer = [](const SimReq& q, SimOut& r) { r.dead = q.post.empty() ? -1 : 5; };  // (no bomb gets it)
  Agent a = agentFor("enemies");
  a.decide(w, liveOf(w), o);
  std::string what = a.what();
  size_t at = what.find("(closing in): walk to (");
  CHECK(at != std::string::npos);
  int c = -1, r = -1;
  if (at != std::string::npos) std::sscanf(what.c_str() + at, "(closing in): walk to (%d,%d)", &c, &r);
  CHECK(c >= 0 && !EnemyModel::x91Sees(w, w.enemies[0], c, r));
}

// Survive: among cells that live, last the ones a $91 sees (5-3: each
// survive went back into its line; it stayed stopped at hp $FF).
TEST("agent: survive: out of a $91's sight first") {
  World w = mk({"#########",
                "#.......#",
                "#.#####.#",
                "#.......#",
                "#########"}, 4, 1,
               "enemy slot=0 type=91 x=120 y=24 vx=0 vy=0 step=- target=7,1 wait=0 hp=255 f4=0C00 "
               "c0=0000 d0=0000 be=0000 bc=0000 ba=0000 off=0 spd=0.5");
  FakeOracle o;
  // (only survive's runs live: 22 a cell + 140, + 120 with the long tail)
  o.answer = [](const SimReq& q, SimOut& r) {
    auto surv = [&](int x) { return x >= 0 && x % 22 == 0; };
    r.dead = q.post.empty() && (surv(q.frames - 140) || surv(q.frames - 260)) ? -1 : 5;
  };
  Agent a = agentFor("enemies");
  a.decide(w, liveOf(w), o);
  std::string log = a.takeLog();
  size_t at = log.find("): toward (");
  CHECK(at != std::string::npos);
  int c = -1, r = -1;
  if (at != std::string::npos) std::sscanf(log.c_str() + at, "): toward (%d,%d)", &c, &r);
  CHECK(c >= 0 && !EnemyModel::x91Sees(w, w.enemies[0], c, r));
}

// With softpass P1 walks through soft blocks (goap.cpp's walkable): the
// agent's routes too (5-4: the way up column 28 was the soft (28,9); the
// plan went through it, the agent's routes never did: 20000 frames closing
// in, time up).
TEST("agent: softpass: walks through a soft block to an item") {
  World w = mk({"#######",
                "#..%..#",
                "#######"}, 1, 1, "item 5 1 item kind=0");
  w.p.softpass = 1;
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; };
  Agent a = agentFor("powerups");
  a.decide(w, liveOf(w), o);
  CHECK(a.committed());
  CHECK(a.what().find("item 5,1: walk to (5,1)") == 0);
}

// A hit that lives only the short tail is a last resort: an option that
// lives the long tail goes first, even after it (whole game, 4-6: a trap
// that hit one lived 90 past its end, then every way died: a heart).
TEST("agent: a chaser near: a hit with the short tail only if nothing lives") {
  World w = mk({"###########",
                "#.........#",
                "#.#.#.#.#.#",
                "#.........#",
                "###########"}, 1, 1,
               "enemy slot=0 type=93 x=120 y=24 vx=0 vy=0 step=- target=0,0 wait=0 hp=2 f4=0C00 "
               "c0=0000 d0=0000 be=0000 bc=0000 ba=0000 off=0 spd=0.5");
  FakeOracle o;
  // (the first bomb run: a hit, dead 100 past the end; the rest: a hit,
  // lives)
  int bombRuns = 0;
  o.answer = [&bombRuns](const SimReq& q, SimOut& r) {
    r.hp = 0, r.enemies = 1, r.mapSig = 1;
    bool first = !q.post.empty() && bombRuns++ == 0;
    r.dead = first ? std::max(1, q.frames - 180 + 100) : -1;
    for (size_t f = 0; 2 * f + 1 < r.trace.size(); f++) r.trace[2 * f] = int16_t(r.px), r.trace[2 * f + 1] = int16_t(r.py);
  };
  Agent a = agentFor("enemies");
  a.decide(w, liveOf(w), o);
  std::string log = a.takeLog();
  CHECK(a.committed());
  CHECK(log.find("option 1,") == std::string::npos);
  CHECK(log.find("taken") == std::string::npos);
}

// Nothing plans unless blasts may reach the gate: the agent plans with gate
// hits and moves, not "no plan" (4-3 played on with pierce: 15000 frames of
// survive, the timer).
TEST("agent: no plan without gate hits: plans with them") {
  World w;
  std::string err;
  CHECK(loadWorld(sbm2test::worldsDir + "/4-3_pierce_gate.txt", w, &err));
  FakeOracle o;
  o.answer = [](const SimReq&, SimOut& r) { r.dead = -1; r.mapSig = 2; r.hp = 0; r.enemies = 0; };
  Agent a = agentFor("powerups+exit");
  a.decide(w, liveOf(w), o);
  CHECK(a.takeLog().find("no plan") == std::string::npos);
}
