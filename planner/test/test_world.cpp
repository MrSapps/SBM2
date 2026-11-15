// World view parsing (docs/worldview.md).
#include <dirent.h>

#include "capture.hpp"
#include "goap.hpp"
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

// $C9 is the closed gate's tile, but an idle mount writes it to its own
// cell while P1 rides another (solid): only the gate's cell reads 'g'.
TEST("capture: a $C9 off the gate's cell is a wall (an idle mount)") {
  std::vector<uint8_t> wram(0x20000, 0);
  auto tile = [&](int c, int r, uint8_t t) { wram[size_t(0x1C000 + r * 0x80 + c * 2)] = t; };
  for (int r = 0; r < 32; r++)
    for (int c = 0; c < 32; c++) tile(c, r, 0xC3);
  for (int c = 1; c <= 6; c++) tile(c, 2, 0x00);
  tile(2, 1, 0xC9);  // the gate
  tile(4, 2, 0xC9);  // a mount
  int gate = 1 * 0x80 + 2 * 2;
  wram[0x5D72] = uint8_t(gate), wram[0x5D73] = uint8_t(gate >> 8);
  World w = captureWorld(wram.data(), 0);
  CHECK(w.hasGate && w.gateC == 2 && w.gateR == 1 && !w.gateOpen);
  CHECK_EQ(w.at(2, 1), 'g');
  CHECK_EQ(w.at(4, 2), '#');
}

// C0:30C8 refuses a bomb on a tile with bit 6 or low bits $01/$11: the
// done panel $70 is floor P1 can't bomb from (2-5: (3,5), behind a push
// block, made the agent retry the push for 8000 frames).
TEST("capture: a done panel is floor that takes no bomb") {
  std::vector<uint8_t> wram(0x20000, 0);
  auto tile = [&](int c, int r, uint8_t t) { wram[size_t(0x1C000 + r * 0x80 + c * 2)] = t; };
  for (int r = 0; r < 32; r++)
    for (int c = 0; c < 32; c++) tile(c, r, 0xC3);
  for (int c = 1; c <= 6; c++) tile(c, 2, 0x00);
  tile(2, 2, 0x70);
  tile(3, 2, 0x11);
  World w = captureWorld(wram.data(), 0);
  CHECK_EQ(w.at(1, 2), '.');
  CHECK_EQ(w.at(2, 2), 'n');
  CHECK_EQ(w.at(3, 2), 'n');
  CHECK(walkableClass('n') && !canPlaceOn('n') && canPlaceOn('.'));
}

// 2-8 (stage $27): a falling bomb (CC:B189) and its marker (CC:B2F1, same
// x) are a range-10 bomb due when the faller lands; a marker with no faller
// yet is due in 60.
TEST("capture: 2-8 boss drops are bombs due when they land") {
  std::vector<uint8_t> wram(0x20000, 0);
  for (int r = 0; r < 32; r++)
    for (int c = 0; c < 32; c++) wram[size_t(0x1C000 + r * 0x80 + c * 2)] = r >= 1 && r <= 12 && c >= 1 && c <= 14 ? 0 : 0xC3;
  wram[0x1A] = 0x27;
  auto obj = [&](int base, uint32_t st, int x, int y) {
    wram[size_t(base)] = uint8_t(st), wram[size_t(base + 1)] = uint8_t(st >> 8), wram[size_t(base + 2)] = uint8_t(st >> 16);
    wram[size_t(base + 0x11)] = uint8_t(x), wram[size_t(base + 0x12)] = uint8_t(x >> 8);
    wram[size_t(base + 0x14)] = uint8_t(y), wram[size_t(base + 0x15)] = uint8_t(y >> 8);
  };
  obj(0x1080, 0xCCB189, 128, uint16_t(-3));  // falling over (8,7)
  obj(0x10C0, 0xCCB2F1, 128, 120);
  obj(0x1100, 0xCCB2F1, 80, 168);             // (5,10): nothing falling yet
  World w = captureWorld(wram.data(), 0);
  CHECK_EQ(w.bombs.size(), size_t(2));
  for (const Bomb& b : w.bombs) {
    CHECK(b.range == 10 && b.enemyOwned && b.slot == -1);
    if (b.c == 8) CHECK(b.r == 7 && b.fuse == (120 + 3) / 2 - 3);
    else CHECK(b.c == 5 && b.r == 10 && b.fuse == 60);
  }
  wram[0x1A] = 0x26;  // other stages: not read
  CHECK(captureWorld(wram.data(), 0).bombs.empty());
}

// The TileMap is a 32x32 ring (C0:BFE2 masks the index): 2-3's off-screen
// enemies in the top rooms read y = 600 on a 29-row map; they are at row
// 37 & 31 = 5.
TEST("capture: enemy positions wrap like the 32x32 map") {
  std::vector<uint8_t> wram(0x20000, 0);
  for (int r = 0; r < 32; r++)
    for (int c = 0; c < 32; c++) wram[size_t(0x1C000 + r * 0x80 + c * 2)] = r >= 1 && r <= 12 && c >= 1 && c <= 14 ? 0 : 0xC3;
  int base = 0x8500;
  wram[size_t(base + 1)] = 0xA0;  // live, off screen
  wram[size_t(base + 0x7E)] = 0x92;
  wram[size_t(base + 0x43)] = 1;
  wram[size_t(base + 0x10)] = 136;
  wram[size_t(base + 0x18)] = uint8_t(600 & 0xFF), wram[size_t(base + 0x19)] = uint8_t(600 >> 8);
  World w = captureWorld(wram.data(), 0);
  CHECK_EQ(w.enemies.size(), size_t(1));
  CHECK(w.enemies.size() == 1 && w.enemies[0].col() == 8 && w.enemies[0].row() == 5);
}
