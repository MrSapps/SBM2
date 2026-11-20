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
#include <array>
#include <map>
#include <set>
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
  int detEnd = 0;  // B presses once path2 is done (each sets off the oldest remote bomb)
  int frames = 0;
  bool wantEnemies = false;  // fill SimOut::etrace
  std::vector<uint16_t> bossBase;  // boss objects: SimOut::bossHp sums their HP
  bool wantBoss = false;           // fill SimOut::btrace
};

struct SimOut {
  int dead = -1;  // -1 lived, -2 left by the exit, else the frame P1 died
  int enemies = 0, hp = 0;
  int enemyMask = 0;  // agent_enemy_mask at the end
  int px = 0, py = 0, ph = 0;
  uint64_t mapSig = 0;
  bool mapChanged = false;      // the map differed from the start at some point (blocks grow back)
  std::vector<uint16_t> pads;   // one per frame
  std::vector<int16_t> trace;   // x, y per frame start
  // wantEnemies: kEnemySlots per frame, each slot's cell (c | r << 8) or -1
  std::vector<int16_t> etrace;
  int bossHp = 0;
  std::vector<int16_t> btrace;  // wantBoss: per frame, per bossBase: world x, y
  std::vector<int16_t> blasts;  // where P1's bombs went off in the run (c | r << 8), in order
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
  int enemyMask = 0;  // agent_enemy_mask
  int bossHp = 0;  // the world's boss lines, summed
  uint64_t mapSig = 0;
};
Live readLive(const uint8_t* wram);

// Remote traps. order: P1's remote bomb cells (r * cols + c), oldest first
// (B sets off the oldest, docs/mechanics.md 10). True when one press now
// catches an enemy in that bomb's blast (with the chain it sets off), P1 is
// out of it, and it reaches neither the gate (enemies come back) nor a
// wanted item (burnt). blast: those cells, if wanted.
// needEnemy false: only P1, the gate and items out of it (a stale trap).
bool remoteTrigger(const World& w, const std::vector<int>& order, std::vector<uint8_t>* blast = nullptr,
                   bool needEnemy = true);
// Traps out and no enemy for this long (frames): they go off once P1 is clear.
constexpr int kTrapWait = 600;
// How long a soft block no bomb cell could reach stays routed around.
constexpr int kBadSoftTTL = 300;
// Keep order in step with the world: gone bombs out, new ones at the end.
void noteRemotes(const World& w, std::vector<int>& order);
// A ride probe's run (boarding the object on (c,r)): the frame the ride let
// P1 off (on the cell, then carried more than a cell away and still for 20
// frames, alive by then), else -1.
struct SimOut;
int rideEnd(const SimOut& r, int c, int rr);

class Agent {
 public:
  explicit Agent(const Goal& g) : goal_(g) {}
  // extra: world-view lines the host adds (boss lines), or null.
  uint16_t frame(const uint8_t* wram, int frame, Oracle& o, const char* extra = nullptr);
  // Decide from a world view (tests call this directly with a fake oracle).
  void decide(const World& w, const Live& live, Oracle& o);
  // Fill in a boss's sense points from memory while it senses nothing
  // (frame() calls it before decide; senseStale() then says so).
  void noteBosses(World& w);
  bool senseStale() const { return senseStale_; }
  std::string takeLog();
  const std::string& worldText() const { return worldText_; }
  bool committed() const { return at_ < int(pads_.size()); }
  int pending() const { return int(pads_.size()) - at_; }  // frames of the move left to play
  const std::string& what() const { return what_; }
  bool verbose = false;  // log every oracle answer
  int killSlot() const { return killSlot_; }
  // Set off the oldest remote bomb now? 1: an enemy is in its blast; 2: it
  // is stale (kTrapWait) or in the way (decide found only survive), P1,
  // the gate and items clear. 0: no. (frame() each
  // frame while P1 holds the remote.)
  int trapCheck(const World& tw);
  // A press in a committed move (A places, B sets off the oldest remote)
  // whose blast would now burn a wanted item P1 can walk to: frame() stops
  // the move (5-7, seed 2: the glove came out of its block 5 frames into a
  // move to bomb that block open; the move's B burnt it, and 5-8's form 2
  // is hurt only by thrown bombs). A remote bomb placed is no blast yet.
  bool moveBurns(const World& cw, uint16_t pad);
  bool trapStuck() const { return trapStuck_; }
  // A trap's B in the middle of a move: the move's run had no B there, and
  // P1's presses stir the enemies' RNG (world 3, 3-7: B on the (4,11) trap
  // mid-move, then the $B0 came on where the run had it turn; P1 died). The
  // oracle plays B, then the rest of the move: true (P1 plays that run on)
  // if P1 lives, else false (the trap waits).
  bool trapMidMove(Oracle& o);
  const std::vector<uint16_t>& pads() const { return pads_; }
  const std::vector<int16_t>& trace() const { return trace_; }
  int at() const { return at_; }

 private:
  struct Cand {
    SimReq req;
    std::string what;
    int commit = 0;   // frames to play
    int endC = -1, endR = -1;
    enum Want { Live, Exit, Change, Kill, BossHit } want = Live;
    int drop = 1;     // BossHit: boss HP lost in the run (its bombs already out count too)
    // Play only this many frames past the run's first A press (the bomb's
    // own fuse and flames), or past its last B press (a remote set off): the
    // walk budget is pessimistic, and world 2's blocks grow back ~190 frames
    // after they break. 0: the whole commit.
    int afterA = 0, afterB = 0;
    bool holdB = false;
    int landC = -1, landR = -1;  // a kick or throw: the run must show a blast of P1's there (else it landed elsewhere)  // end before the run's first B (a remote bomb left out for the next move)
  };
  // waits: if every option died, try the best two again after 32/64/128
  // frames standing still.
  bool tryCands(std::vector<Cand>& cs, const Live& live, Oracle& o, size_t maxRuns, bool waits = true);
  void survive(const World& w, const Live& live, Oracle& o, const char* why);
  // While decide() tries a later plan step, a step whose options all die
  // gives up (deferWhy_ set) instead of surviving.
  bool deferSurvive_ = false;
  std::string deferWhy_;
  void learnSpeed(const SimReq& q, const SimOut& r);
  void log(const std::string& s);

  Goal goal_;
  std::map<int, std::vector<std::pair<int, int>>> senseMem_;  // boss base -> its last sense points
  // Bomb cells whose runs never changed the map (the game refused the bomb:
  // ObjectOnPlayerCell C0:31A0 blocks a cell some object, even a dormant
  // enemy, has as its cell or target), and soft blocks no usable cell
  // reaches. Per stage; the marks wear off (kBadSoftTTL: what kept a cell
  // out may pass: a blast, a bomb in the way, an enemy on it or nearby).
  int stage_ = -1;
  std::map<int, int> bombFails_, bombFailAt_;  // cell -> fails, frame of the last
  std::map<int, int> throwBad_;
  int trapFor_ = -1;                // the enemy slot the last trap was laid for
  std::map<int, std::pair<int, int>> trapStale_;  // enemy slot -> (traps for it that went off stale, frame of the last)  // (thrower cell * 4 + way) -> frame its planned throw last had no move that worked
  bool bombDropped(int cell) const;  // 3 fails, the last within kBadSoftTTL * 2
  std::map<int, int> badSoft_;  // soft block -> frame marked
  int dismountAt_ = -100000;  // the last "get off the mount" try (frame)
  std::set<int> badSoftNow() const;
  // Power-ups (optional) whose moves failed: given up after 3 (cell -> fails).
  std::map<int, int> itemFails_;
  std::set<int> softSeen_;
  // Rides learnt this stage (World::rides), probes that went nowhere (cell
  // -> count), and where the last committed run ended (world pixels).
  std::vector<Ride> rides_;
  std::map<int, int> rideFails_;
  int dockFor_ = -1;  // the ride just probed (its dock is seen next decision)
  // Dormant enemies where first seen dormant this stage (slot -> type, x, y).
  std::map<int, std::array<int, 3>> dormantAt_;
  int lastPx_ = 0, lastPy_ = 0;  // cells that held a soft block this stage (gaps now, if open)
  bool senseStale_ = false;  // this frame's boss senses nothing (points remembered)
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
  // Remote traps: P1's remote bombs oldest first, B frames left to press,
  // frames before the next look, and when the last trap went down or an
  // enemy last died (traps out: wait this long for them to work).
  bool holdsRemote_ = false;
  std::vector<int> remoteOrder_;
  int trapB_ = 0, trapCool_ = 0, trapSince_ = -100000, lastEnemies_ = -1;
  int stayCell_ = -1, staySince_ = 0, lastSurvive_ = -100000;  // survive: the cell P1 keeps to, since
  int lastBossHp_ = -1;  // (logged when it changes)
  int tail_ = 60;  // frames past a move's end its run must live (kTail; kBossTail with a boss)
  bool trapStuck_ = false;
  int killSlot_ = -1;  // the kill task's enemy slot (0 = $8400), else -1  // the last decision was a survive with traps out
};

}  // namespace sbm2
