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
