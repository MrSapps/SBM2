// World view parsing (docs/worldview.md).
#include <dirent.h>

#include "capture.hpp"
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

// The item list $7E3C48 ({cell, kind}, $FFFF ends; kind bit 14: its block
// broke): a hidden item sits in an unbroken soft block, a visible one is a
// $44 tile; both carry their kind. maxbombs is +$E1.
TEST("capture: items keep their kind; ones still in a block are hidden") {
  std::vector<uint8_t> wram(0x20000, 0);
  auto tile = [&](int c, int r, uint8_t t) { wram[size_t(0x1C000 + r * 0x80 + c * 2)] = t; };
  for (int r = 0; r < 32; r++)
    for (int c = 0; c < 32; c++) tile(c, r, 0xC3);
  for (int c = 1; c <= 6; c++) tile(c, 2, 0x00);
  tile(3, 2, 0xC2);  // soft block, bomb up inside
  tile(5, 2, 0x44);  // fire up, out
  tile(6, 2, 0xC2);  // soft block, nothing listed
  auto entry = [&](int i, int c, int r, int kind) {
    int a = 0x3C48 + 4 * i, cell = r * 0x80 + c * 2;
    wram[size_t(a)] = uint8_t(cell), wram[size_t(a + 1)] = uint8_t(cell >> 8);
    wram[size_t(a + 2)] = uint8_t(kind), wram[size_t(a + 3)] = uint8_t(kind >> 8);
  };
  entry(0, 3, 2, 0);
  entry(1, 5, 2, 2 | 0x4000);
  entry(2, 4, 9, 11 | 0x4000);  // (broke, picked up: no tile there now)
  wram[0x3C48 + 12] = wram[0x3C48 + 13] = 0xFF;
  wram[0x8000 + 0xE1] = 3;
  wram[0x8000 + 0xF1] = 2;
  World w = captureWorld(wram.data(), 0);
  CHECK_EQ(w.p.maxbombs, 3);
  CHECK_EQ(w.p.bombs, 2);
  CHECK_EQ(w.hidden.size(), size_t(1));
  CHECK(w.hidden.size() == 1 && w.hidden[0].c == 3 && w.hidden[0].r == 2 && w.hidden[0].kind == kItemBomb);
  CHECK_EQ(w.items.size(), size_t(1));
  CHECK(w.items.size() == 1 && w.items[0].c == 5 && w.items[0].kind == kItemFire && !w.items[0].heart);
  // ...and the text form keeps them.
  World x;
  CHECK(parseWorld(encodeWorld(w), x));
  CHECK(x.hidden.size() == 1 && x.hidden[0].kind == kItemBomb && x.items.size() == 1 && x.items[0].kind == kItemFire);
  CHECK_EQ(x.p.maxbombs, 3);
}
