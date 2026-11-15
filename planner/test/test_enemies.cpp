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
