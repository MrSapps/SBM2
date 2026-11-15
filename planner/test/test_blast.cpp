// Blast model (planner/blast.cpp) on invented maps.
#include <algorithm>

#include "blast.hpp"
#include "test.hpp"

using namespace sbm2;

// A world from map rows plus extra header lines (bombs etc.).
static World mk(const std::vector<std::string>& rows, const std::string& extra = "") {
  std::string s = "worldview 1\nsize " + std::to_string(rows[0].size()) + " " + std::to_string(rows.size()) +
                  "\nplayer x=8 y=8\n" + extra + "\nmap\n";
  for (auto& r : rows) s += r + "\n";
  World w;
  std::string err;
  if (!parseWorld(s, w, &err)) std::printf("  mk: %s\n", err.c_str());
  return w;
}

static bool has(const std::vector<Cell>& v, int c, int r) {
  return std::find(v.begin(), v.end(), Cell{c, r}) != v.end();
}

TEST("blast: walls stop it, soft blocks are reached and stop it") {
  World w = mk({"#######",
                "#..b.%.",
                "#######"});
  auto cells = blastCells(w, 3, 1, 5, false);
  CHECK(has(cells, 3, 1));
  CHECK(has(cells, 1, 1));
  CHECK(!has(cells, 0, 1));  // wall
  CHECK(has(cells, 5, 1));   // soft block reached
  CHECK(!has(cells, 6, 1));  // ...and it stops there
  CHECK(!has(cells, 3, 0));
}

TEST("blast: pierce goes through soft blocks to full range") {
  World w = mk({"#########",
                "#b.%%...#",
                "#########"});
  auto cells = blastCells(w, 1, 1, 5, true);
  CHECK(has(cells, 3, 1));
  CHECK(has(cells, 4, 1));
  CHECK(has(cells, 6, 1));
  CHECK(!has(cells, 7, 1));  // range 5
}

TEST("blast: stairs and $4B traps stop it after the cell, $63 traps don't") {
  World w = mk({"#########",
                "#b.^..t.#",
                "#.#######",
                "#T#######",
                "#.#######"});
  auto cells = blastCells(w, 1, 1, 6, false);
  CHECK(has(cells, 3, 1));
  CHECK(!has(cells, 4, 1));
  CHECK(has(cells, 1, 3));
  CHECK(!has(cells, 1, 4));
  World w2 = mk({"#########",
                 "#b...t..#",
                 "#########"});
  CHECK(has(blastCells(w2, 1, 1, 6, false), 6, 1));
}

TEST("blast: items stop it unless through") {
  World w = mk({"#######",
                "#b.i..#",
                "#######"});
  CHECK(!has(blastCells(w, 1, 1, 5, false, false), 4, 1));
  CHECK(has(blastCells(w, 1, 1, 5, false, true), 4, 1));
}

TEST("blast: an open gate lets it through, a closed one stops it") {
  World open = mk({"#######",
                   "#b.g..#",
                   "#######"}, "gate 3 1 open");
  World shut = mk({"#######",
                   "#b.g..#",
                   "#######"}, "gate 3 1 closed");
  CHECK(has(blastCells(open, 1, 1, 5, false), 4, 1));
  CHECK(has(blastCells(shut, 1, 1, 5, false), 3, 1));
  CHECK(!has(blastCells(shut, 1, 1, 5, false), 4, 1));
}

TEST("danger: windows and chain reactions") {
  World w = mk({"#########",
                "#b...b..#",
                "#########"},
               "bomb c=1 r=1 fuse=30 range=4\nbomb c=5 r=1 fuse=120 range=2");
  DangerMap d(w);
  CHECK_EQ(d.times()[0], 30);
  CHECK_EQ(d.times()[1], 30);  // reached by the first: goes off with it
  CHECK(d.deadly(7, 1, 26, 26));   // the second bomb's far end, chained
  CHECK(!d.deadly(7, 1, 0, 25));
  CHECK(!d.deadly(7, 1, 30 + kFlameFrames + 1, 500));
}

TEST("danger: remote bombs wait for a press") {
  World w = mk({"#######",
                "#b....#",
                "#######"},
               "bomb c=1 r=1 fuse=0 range=3 remote=1");
  DangerMap d(w);
  CHECK(!d.deadly(2, 1, 0, 10000));
  d.addBomb(4, 1, 3, false, 50);  // ...or a blast
  CHECK(d.deadly(2, 1, 50, 50));
}

TEST("danger: existing flames") {
  World w = mk({"#####",
                "#.*.#",
                "#####"});
  DangerMap d(w);
  CHECK(d.deadly(2, 1, 0, 0));
  CHECK(!d.deadly(1, 1, 0, 1000));
}
