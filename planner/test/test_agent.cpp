// The agent (planner/agent.cpp) with a fake oracle, and the snapshot text
// (planner/capture.cpp). No emulator: the oracle's answers are made up per
// test, so these pin the decision rules, not the game.
#include <fstream>
#include <functional>
#include <sstream>

#include "agent.hpp"
#include "blast.hpp"
#include "capture.hpp"
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
      if (last.size() >= 2) o.px = last[last.size() - 2], o.py = last.back();
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
