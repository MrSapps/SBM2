#include "world.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace sbm2 {

const Bomb* World::bombAt(int c, int r) const {
  for (const Bomb& b : bombs)
    if (b.c == c && b.r == r) return &b;
  return nullptr;
}

namespace {

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string t;
  while (in >> t) out.push_back(t);
  return out;
}

// key=value fields of an object line (from word 1 on).
std::map<std::string, std::string> fields(const std::vector<std::string>& words) {
  std::map<std::string, std::string> f;
  for (size_t i = 1; i < words.size(); i++) {
    auto eq = words[i].find('=');
    if (eq != std::string::npos) f[words[i].substr(0, eq)] = words[i].substr(eq + 1);
  }
  return f;
}

int dec(const std::map<std::string, std::string>& f, const char* k, int def = 0) {
  auto it = f.find(k);
  return it == f.end() ? def : std::atoi(it->second.c_str());
}
int hex(const std::map<std::string, std::string>& f, const char* k, int def = 0) {
  auto it = f.find(k);
  return it == f.end() ? def : int(std::strtol(it->second.c_str(), nullptr, 16));
}
double real(const std::map<std::string, std::string>& f, const char* k) {
  auto it = f.find(k);
  return it == f.end() ? 0.0 : std::atof(it->second.c_str());
}
char ch(const std::map<std::string, std::string>& f, const char* k, char def) {
  auto it = f.find(k);
  return it == f.end() || it->second.empty() ? def : it->second[0];
}

}  // namespace

bool parseWorld(const std::string& text, World& w, std::string* err) {
  w = World{};
  std::istringstream in(text);
  std::string line;
  int lineNo = 0;
  auto fail = [&](const std::string& why) {
    if (err) *err = "line " + std::to_string(lineNo) + ": " + why;
    return false;
  };
  std::string layer;  // "map" / "under" / "raw" while reading grid rows
  int layerRow = 0;
  while (std::getline(in, line)) {
    lineNo++;
    if (!layer.empty() && layerRow < w.rows) {
      if (layer == "raw" || layer == "rawunder") {
        auto words = split(line);
        if (int(words.size()) != w.cols) return fail(layer + " row has " + std::to_string(words.size()) + " cells");
        auto& dst = layer == "raw" ? w.raw : w.rawUnder;
        for (auto& s : words) dst.push_back(uint8_t(std::strtol(s.c_str(), nullptr, 16)));
      } else {
        if (int(line.size()) < w.cols) return fail(layer + " row too short");
        auto& dst = layer == "map" ? w.map : w.under;
        dst.insert(dst.end(), line.begin(), line.begin() + w.cols);
      }
      if (++layerRow == w.rows) layer.clear();
      continue;
    }
    auto hash = line.find('#');
    if (hash != std::string::npos && (hash == 0 || line[hash - 1] == ' ')) line.resize(hash);
    auto words = split(line);
    if (words.empty()) continue;
    const std::string& k = words[0];
    auto f = fields(words);
    auto arg = [&](size_t i) { return i < words.size() ? words[i] : std::string(); };
    if (k == "worldview") w.version = std::atoi(arg(1).c_str());
    else if (k == "stage") w.stage = int(std::strtol(arg(1).c_str(), nullptr, 16));
    else if (k == "frame") w.frame = std::atoi(arg(1).c_str());
    else if (k == "time") w.time = std::atoi(arg(1).c_str());
    else if (k == "origin") { w.ox = std::atoi(arg(1).c_str()); w.oy = std::atoi(arg(2).c_str()); }
    else if (k == "size") { w.cols = std::atoi(arg(1).c_str()); w.rows = std::atoi(arg(2).c_str()); }
    else if (k == "light") w.light = arg(1);
    else if (k == "intro") w.intro = arg(1) == "1";
    else if (k == "gate") {
      w.hasGate = true;
      w.gateC = std::atoi(arg(1).c_str());
      w.gateR = std::atoi(arg(2).c_str());
      w.gateOpen = arg(3) == "open";
    } else if (k == "player") {
      Player& p = w.p;
      p.x = dec(f, "x"); p.y = dec(f, "y"); p.dir = ch(f, "dir", '-');
      p.hearts = dec(f, "hearts"); p.flash = dec(f, "flash"); p.bombs = dec(f, "bombs"); p.maxbombs = dec(f, "maxbombs");
      p.e0 = dec(f, "e0"); p.fire = dec(f, "fire", 1);
      p.pierce = dec(f, "pierce"); p.remote = dec(f, "remote"); p.ride = dec(f, "ride");
      p.glove = dec(f, "glove"); p.softpass = dec(f, "softpass");
      p.lock = uint16_t(hex(f, "lock")); p.f4 = uint16_t(hex(f, "f4")); p.f60 = uint16_t(hex(f, "f60"));
    } else if (k == "bomb") {
      Bomb b;
      b.c = dec(f, "c"); b.r = dec(f, "r"); b.fuse = dec(f, "fuse"); b.range = dec(f, "range", 1);
      b.slot = dec(f, "slot"); b.enemyOwned = f["owner"] == "enemy";
      b.remote = dec(f, "remote"); b.pierce = dec(f, "pierce"); b.magnet = dec(f, "magnet");
      b.flags = hex(f, "flags");
      w.bombs.push_back(b);
    } else if (k == "enemy") {
      Enemy e;
      e.slot = dec(f, "slot"); e.type = hex(f, "type"); e.x = dec(f, "x"); e.y = dec(f, "y");
      e.vx = real(f, "vx"); e.vy = real(f, "vy"); e.step = ch(f, "step", '-');
      auto t = f["target"];
      auto comma = t.find(',');
      if (comma != std::string::npos) {
        e.tc = std::atoi(t.substr(0, comma).c_str());
        e.tr = std::atoi(t.substr(comma + 1).c_str());
      }
      e.wait = dec(f, "wait"); e.hp = dec(f, "hp");
      e.f4 = uint16_t(hex(f, "f4")); e.c0 = uint16_t(hex(f, "c0")); e.d0 = uint16_t(hex(f, "d0"));
      e.be = uint16_t(hex(f, "be")); e.bc = uint16_t(hex(f, "bc")); e.ba = uint16_t(hex(f, "ba"));
      e.offscreen = dec(f, "off");
      e.spd = real(f, "spd");
      w.enemies.push_back(e);
    } else if (k == "panel") {
      w.panels.push_back({std::atoi(arg(1).c_str()), std::atoi(arg(2).c_str()), dec(f, "mode"), arg(3) == "done"});
    } else if (k == "switch") {
      w.lightSwitch = std::make_pair(std::atoi(arg(1).c_str()), std::atoi(arg(2).c_str()));
    } else if (k == "item") {
      w.items.push_back({std::atoi(arg(1).c_str()), std::atoi(arg(2).c_str()), arg(3) == "heart", dec(f, "kind", -1)});
    } else if (k == "hidden") {
      int kind = dec(f, "kind", -1);
      w.hidden.push_back({std::atoi(arg(1).c_str()), std::atoi(arg(2).c_str()), kind == kItemHeart, kind});
    } else if (k == "magnet") {
      w.magnets.push_back({std::atoi(arg(1).c_str()), std::atoi(arg(2).c_str()), ch(f, "dir", 'u')});
    } else if (k == "boss") {
      BossObj b;
      b.base = hex(f, "base");
      b.state = uint32_t(hex(f, "state"));
      b.x = dec(f, "x"); b.y = dec(f, "y"); b.hp = dec(f, "hp"); b.hurt = dec(f, "hurt");
      auto box = f["box"];
      if (!box.empty()) {
        b.hasBox = std::sscanf(box.c_str(), "%d,%d,%d,%d", &b.box[0], &b.box[1], &b.box[2], &b.box[3]) == 4;
      }
      // sense=dx,dy;dx,dy...
      std::string sp = f["sense"];
      for (size_t i = 0; i < sp.size();) {
        size_t j = sp.find(';', i);
        if (j == std::string::npos) j = sp.size();
        int dx, dy;
        if (std::sscanf(sp.substr(i, j - i).c_str(), "%d,%d", &dx, &dy) == 2) b.sense.push_back({dx, dy});
        i = j + 1;
      }
      w.bosses.push_back(b);
    } else if (k == "map" || k == "under" || k == "raw" || k == "rawunder") {
      if (w.cols <= 0 || w.rows <= 0) return fail(k + " before size");
      layer = k;
      layerRow = 0;
    }
    // unknown keys: ignored (the format grows compatibly)
  }
  if (!layer.empty()) return fail(layer + " layer ends early");
  if (w.version != 1) return fail("not a worldview 1 snapshot");
  if (int(w.map.size()) != w.cols * w.rows) return fail("no map layer");
  return true;
}

bool loadWorld(const std::string& path, World& w, std::string* err) {
  std::ifstream f(path);
  if (!f) {
    if (err) *err = "can't read " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return parseWorld(ss.str(), w, err);
}

}  // namespace sbm2
