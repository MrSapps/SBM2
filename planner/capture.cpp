#include <cstring>
#include "capture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sbm2 {

namespace {

constexpr int kMap = 0x1C000, kUnder = 0x1D000, kRow = 0x80, kMapW = 32, kMapH = 32;
constexpr int kP1 = 0x8000, kBombs = 0x12000, kBombSlots = 40, kBombStride = 0x100;

// Tile code -> cell class (docs/worldview.md; the same table as worldview.lua).
struct ClassTable {
  char t[256];
  ClassTable() {
    for (int i = 0; i < 256; i++) t[i] = '?';
    for (int i = 0x04; i <= 0x3F; i++) t[i] = '.';
    t[0x00] = '.';
    for (int i = 0x01; i <= 0x03; i++) t[i] = '*';
    for (int i = 0x40; i <= 0x43; i++) t[i] = 'b';
    // Floor the game won't take a bomb on (C0:30C8: the tile under P1 has
    // bit 6, or its low 6 bits are $01/$11): done panels etc. (2-5: a bomb
    // on the done panel $70 at (3,5) to push (5,5) was never placed).
    for (int i : {0x6F, 0x70, 0x72, 0x11}) t[i] = 'n';
    t[0x6B] = 'v';
    t[0x44] = 'i';
    for (int i = 0xC3; i <= 0xCF; i++) t[i] = '#';
    t[0xC2] = '%';
    t[0xC9] = 'g';
    t[0x49] = 'g';
    t[0xD3] = 'O';
    t[0xE2] = 'X';
    t[0xF5] = 'x';
    t[0xD0] = 'w';
    for (int i : {0xD2, 0xD5, 0xE0, 0xE1}) t[i] = 'm';
    t[0xD6] = 'B';
    t[0xE7] = 'F';
    t[0xCD] = 'K';
    t[0x4D] = 'k';
    t[0x4C] = 'S';
    t[0x36] = 'L';
    t[0x71] = '#';  // 4-4 (2,5), on top and under, where the spawner was bombed: solid as far as seen
    t[0x6A] = 'a';  // platform spawner (a flame on it: one more $DC)  // platform lane end (board cell, docs/mechanics.md 7)
    t[0x4B] = 'T';
    t[0x63] = 't';
    t[0x25] = 'c';
    t[0x65] = 'C';
    for (int i : {0x69, 0x73, 0x74}) t[i] = 'P';
    t[0x68] = 'y';
    t[0x64] = t[0x6E] = 's';
    t[0x14] = 'p';
    t[0x0E] = 'f';
    t[0x66] = '^';
    for (int i = 0x17; i <= 0x1A; i++) t[i] = '=';
  }
};
const ClassTable kClass;

struct Rd {
  const uint8_t* w;
  int b(int a) const { return w[a & (kWramSize - 1)]; }
  int h(int a) const { return b(a) | b(a + 1) << 8; }
  int s(int a) const {
    int v = h(a);
    return v >= 0x8000 ? v - 0x10000 : v;
  }
};

char stepName(int v) {
  if (v >= 0x8000) v -= 0x10000;
  if (v == -0x80) return 'u';
  if (v == 0x80) return 'd';
  if (v == -2) return 'l';
  if (v == 2) return 'r';
  return '-';
}

}  // namespace

World captureWorld(const uint8_t* wram, int frame) {
  Rd rd{wram};
  World w;
  w.version = 1;
  w.frame = frame;
  w.stage = rd.b(0x1A);
  w.time = rd.h(0x5E10) * 60 + rd.h(0x5E12);
  w.ox = rd.s(0x5F01);
  w.oy = rd.s(0x5EFB);
  int light = rd.h(0x5AAC);
  w.light = light == 0xFFFF ? "none" : ((light & 1) ? "on" : "off");
  w.intro = rd.h(0x5E48) != 0;

  auto raw = [&](int c, int r) { return rd.b(kMap + r * kRow + c * 2); };
  auto under = [&](int c, int r) { return rd.b(kUnder + r * kRow + c * 2); };
  int fill = raw(kMapW - 1, kMapH - 1), maxC = 0, maxR = 0;
  for (int r = 0; r < kMapH; r++)
    for (int c = 0; c < kMapW; c++)
      if (raw(c, r) != fill) {
        if (c > maxC) maxC = c;
        if (r > maxR) maxR = r;
      }
  w.cols = maxC + 1;
  w.rows = maxR + 1;
  for (int r = 0; r < w.rows; r++)
    for (int c = 0; c < w.cols; c++) {
      w.raw.push_back(uint8_t(raw(c, r)));
      w.rawUnder.push_back(uint8_t(under(c, r)));
      w.map.push_back(kClass.t[raw(c, r)]);
      w.under.push_back(kClass.t[under(c, r)]);
    }

  int gate = rd.h(0x5D72);
  if (gate != 0 && gate != 0xFFFF) {
    w.hasGate = true;
    w.gateC = (gate & 0x7F) / 2;
    w.gateR = gate >> 7;
    w.gateOpen = w.gateR < kMapH && raw(w.gateC, w.gateR) == 0x49;
  }
  // $C9 off the gate's cell: a mount standing idle while P1 rides one
  // (C1:C75A writes $C9 to its cell, solid, until P1 gets off; 3-3 rode
  // into one in a 1-wide way and stood there for 12000 frames).
  for (int r = 0; r < w.rows; r++)
    for (int c = 0; c < w.cols; c++)
      if (raw(c, r) == 0xC9 && !(w.hasGate && c == w.gateC && r == w.gateR)) w.map[size_t(r * w.cols + c)] = '#';

  Player& p = w.p;
  int f4 = rd.h(kP1 + 4), d = rd.h(kP1 + 0x6C), stats = rd.b(kP1 + 0xF3);
  p.x = rd.s(kP1 + 0x10) + w.ox;
  p.y = rd.s(kP1 + 0x18) + w.oy;
  p.dir = (d & 8) ? 'u' : (d & 4) ? 'd' : (d & 2) ? 'l' : (d & 1) ? 'r' : '-';
  p.hearts = rd.b(kP1 + 0xEF);
  p.flash = (f4 & 0x20) ? rd.b(kP1 + 0xB9) : 0;
  p.bombs = rd.b(kP1 + 0xF1);
  p.maxbombs = rd.b(kP1 + 0xE1);
  p.e0 = rd.b(kP1 + 0xE0);
  p.fire = rd.b(kP1 + 0xE2) + 1;
  p.pierce = stats & 4;
  p.remote = stats & 1;
  p.glove = rd.b(kP1 + 0xFE);
  p.softpass = rd.b(kP1 + 0xFB);
  p.kick = rd.b(kP1 + 0xFA);
  p.lock = uint16_t(rd.h(kP1 + 0x52));
  p.ride = rd.h(kP1 + 2) & 0x8000;
  p.f4 = uint16_t(f4);
  p.f60 = uint16_t(rd.h(kP1 + 0x60));

  for (int i = 0; i < kBombSlots; i++) {
    int base = kBombs + i * kBombStride, flags = rd.b(base + 1);
    if (!(flags & 0x80)) continue;
    int cell = rd.h(base + 2);
    Bomb b;
    b.slot = i;
    b.c = (cell & 0x3F) / 2;
    b.r = cell >> 6;
    b.fuse = (flags & 0x40) ? 0 : rd.b(base + 4) * 2;
    b.range = rd.b(base + 7) + 1;
    b.enemyOwned = flags & 0x08;
    b.remote = rd.b(base) & 1;
    b.pierce = rd.b(base) & 4;
    b.magnet = flags & 0x10;
    b.flags = flags;
    w.bombs.push_back(b);
  }
  // 2-8 boss drops (docs/enemies.md 6b): a bomb falls at 2 px/f (state
  // CC:B189) onto the marker under it (CC:B2F1, same x) and goes off on
  // landing, range 10. Each marker is a bomb due when the faller lands
  // (none seen yet: 60 frames). slot -1: not in the bomb table.
  if (w.stage == 0x27) {
    auto st = [&](int base) { return rd.h(base) | rd.b(base + 2) << 16; };
    std::vector<std::pair<int, int>> falls;
    for (int base = 0x1040; base <= 0x17C0; base += 0x40)
      if (st(base) == 0xCCB189) falls.push_back({rd.h(base + 0x11), rd.s(base + 0x14)});
    for (int base = 0x1040; base <= 0x17C0; base += 0x40) {
      if (st(base) != 0xCCB2F1) continue;
      int mx = rd.h(base + 0x11), my = rd.h(base + 0x14), t = 60;
      for (auto& f : falls)
        if (std::abs(f.first - mx) <= 2) t = std::max(0, (my - f.second) / 2 - 3);
      Bomb b;
      b.slot = -1;
      b.c = (mx + w.ox) / 16;
      b.r = (my + w.oy) / 16;
      b.fuse = t;
      b.range = 10;
      b.enemyOwned = true;
      bool dup = false;
      for (const Bomb& o : w.bombs) dup |= o.c == b.c && o.r == b.r;
      if (!dup) w.bombs.push_back(b);
    }
  }

  for (int base = 0x8400; base <= 0x8F00; base += 0x100) {
    int s0 = rd.h(base), h = s0 & 0xF000;
    if (h != 0x9000 && h != 0xA000) continue;
    auto vel = [&](int a, int negBit) {
      double v = rd.h(a) + rd.h(a + 2) / 65536.0;
      return (s0 & negBit) ? -v : v;
    };
    Enemy e;
    e.slot = (base >> 8) - 0x80;
    e.type = rd.b(base + 0x7E);
    // (positions wrap at 512 px like the 32x32 TileMap, C0:BFE2: 2-3's
    // off-screen enemies in the top rooms read 32 rows too far down)
    e.x = (rd.s(base + 0x10) + w.ox) & 511;
    e.y = (rd.s(base + 0x18) + w.oy) & 511;
    e.vx = vel(base + 0x14, 1);
    e.vy = vel(base + 0x1C, 2);
    e.spd = rd.h(base + 0x46) + rd.b(base + 0x49) / 256.0;
    e.step = stepName(rd.h(base + 0x4A));
    int tgt = rd.h(base + 0x4C);
    e.tc = (tgt & 0x7F) / 2;
    e.tr = (tgt >> 7) & 0x1F;
    e.wait = rd.h(base + 0x4E);
    e.hp = rd.b(base + 0x43);
    e.f4 = uint16_t(rd.h(base + 4));
    e.c0 = uint16_t(rd.h(base + 0xC0));
    e.d0 = uint16_t(rd.h(base + 0xD0));
    e.be = uint16_t(rd.h(base + 0xBE));
    e.bc = uint16_t(rd.h(base + 0xBC));
    e.ba = uint16_t(rd.h(base + 0xBA));
    e.offscreen = h == 0xA000;
    w.enemies.push_back(e);
  }

  int pstate = rd.h(0x5D94);
  if (pstate != 0xFFFF) {
    for (int a = 0x5B56; rd.h(a) != 0xFFFF && a < 0x5B56 + 8 * 32; a += 8) {
      int cell = rd.h(a + 2), c = (cell & 0x7F) / 2, r = cell >> 7;
      bool done = (pstate & 1) == 0 ? raw(c, r) != 0x69 : (rd.h(a) & 1) != 0;
      w.panels.push_back({c, r, pstate & 1, done});
    }
  }
  // (C0:F428 fills $7F0300 = WRAM $10300 on regrow stages only, 0-ended)
  for (int i = 0; i < 0x800; i += 2) {
    int cell = rd.h(0x10300 + i);
    if (!cell) break;
    w.regrowCells.push_back({(cell & 0x7F) / 2, cell >> 7});
  }
  if (!w.regrowCells.empty()) {
    w.regrowIn = rd.h(0x5D0A);
    if (rd.h(0x10B00)) {
      int cell = rd.h(0x10B02);
      w.regrowing = std::make_pair((cell & 0x7F) / 2, cell >> 7);
    }
  }
  if (light != 0xFFFF) {
    int cell = rd.h(0x5AAE);
    w.lightSwitch = std::make_pair((cell & 0x7F) / 2, cell >> 7);
  }

  // The item list: {cell, kind}; kind bit 14 is set once its block broke.
  int kindAt[kMapH * kRow];
  std::fill(kindAt, kindAt + kMapH * kRow, -1);
  for (int i = 0; i < 64; i++) {
    int cell = rd.h(0x3C48 + 4 * i), kind = rd.h(0x3C48 + 4 * i + 2);
    if (cell >= 0x8000) break;
    int c = (cell & 0x7F) / 2, r = cell >> 7;
    if (r >= kMapH) continue;
    kindAt[cell] = kind & 0x3FFF;
    if (!(kind & 0x4000) && kClass.t[raw(c, r)] == '%') w.hidden.push_back({c, r, (kind & 0x3FFF) == kItemHeart, kind & 0x3FFF});
  }
  for (int r = 0; r < kMapH; r++)
    for (int c = 0; c < kMapW; c++)
      if (raw(c, r) == 0x44) w.items.push_back({c, r, kindAt[r * kRow + c * 2] == kItemHeart, kindAt[r * kRow + c * 2]});

  static const char kMd[8] = {'u', 0, 'r', 0, 'd', 0, 'l', 0};
  for (int i = 0; i < 4; i++) {
    int m = 0x5828 + i * 0x40, cell = rd.h(m + 2), dir = rd.h(m + 8);
    int c = (cell & 0x7F) / 2, r = cell >> 7;
    if (dir < 8 && kMd[dir] && r < kMapH && kClass.t[raw(c, r)] == 'm') w.magnets.push_back({c, r, kMd[dir]});
  }
  return w;
}

namespace {
// worldview.lua's num(): integers plain, else up to 4 decimals, trailing 0s cut.
std::string num(double v) {
  if (v == 0) return "0";
  if (v == std::floor(v) && std::fabs(v) < 1e9) return std::to_string(long(v));
  char b[64];
  std::snprintf(b, sizeof b, "%.4f", v);
  std::string s = b;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}
}  // namespace

std::string encodeWorld(const World& w) {
  std::string o;
  char b[512];
  auto add = [&](const std::string& s) { o += s + "\n"; };
  add("worldview 1");
  std::snprintf(b, sizeof b, "stage %02X", w.stage);
  add(b);
  add("frame " + std::to_string(w.frame));
  add("time " + std::to_string(w.time));
  std::snprintf(b, sizeof b, "origin %d %d", w.ox, w.oy);
  add(b);
  std::snprintf(b, sizeof b, "size %d %d", w.cols, w.rows);
  add(b);
  add("light " + w.light);
  if (w.intro) add("intro 1");
  if (w.hasGate) {
    std::snprintf(b, sizeof b, "gate %d %d %s", w.gateC, w.gateR, w.gateOpen ? "open" : "closed");
    add(b);
  }
  const Player& p = w.p;
  std::snprintf(b, sizeof b,
                "player x=%d y=%d dir=%c hearts=%d flash=%d bombs=%d maxbombs=%d e0=%d fire=%d pierce=%d remote=%d glove=%d "
                "softpass=%d lock=%04X ride=%d f4=%04X f60=%04X",
                p.x, p.y, p.dir, p.hearts, p.flash, p.bombs, p.maxbombs, p.e0, p.fire, int(p.pierce), int(p.remote), p.glove,
                p.softpass, p.lock, int(p.ride), p.f4, p.f60);
  if (p.kick) std::snprintf(b + std::strlen(b), sizeof b - std::strlen(b), " kick=%d", p.kick);  // (only when held: older snapshots have none)
  add(b);
  for (const Bomb& m : w.bombs) {
    std::snprintf(b, sizeof b, "bomb c=%d r=%d fuse=%d range=%d owner=%s remote=%d pierce=%d magnet=%d slot=%d flags=%02X", m.c,
                  m.r, m.fuse, m.range, m.enemyOwned ? "enemy" : "p1", int(m.remote), int(m.pierce), int(m.magnet),
                  m.slot, m.flags);
    add(b);
  }
  for (const Enemy& e : w.enemies) {
    std::snprintf(b, sizeof b,
                  "enemy slot=%d type=%02X x=%d y=%d vx=%s vy=%s step=%c target=%d,%d wait=%d hp=%d f4=%04X c0=%04X "
                  "d0=%04X be=%04X bc=%04X ba=%04X off=%d%s",
                  e.slot, e.type, e.x, e.y, num(e.vx).c_str(), num(e.vy).c_str(), e.step, e.tc, e.tr, e.wait, e.hp,
                  e.f4, e.c0, e.d0, e.be, e.bc, e.ba, int(e.offscreen),
                  e.spd > 0 ? (" spd=" + num(e.spd)).c_str() : "");
    add(b);
  }
  for (const Panel& pn : w.panels) {
    std::snprintf(b, sizeof b, "panel %d %d %s mode=%d", pn.c, pn.r, pn.done ? "done" : "todo", pn.mode);
    add(b);
  }
  for (const Ride& rd : w.rides) {
    std::snprintf(b, sizeof b, "ride %d %d %d %d dock_c=%d dock_r=%d", rd.c, rd.r, rd.tc, rd.tr, rd.dc, rd.dr);
    add(b);
  }
  if (w.lightSwitch) {
    std::snprintf(b, sizeof b, "switch %d %d", w.lightSwitch->first, w.lightSwitch->second);
    add(b);
  }
  if (!w.regrowCells.empty()) {
    std::string t = "regrow in=" + std::to_string(w.regrowIn);
    if (w.regrowing) t += " growing=" + std::to_string(w.regrowing->first) + "," + std::to_string(w.regrowing->second);
    t += " cells=";
    for (size_t i = 0; i < w.regrowCells.size(); i++)
      t += (i ? ";" : "") + std::to_string(w.regrowCells[i].first) + "," + std::to_string(w.regrowCells[i].second);
    add(t);
  }
  for (const Item& it : w.items) {
    std::snprintf(b, sizeof b, "item %d %d %s kind=%d", it.c, it.r, it.heart ? "heart" : "item", it.kind);
    add(b);
  }
  for (const Item& it : w.hidden) {
    std::snprintf(b, sizeof b, "hidden %d %d kind=%d", it.c, it.r, it.kind);
    add(b);
  }
  for (const Magnet& m : w.magnets) {
    std::snprintf(b, sizeof b, "magnet %d %d dir=%c", m.c, m.r, m.dir);
    add(b);
  }
  for (const BossObj& m : w.bosses) {
    std::string s;
    std::snprintf(b, sizeof b, "boss base=%04X state=%06X x=%d y=%d hp=%d hurt=%d", m.base, unsigned(m.state), m.x, m.y, m.hp,
                  int(m.hurt));
    s = b;
    if (m.hasBox) {
      std::snprintf(b, sizeof b, " box=%d,%d,%d,%d", m.box[0], m.box[1], m.box[2], m.box[3]);
      s += b;
    }
    if (!m.sense.empty()) {
      s += " sense=";
      for (size_t i = 0; i < m.sense.size(); i++) {
        std::snprintf(b, sizeof b, "%s%d,%d", i ? ";" : "", m.sense[i].first, m.sense[i].second);
        s += b;
      }
    }
    add(s);
  }
  auto grid = [&](const char* name, const std::vector<char>& v) {
    add(name);
    for (int r = 0; r < w.rows; r++) add(std::string(v.begin() + r * w.cols, v.begin() + (r + 1) * w.cols));
  };
  auto hexGrid = [&](const char* name, const std::vector<uint8_t>& v) {
    add(name);
    for (int r = 0; r < w.rows; r++) {
      std::string s;
      for (int c = 0; c < w.cols; c++) {
        std::snprintf(b, sizeof b, "%s%02X", c ? " " : "", v[size_t(r * w.cols + c)]);
        s += b;
      }
      add(s);
    }
  };
  grid("map", w.map);
  if (!w.under.empty()) grid("under", w.under);
  if (!w.raw.empty()) hexGrid("raw", w.raw);
  if (!w.rawUnder.empty()) hexGrid("rawunder", w.rawUnder);
  return o;
}

}  // namespace sbm2
