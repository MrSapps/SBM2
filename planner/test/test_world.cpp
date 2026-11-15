// World view parsing (docs/worldview.md).
#include <dirent.h>

#include "test.hpp"
#include "world.hpp"

using namespace sbm2;

static const char* kTiny = R"(worldview 1
stage 10   # 1-1
frame 500
time 200
origin 8 0
size 5 4
light none
gate 3 1 closed
player x=24 y=40 dir=r hearts=1 flash=0 bombs=1 e0=1 fire=2 pierce=0 remote=0 glove=0 softpass=0 lock=0000 ride=0 f4=0600 f60=0000
bomb c=2 r=2 fuse=40 range=2 owner=p1 remote=0 pierce=0 magnet=0 slot=0 flags=80
enemy slot=5 type=94 x=56 y=23 vx=-0.5 vy=0 step=l target=3,1 wait=0 hp=1 f4=0C20 c0=8000 d0=0000 be=0000 bc=0000 ba=0000 off=0
panel 1 1 todo mode=0
future thing=1
map
#####
#P.g#
#.b%#
#####
under
#####
#..g#
#...#
#####
)";

TEST("world: tiny snapshot parses") {
  World w;
  std::string err;
  CHECK(parseWorld(kTiny, w, &err));
  CHECK_EQ(err, std::string());
  CHECK_EQ(w.stage, 0x10);
  CHECK_EQ(w.cols, 5);
  CHECK_EQ(w.rows, 4);
  CHECK_EQ(w.p.col(), 1);
  CHECK_EQ(w.p.row(), 2);
  CHECK_EQ(w.p.fire, 2);
  CHECK_EQ(w.p.dir, 'r');
  CHECK_EQ(w.at(2, 2), 'b');
  CHECK_EQ(w.at(3, 2), '%');
  CHECK_EQ(w.underAt(3, 2), '.');
  CHECK_EQ(w.at(-1, 0), '#');  // outside = wall
  CHECK_EQ(w.at(9, 9), '#');
  CHECK(w.hasGate && !w.gateOpen);
  CHECK(w.bombAt(2, 2) != nullptr);
  CHECK_EQ(w.bombAt(2, 2)->fuse, 40);
  CHECK_EQ(int(w.enemies.size()), 1);
  CHECK_EQ(w.enemies[0].type, 0x94);
  CHECK_EQ(w.enemies[0].vx, -0.5);
  CHECK_EQ(w.enemies[0].tc, 3);
  CHECK(w.enemies[0].flashing());
  CHECK(!w.enemies[0].dormant());
  CHECK_EQ(int(w.panels.size()), 1);
  CHECK(!w.panels[0].done);
  CHECK(w.raw.empty());
  CHECK_EQ(w.rawAt(1, 1), -1);
}

TEST("world: errors name the line") {
  World w;
  std::string err;
  CHECK(!parseWorld("worldview 1\nmap\n#\n", w, &err));
  CHECK_EQ(err, std::string("line 2: map before size"));
  CHECK(!parseWorld("worldview 2\nsize 1 1\nmap\n#\n", w, &err));
  CHECK(!parseWorld("worldview 1\nsize 3 2\nmap\n###\n", w, &err));
  CHECK_EQ(err, std::string("line 4: map layer ends early"));
}

// Every committed snapshot loads, and its map agrees with its raw layer.
TEST("world: committed snapshots load") {
  DIR* d = opendir(sbm2test::worldsDir.c_str());
  CHECK(d != nullptr);
  if (!d) return;
  int n = 0;
  while (dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (name.size() < 4 || name.substr(name.size() - 4) != ".txt") continue;
    World w;
    std::string err;
    bool ok = loadWorld(sbm2test::worldsDir + "/" + name, w, &err);
    if (!ok) std::printf("  %s: %s\n", name.c_str(), err.c_str());
    CHECK(ok);
    if (!ok) continue;
    n++;
    CHECK(w.inside(w.p.col(), w.p.row()));
    for (int r = 0; r < w.rows; r++)
      for (int c = 0; c < w.cols; c++)
        if (w.at(c, r) == '?') {
          std::printf("  %s: unclassified tile %02X at (%d,%d)\n", name.c_str(), w.rawAt(c, r), c, r);
          sbm2test::failures++;
        }
  }
  closedir(d);
  CHECK(n > 0);
}
