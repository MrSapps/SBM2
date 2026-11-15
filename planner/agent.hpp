// The agent: plays the game from the world view, one pad per frame.
//
//   goal layer (goap.hpp)   what to do next: a task and where to stand
//   actions (here)          candidate moves for that task: walk to a cell;
//                           walk, drop a bomb, walk to cover, wait it out
//   oracle (host)           runs each candidate ahead on a copy of the live
//                           machine and says whether P1 lives and what
//                           changed (enemies, map)
//
// A candidate that lives and does the task is committed: the agent plays the
// exact pads the oracle pressed, frame by frame, and checks P1 stays on the
// oracle's track. The game is deterministic, so a committed move can't kill
// P1. When nothing lives the agent takes the move that lives longest and
// decides again soon.
//
// No stage names anywhere: every rule is a game rule over the world view.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "goap.hpp"
#include "world.hpp"

namespace sbm2 {

struct SimReq {
  std::vector<uint16_t> pre;
  std::vector<int16_t> path;  // x, y world pixels
  std::vector<uint16_t> post;
  std::vector<int16_t> path2;
  bool detEnd = false;
  int frames = 0;
  bool wantEnemies = false;  // fill SimOut::etrace
  std::vector<uint16_t> bossBase;  // boss objects: SimOut::bossHp sums their HP
  bool wantBoss = false;           // fill SimOut::btrace
};

struct SimOut {
  int dead = -1;  // -1 lived, -2 left by the exit, else the frame P1 died
  int enemies = 0, hp = 0;
  int px = 0, py = 0, ph = 0;
  uint64_t mapSig = 0;
  std::vector<uint16_t> pads;   // one per frame
  std::vector<int16_t> trace;   // x, y per frame start
  // wantEnemies: kEnemySlots per frame, each slot's cell (c | r << 8) or -1
  std::vector<int16_t> etrace;
  int bossHp = 0;
  std::vector<int16_t> btrace;  // wantBoss: per frame, per bossBase: world x, y
};
constexpr int kEnemySlots = 12;  // $8400..$8F00 (agent_api.h AG_ESLOTS)

class Oracle {
 public:
  virtual ~Oracle() = default;
  virtual void run(const std::vector<SimReq>& reqs, std::vector<SimOut>& out) = 0;
};

// Pad bits (SNES joypad 1).
enum : uint16_t { kPadA = 0x0080, kPadB = 0x8000, kPadUp = 0x0800, kPadDown = 0x0400, kPadLeft = 0x0200, kPadRight = 0x0100 };

struct Live {  // what the agent reads from WRAM besides the world view
  bool inStage = false, locked = false;
  int px = 0, py = 0;  // P1 world pixels
  int enemies = 0, hp = 0;
  int bossHp = 0;  // the world's boss lines, summed
  uint64_t mapSig = 0;
};
Live readLive(const uint8_t* wram);

class Agent {
 public:
  explicit Agent(const Goal& g) : goal_(g) {}
  // extra: world-view lines the host adds (boss lines), or null.
  uint16_t frame(const uint8_t* wram, int frame, Oracle& o, const char* extra = nullptr);
  // Decide from a world view (tests call this directly with a fake oracle).
  void decide(const World& w, const Live& live, Oracle& o);
  std::string takeLog();
  const std::string& worldText() const { return worldText_; }
  bool committed() const { return at_ < int(pads_.size()); }
  const std::string& what() const { return what_; }
  bool verbose = false;  // log every oracle answer

 private:
  struct Cand {
    SimReq req;
    std::string what;
    int commit = 0;   // frames to play
    int endC = -1, endR = -1;
    enum Want { Live, Exit, Change, Kill, BossHit } want = Live;
  };
  bool tryCands(std::vector<Cand>& cs, const Live& live, Oracle& o, size_t maxRuns);
  void survive(const World& w, const Live& live, Oracle& o, const char* why);
  void learnSpeed(const SimReq& q, const SimOut& r);
  void log(const std::string& s);

  Goal goal_;
  std::vector<uint16_t> bossBase_;  // this decision's boss objects (every run reports their HP)
  std::vector<uint16_t> pads_;
  std::vector<int16_t> trace_;
  int at_ = 0, frame_ = 0, decisions_ = 0, sims_ = 0;
  // Frames per cell P1 walks, measured from the oracle's runs (the bomb
  // press frame over the cells walked to it); P1's speed changes with items.
  double fpc_ = 16;
  // The game lags: on a busy frame its logic doesn't run ($7E0032 counts
  // logic frames). Real frames per logic frame, measured live over the
  // last ~256 frames; the enemy model's clock (fuses too) runs by it.
  int lastTick_ = -1;
  double lagReal_ = 0, lagLogic_ = 0, eScale_ = 1;
  void watchLag(const uint8_t* wram);
  std::string what_, log_, logOut_, worldText_;
};

}  // namespace sbm2
