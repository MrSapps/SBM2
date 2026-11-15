// World view (docs/worldview.md): one frame of the game, facts only.
// The planner reads nothing else, so every decision can be replayed from text.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sbm2 {

struct Player {
  int x = 0, y = 0;  // map pixels
  char dir = '-';    // u d l r -
  int hearts = 0, flash = 0, bombs = 0, e0 = 0, fire = 1;
  bool pierce = false, remote = false, ride = false;
  int glove = 0, softpass = 0;
  uint16_t lock = 0, f4 = 0, f60 = 0;
  int col() const { return x / 16; }
  int row() const { return y / 16; }
};

struct Bomb {
  int c = 0, r = 0, fuse = 0, range = 1, slot = 0, flags = 0;
  bool enemyOwned = false, remote = false, pierce = false, magnet = false;
};

struct Enemy {
  int slot = 0, type = 0, x = 0, y = 0;
  double vx = 0, vy = 0;
  double spd = 0;  // its walking speed, px/f (+$46 + $48's high byte / 256; 0: unknown)
  char step = '-';
  int tc = 0, tr = 0, wait = 0, hp = 0;
  uint16_t f4 = 0, c0 = 0, d0 = 0, be = 0, bc = 0, ba = 0;
  bool offscreen = false;
  int col() const { return x / 16; }
  int row() const { return y / 16; }
  bool dormant() const { return f4 & 0x8000; }
  bool hidden() const { return f4 & 0x40; }
  bool flashing() const { return f4 & 0x20; }
};

struct Panel { int c = 0, r = 0, mode = 0; bool done = false; };
struct Item { int c = 0, r = 0; bool heart = false; };
struct Magnet { int c = 0, r = 0; char dir = 'u'; };

// A boss-engine object ($1000-$17FF, docs/enemies.md 8) as the boss code
// itself checked it last frame: the contact box it tested P1 against
// (BossContactP1 CC:A538) and the points it looked for flames under
// (BossFlameSense CC:A266). The host's exec hooks see these; WRAM alone
// doesn't hold them.
struct BossObj {
  int base = 0;
  uint32_t state = 0;  // +$00..+$02: the routine it runs
  int x = 0, y = 0;    // world pixels (+$11/+$14 + origin, a blink's +$100 taken off)
  int hp = 0;          // +$20
  bool hurt = false;   // +$26 running: no flame is sensed
  bool hasBox = false;
  int box[4] = {0, 0, 0, 0};  // dx0, dx1, dy0, dy1: P1 caught when x+dx0 < p1x <= x+dx1-1, same for y
  std::vector<std::pair<int, int>> sense;  // (dx, dy)
};

struct World {
  int version = 0, stage = 0, frame = 0, time = 0;
  int ox = 0, oy = 0, cols = 0, rows = 0;
  std::string light = "none";
  // A mini-boss intro is on ($7E5E48, set when a $A4-$A8 spawns): those
  // enemies skip their flame check (C1:67FD) until it ends.
  bool intro = false;
  bool hasGate = false, gateOpen = false;
  int gateC = 0, gateR = 0;
  Player p;
  std::vector<Bomb> bombs;
  std::vector<Enemy> enemies;
  std::vector<Panel> panels;
  std::vector<Item> items;
  std::vector<Magnet> magnets;
  std::vector<BossObj> bosses;
  std::optional<std::pair<int, int>> lightSwitch;
  std::vector<char> map, under;  // cell classes, rows * cols
  std::vector<uint8_t> raw;      // tile codes (empty if the snapshot has none)
  std::vector<uint8_t> rawUnder; // CellItems codes (empty if none)

  bool inside(int c, int r) const { return c >= 0 && r >= 0 && c < cols && r < rows; }
  // Outside the playfield is wall.
  char at(int c, int r) const { return inside(c, r) ? map[r * cols + c] : '#'; }
  char underAt(int c, int r) const { return inside(c, r) && !under.empty() ? under[r * cols + c] : '#'; }
  int rawAt(int c, int r) const { return inside(c, r) && !raw.empty() ? raw[r * cols + c] : -1; }
  const Bomb* bombAt(int c, int r) const;
};

// Parse a snapshot. On failure returns false and sets *err ("line N: ...").
bool parseWorld(const std::string& text, World& w, std::string* err = nullptr);
bool loadWorld(const std::string& path, World& w, std::string* err = nullptr);

}  // namespace sbm2
