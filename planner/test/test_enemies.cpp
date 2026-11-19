// The enemy model (planner/enemies.cpp) on small made-up maps: the rules of
// docs/enemies.md 4.1/4.2, and the odds the agent ranks bombs and covers by.
#include "enemies.hpp"
#include "test.hpp"

using namespace sbm2;

namespace {

World mkw(const std::vector<std::string>& rows, const std::string& enemies, int pc = 1, int pr = 1) {
  std::string s = "worldview 1\nsize " + std::to_string(rows[0].size()) + " " + std::to_string(rows.size()) +
                  "\nplayer x=" + std::to_string(pc * 16 + 8) + " y=" + std::to_string(pr * 16 + 8) + " fire=2\n" +
                  enemies + "\nmap\n";
  for (auto& r : rows) s += r + "\n";
  World w;
  std::string err;
  if (!parseWorld(s, w, &err)) std::printf("  mkw: %s\n", err.c_str());
  return w;
}

// A generic enemy at (c,r) walking dir at 0.5 px/f toward the next cell.
std::string walker(int c, int r, char dir) {
  int tc = c + (dir == 'r') - (dir == 'l'), tr = r + (dir == 'd') - (dir == 'u');
  double vx = dir == 'r' ? 0.5 : dir == 'l' ? -0.5 : 0, vy = dir == 'd' ? 0.5 : dir == 'u' ? -0.5 : 0;
  char b[200];
  std::snprintf(b, sizeof b, "enemy slot=4 type=94 x=%d y=%d vx=%g vy=%g step=%c target=%d,%d wait=0 hp=1 f4=0C00 c0=8000",
                c * 16 + 8, r * 16 + 7, vx, vy, dir, tc, tr);
  return b;
}

}  // namespace

TEST("enemies: generic walks straight to the wall, then stops 32 frames") {
  World w = mkw({"#########",
                 "#.......#",
                 "#########"}, walker(1, 1, 'r'));
  EnemyModel m(w, 400);
  auto runs = m.roll(nullptr, 1, 1, 8);
  CHECK_EQ(runs[0].rolls.size(), size_t(8));
  // Every rollout: at (7,1) by about 6 cells * 32 frames, and still there
  // 32 frames on (it can only turn back after the stop).
  for (const auto& roll : runs[0].rolls) {
    bool there = false;
    for (const auto& s : roll)
      if (s.c == 7 && s.r == 1 && s.t0 <= 6 * 32 && s.t1 >= 6 * 32 + 20) there = true;
    CHECK(there);
  }
}

TEST("enemies: a bomb at the mouth of a dead end kills what is inside") {
  // Pocket (1..3,1) opens at (4,1); the enemy walks left into it.
  World w = mkw({"#########",
                 "#.......#",
                 "####.####",
                 "####.####",
                 "#########"}, walker(3, 1, 'l'), 4, 3);
  EnemyModel m(w, 400);
  ModelBomb b{4, 1, 10, 160, 3, false};
  auto runs = m.roll(&b, 4, 3, 64);
  CHECK(EnemyModel::killOdds(runs) > 0.95);
}

TEST("enemies: a bomb far from the enemy's corridor kills nothing") {
  World w = mkw({"###########",
                 "#.........#",
                 "#.#######.#",
                 "#.........#",
                 "###########"}, walker(1, 1, 'r'), 1, 3);
  EnemyModel m(w, 300);
  ModelBomb b{5, 3, 0, 40, 1, false};  // goes off before it can get there
  auto runs = m.roll(&b, 1, 3, 32);
  CHECK(EnemyModel::killOdds(runs) < 0.05);
  // ...and the walker comes by (5,1) early on, never by (5,3) so soon.
  CHECK(EnemyModel::nearOdds(runs, 5, 1, 0, 150) > 0.95);
  CHECK(EnemyModel::nearOdds(runs, 5, 3, 0, 100) < 0.05);
}

TEST("enemies: a chaser heads for P1") {
  World w = mkw({"###########",
                 "#.........#",
                 "###########"},
                "enemy slot=4 type=89 x=24 y=23 vx=0 vy=0 step=- target=1,1 wait=0 hp=1 f4=0C00 c0=0000 d0=0080", 9, 1);
  EnemyModel m(w, 400);
  auto runs = m.roll(nullptr, 9, 1, 8);
  CHECK(EnemyModel::nearOdds(runs, 9, 1, 0, 9 * 32) > 0.95);
}

TEST("enemies: $8C halts once it sees P1 down its column") {
  // P1 at (5,4), the $8C at (5,1) walking down: it stops in sight (column
  // 5, 3 cells) and stays there; a plain chaser would walk on to P1.
  World w = mkw({"#######",
                 "#.....#",
                 "#####.#",
                 "#####.#",
                 "#####.#",
                 "#######"},
                "enemy slot=4 type=8C x=88 y=23 vx=0 vy=0 step=- target=5,1 wait=0 hp=1 f4=0C00 c0=0000 d0=00E5", 5, 4);
  EnemyModel m(w, 200);
  auto runs = m.roll(nullptr, 5, 4, 8);
  CHECK(EnemyModel::nearOdds(runs, 5, 4, 0, 200) < 0.05);
  CHECK(EnemyModel::nearOdds(runs, 5, 1, 150, 200) > 0.95);
}

TEST("enemies: a measured hold keeps a stopped enemy in its cell") {
  World w = mkw({"#########",
                 "#.......#",
                 "#########"},
                "enemy slot=4 type=94 x=56 y=23 vx=0 vy=0 step=- target=3,1 wait=0 hp=1 f4=0C00 c0=0000", 7, 1);
  EnemyModel m(w, 400);
  m.hold = {200};
  auto runs = m.roll(nullptr, 7, 1, 16);
  CHECK(EnemyModel::nearOdds(runs, 1, 1, 0, 180) < 0.05);  // (3,1) -> (1,1) can't happen before it sets off
  CHECK(EnemyModel::nearOdds(runs, 3, 1, 150, 190) > 0.95);
}

TEST("enemies: $A5 walks 1 px/f, unhurt in the mini-boss intro until it moves") {
  // spd=1: 16 frames a cell, no rest when blocked.
  World w = mkw({"#########",
                 "#.......#",
                 "#########"},
                "intro 1\nenemy slot=5 type=A5 x=24 y=23 vx=0 vy=0 step=- target=1,1 wait=0 hp=3 f4=0C00 c0=0000 spd=1", 7, 1);
  CHECK(w.intro);
  EnemyModel m(w, 400);
  m.hold = {100};
  ModelBomb early{2, 1, 0, 60, 2, false};  // flames over it before it moves: no kill
  CHECK(EnemyModel::killOdds(m.roll(&early, 7, 1, 32)) < 0.05);
  // started (a target), out of the intro: the same bomb kills it
  w.intro = false;
  w.enemies[0].tc = 1, w.enemies[0].tr = 1;
  EnemyModel m2(w, 400);
  m2.hold = {100};
  CHECK(EnemyModel::killOdds(m2.roll(&early, 7, 1, 32)) > 0.95);
  // never started (no target): unhurt too, intro or not
  w.enemies[0].tc = 0, w.enemies[0].tr = 0;
  EnemyModel m4(w, 400);
  m4.hold = {100};
  CHECK(EnemyModel::killOdds(m4.roll(&early, 7, 1, 32)) < 0.05);
  // 1 px/f: from (1,1) it reaches (7,1) at about 100 + 6 * 16
  EnemyModel m3(w, 400);
  m3.hold = {100};
  auto runs = m3.roll(nullptr, 1, 1, 16);
  CHECK(EnemyModel::nearOdds(runs, 7, 1, 0, 100 + 4 * 16) < 0.05);
  CHECK(EnemyModel::nearOdds(runs, 6, 1, 0, 100 + 8 * 16) > 0.3);
}

TEST("enemies: $A5 at its spd (1 px/f), turning back at a wall at once") {
  // From (1,1) in a corridor: right only. 16 frames a cell: at (7,1) by
  // ~96 (0.5 px/f: ~192), and straight back (the generic rests 32 first).
  World w = mkw({"#########",
                 "#.......#",
                 "#########"},
                "enemy slot=5 type=A5 x=24 y=23 vx=0 vy=0 step=- target=1,1 wait=0 hp=3 f4=0C00 c0=0000 spd=1", 1, 1);
  EnemyModel m(w, 300);
  m.hold = {8};
  auto runs = m.roll(nullptr, 1, 1, 16);
  CHECK(EnemyModel::nearOdds(runs, 7, 1, 80, 104) > 0.95);
  CHECK(EnemyModel::nearOdds(runs, 4, 1, 110, 145) > 0.95);  // (back by (5,1) at 121..143; generic 153+)
}

TEST("enemies: $86 goes at most 4 cells, rests 128, then heads for P1 in sight") {
  // A long corridor, P1 far off to the right, out of sight (9+ cells).
  World w = mkw({"################",
                 "#..............#",
                 "################"},
                "enemy slot=4 type=86 x=24 y=23 vx=0 vy=0 step=- target=1,1 wait=0 hp=1 f4=0C00 c0=0000 spd=2.5", 14, 1);
  EnemyModel m(w, 400);
  m.hold = {0};
  auto runs = m.roll(nullptr, 14, 1, 16);
  // 2.5 px/f: ~6 frames a cell. 4 cells by ~30, then still there at ~150.
  CHECK(EnemyModel::nearOdds(runs, 7, 1, 0, 150) < 0.05);
  CHECK(EnemyModel::nearOdds(runs, 5, 1, 60, 150) > 0.95);
  // In sight (P1 4 cells off): straight for P1 after the rest.
  World v = mkw({"################",
                 "#..............#",
                 "#.############.#",
                 "#..............#",
                 "################"},
                "enemy slot=4 type=86 x=24 y=23 vx=0 vy=0 step=- target=1,1 wait=0 hp=1 f4=0C00 c0=0000 spd=2.5", 9, 1);
  EnemyModel m2(v, 400);
  m2.hold = {200};
  auto r2 = m2.roll(nullptr, 9, 1, 16);
  CHECK(EnemyModel::nearOdds(r2, 5, 1, 190, 240) > 0.95);
}

// $90/$98 (docs/enemies.md) walk as generic enemies; +$BE bit 15 is a
// mount ($90) or hidden ($98): not an enemy to model then.
TEST("enemies: $90/$98 modelled unless a mount or hidden") {
  Enemy e;
  e.type = 0x98, e.hp = 1;
  CHECK(EnemyModel::modelled(e));
  e.be = 0x8000;
  CHECK(!EnemyModel::modelled(e));
  e.type = 0x90, e.be = 0;
  CHECK(EnemyModel::modelled(e));
  e.hp = 0xFF;  // (firing: hp $FF)
  CHECK(!EnemyModel::modelled(e));
  Enemy a0;
  a0.type = 0xA0, a0.hp = 1;  // (a jump pad)
  CHECK(!EnemyModel::modelled(a0));
}

// A $98 vanishes at +$BC = $1C0 (then reappears anywhere): a blast after
// that hits nothing (3-1: bombs timed past it, a stage of misses).
TEST("enemies: a $98 can't be hit once it vanishes") {
  World w = mkw({"#########",
                 "#.......#",
                 "####.####",
                 "####.####",
                 "#########"}, walker(3, 1, 'l'), 4, 3);
  w.enemies[0].type = 0x98;
  ModelBomb b{4, 1, 10, 160, 3, false};
  w.enemies[0].bc = 0x1C0 - 400;  // (vanishes after the blast)
  CHECK(EnemyModel::killOdds(EnemyModel(w, 400).roll(&b, 4, 3, 64)) > 0.95);
  w.enemies[0].bc = 0x1C0 - 100;  // (gone before it)
  CHECK(EnemyModel::killOdds(EnemyModel(w, 400).roll(&b, 4, 3, 64)) < 0.05);
}

// A $91 stops at hp $FF while P1 is within 4 cells in line, walls blocking
// (docs/enemies.md, C1:BA6B). 5-3: P1 waited 2 cells below one for 9000
// frames; it never moved and could not be hurt.
TEST("enemies: what a $91 sees") {
  World w = mkw({"#########",
                 "#.......#",
                 "#.#.#.#.#",
                 "#.......#",
                 "#.......#",
                 "#.......#",
                 "#########"},
                "enemy slot=0 type=91 x=56 y=24 vx=0 vy=0 step=- target=3,1 wait=0 hp=255");
  Enemy e = w.enemies[0];
  CHECK(EnemyModel::x91Sees(w, e, 3, 3));    // 2 below, (3,2) open
  CHECK(EnemyModel::x91Sees(w, e, 7, 1));    // 4 right
  CHECK(!EnemyModel::x91Sees(w, e, 3, 6));   // 5 below: too far (and a wall)
  CHECK(EnemyModel::x91Sees(w, e, 3, 5));    // 4 below, open: sees
  CHECK(!EnemyModel::x91Sees(w, e, 4, 3));   // not in line
  w.enemies[0].type = 0x83;
  CHECK(!EnemyModel::x91Sees(w, w.enemies[0], 3, 3));  // only a $91
  // a wall between: (2,1) open, the $91 at (1,3) looking up past (1,2)
  World v = mkw({"#####",
                 "#...#",
                 "##..#",
                 "#...#",
                 "#####"},
                "enemy slot=0 type=91 x=24 y=56 vx=0 vy=0 step=- target=1,3 wait=0 hp=255");
  CHECK(!EnemyModel::x91Sees(v, v.enemies[0], 1, 1));
}
