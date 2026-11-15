#include "blast.hpp"

namespace sbm2 {

namespace {
const int kDirs[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};

void cellsInto(const World& w, int c, int r, int range, bool pierce, bool through, bool inBarrel,
               std::vector<Cell>& out) {
  out.push_back({c, r});
  for (auto& d : kDirs) {
    for (int k = 1; k <= range; k++) {
      int cc = c + d[0] * k, rr = r + d[1] * k;
      if (!w.inside(cc, rr)) break;
      char t = w.at(cc, rr);
      if (pierce && (t == '%' || t == 'i' || t == 'T')) {
        out.push_back({cc, rr});
        continue;
      }
      if (t == '%' || t == 'X' || (t == 'g' && !(w.hasGate && w.gateOpen && cc == w.gateC && rr == w.gateR))) {
        out.push_back({cc, rr});  // reached, and stops it
        break;
      }
      if (t == 'B') {
        if (!inBarrel) {
          // BarrelExplode C1:26AA: bursts on the barrel and 2 cells away
          // (skipped where the tile has bit 6 set: walls and the like).
          cellsInto(w, cc, rr, 2, true, true, true, out);
          for (auto& e : kDirs) {
            int bc = cc + 2 * e[0], br = rr + 2 * e[1];
            int raw = w.rawAt(bc, br);
            bool bit6 = raw >= 0 ? (raw & 0x40) : (w.at(bc, br) == '#');
            if (w.inside(bc, br) && !bit6) cellsInto(w, bc, br, 2, true, true, true, out);
          }
        }
        break;
      }
      if (t == '#' || t == 'O' || t == 'w' || t == 'm' || t == 'F' || t == 'K') break;  // solid
      out.push_back({cc, rr});
      if (t == '^' || t == 'T' || (t == 'i' && !through)) break;
    }
  }
}
}  // namespace

std::vector<Cell> blastCells(const World& w, int c, int r, int range, bool pierce, bool through) {
  std::vector<Cell> out;
  cellsInto(w, c, r, range, pierce, through, false, out);
  return out;
}

DangerMap::DangerMap(const World& w) : w_(w) {
  for (const Bomb& b : w.bombs) {
    int t = b.fuse;
    if (b.remote && !b.enemyOwned) t = kNever;
    bombs_.push_back({b.c, b.r, b.range, b.pierce, t});
  }
  rebuild();
}

void DangerMap::addBomb(int c, int r, int range, bool pierce, int fuse) {
  bombs_.push_back({c, r, range, pierce, fuse});
  rebuild();
}

void DangerMap::rebuild() {
  size_t n = bombs_.size();
  times_.assign(n, 0);
  for (size_t i = 0; i < n; i++) times_[i] = bombs_[i].t;
  std::vector<std::vector<Cell>> cells(n);
  for (size_t i = 0; i < n; i++)
    cells[i] = blastCells(w_, bombs_[i].c, bombs_[i].r, bombs_[i].range, bombs_[i].pierce, true);
  // Chain reactions: relax until no bomb goes off earlier.
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 0; i < n; i++) {
      if (times_[i] >= kNever) continue;
      for (const Cell& x : cells[i])
        for (size_t j = 0; j < n; j++)
          if (j != i && bombs_[j].c == x.c && bombs_[j].r == x.r && times_[j] > times_[i]) {
            times_[j] = times_[i];
            changed = true;
          }
    }
  }
  win_.assign(size_t(w_.cols * w_.rows), {});
  auto add = [&](int c, int r, int t0, int t1) {
    if (w_.inside(c, r)) win_[size_t(r * w_.cols + c)].push_back({t0, t1});
  };
  for (size_t i = 0; i < n; i++) {
    if (times_[i] >= kNever) continue;
    for (const Cell& x : cells[i]) add(x.c, x.r, times_[i] - kBlastLead, times_[i] + kFlameFrames);
  }
  for (int r = 0; r < w_.rows; r++)
    for (int c = 0; c < w_.cols; c++)
      if (w_.at(c, r) == '*') add(c, r, -kFlameFrames, kFlameFrames / 2);
}

const std::vector<Window>& DangerMap::at(int c, int r) const {
  static const std::vector<Window> none;
  return w_.inside(c, r) ? win_[size_t(r * w_.cols + c)] : none;
}

bool DangerMap::deadly(int c, int r, int t0, int t1) const {
  for (const Window& x : at(c, r))
    if (x.t0 <= t1 && x.t1 >= t0) return true;
  return false;
}

}  // namespace sbm2
