-- Offline regression tests for the bot's world models: one test per game
-- rule a no-rewind death traced back to (docs/mechanics.md, docs/enemies.md).
-- Each sets up an invented map (inline, bot/test/maps.lua format) and pokes
-- the WRAM fields the rule reads; no emulator, no ROM-derived data.
--   build/tools/lua bot/test/test_models.lua        (make bot-test)
local ROOT = arg[0]:match("^(.*)/bot/test/[^/]*$") or "."
local E = dofile(ROOT .. "/bot/test/emu.lua")
local Maps = dofile(ROOT .. "/bot/test/maps.lua")
local fails, passes = 0, 0
local OUT = os.tmpname(); os.remove(OUT)
local KEY_W = 32

local function test(name, fn)
  local ok, err = pcall(fn)
  if ok then passes = passes + 1; print("ok   " .. name)
  else fails = fails + 1; print("FAIL " .. name .. ": " .. tostring(err)) end
end
local function check(cond, what) if not cond then error(what, 2) end end

-- An open 15x7 room (walls round it), P1 at (2,3); `rows` replace rows.
local function room(stage, rows, extra)
  local lines = { "stage " .. stage, "p1 2 3", "fire 3", "bombs 1", "panels none", extra or "", "grid" }
  for r = 0, 6 do
    local t = {}
    for c = 0, 14 do t[#t + 1] = (r == 0 or r == 6 or c == 0 or c == 14) and "C6" or "." end
    if rows and rows[r] then t = {} for w in rows[r]:gmatch("%S+") do t[#t + 1] = w end end
    lines[#lines + 1] = table.concat(t, " ")
  end
  return table.concat(lines, "\n")
end

local function setup(text)
  local spec = Maps.parse(text)
  E.install { wram = Maps.toWram(spec), rom = ROOT .. "/baserom.sfc", env = { SBM2_OUT = OUT, SBM2_NOASSIST = "1" } }
  return spec
end
local function loadBot()
  local bot = E.loadBot(ROOT .. "/bot/bot.lua")
  KEY_W = bot.KEY_W  -- (the bot's cell keys; boss/enemy modules get 32 below)
  return bot, bot.readMap()
end
local function cellSet(cells)
  local s = {}
  for _, c in ipairs(cells) do s[c[2] * KEY_W + c[1]] = true end
  return s
end
local function has(set, c, r) return set[r * KEY_W + c] == true end

-- A bomb in table slot i: cell, fuse, flags (+1), range (+7 = range - 1), +0.
local function bomb(i, c, r, fuse, flags, range, b0)
  local base = 0x12000 + i * 0x100
  E.poke(base, b0 or 0)
  E.poke(base + 1, flags or 0x80)
  E.poke16(base + 2, r * 0x40 + c * 2)  -- (bomb table cells: $40 a row)
  E.poke(base + 4, fuse or 72)
  E.poke(base + 7, (range or 2) - 1)
end
-- A boss-engine object (24-bit state at +0, screen x +$11, y +$14).
local function bossObj(base, state, x, y)
  E.poke16(base, state & 0xFFFF); E.poke(base + 2, state >> 16)
  E.poke16(base + 0x11, x); E.poke16(base + 0x14, y)
end
local function firstWindow(w, c, r)
  local f
  for _, iv in ipairs(w[r * KEY_W + c] or {}) do if not f or iv[1] < f then f = iv[1] end end
  return f
end

-- Blast model ----------------------------------------------------------------

test("pierce bombs go through soft blocks (2-2: BlastResolveLeft C0:ED42)", function()
  setup(room(21, { [3] = "C6 . . . C2 . . . . . . . . . C6" }))
  local bot, g = loadBot()
  local plain = cellSet(bot.blastCells(g, 2, 3, 4, false))
  local pierce = cellSet(bot.blastCells(g, 2, 3, 4, true))
  check(has(plain, 4, 3) and not has(plain, 5, 3), "plain blast stops at the soft block")
  check(has(pierce, 5, 3) and has(pierce, 6, 3), "pierce blast goes on past it to its range")
end)

test("barrel bursts are range-2 pierce blasts (1-5: C1:275F)", function()
  -- barrel D6 at (6,3); a bomb at (4,3) reaches it; the burst 2 cells out
  -- at (8,3) reaches (10,3) and passes the soft block at (9,3)
  setup(room(14, { [3] = "C6 . . . . . D6 . . C2 . . . . C6" }))
  local bot, g = loadBot()
  local cells = cellSet(bot.blastCells(g, 4, 3, 2, false))
  check(has(cells, 6, 3), "the barrel is hit")
  check(has(cells, 10, 3), "the burst at (8,3) reaches 2 cells, through the soft block")
  check(has(cells, 6, 1), "the burst at (6,1) reaches up")
end)

-- Bomb table ---------------------------------------------------------------

test("enemy bombs (flags bit 3) are due any moment (4-7's $A8)", function()
  setup(room(46))
  bomb(0, 8, 3, 72, 0x88, 2)
  local bot, g = loadBot()
  local w = bot.blastWindows(g)
  local f = firstWindow(w, 8, 3)
  check(f and f <= 8, "window at the enemy bomb starts within 8 frames, got " .. tostring(f))
end)

test("a magnet pulls a ticking bomb on its line to the cell next to it (1-3)", function()
  -- magnet $E1 (facing left) at (10,3); a bomb at (7,3) is 3 cells down its line
  setup(room(12, { [3] = "C6 . . . . . . . . . E1 . . . C6" }))
  E.poke16(0x5828 + 2, 3 * 0x80 + 10 * 2); E.poke16(0x5828 + 8, 6)
  bomb(0, 7, 3, 72, 0x80, 2)
  local bot, g = loadBot()
  local w = bot.blastWindows(g)
  check(firstWindow(w, 9, 3) ~= nil, "the bomb is modelled at (9,3), next to the magnet")
  check(firstWindow(w, 5, 3) == nil, "not at its old cell's reach")
  check(MagLine and MagLine[3 * KEY_W + 8], "the magnet's line is recorded (no place to stop)")
end)

test("2-8 drops: a marker is a range-10 bomb due when the faller lands", function()
  setup(room(27))
  bossObj(0x1040, 0xCCB2F1, 128, 40)  -- marker at screen (128,40): cell (8,2)
  bossObj(0x1080, 0xCCB189, 128, 0)   -- faller 40 px above: 17 frames
  local bot, g = loadBot()
  local w = bot.blastWindows(g)
  local f = firstWindow(w, 8, 2)
  check(f and f >= 8 and f <= 20, "due in ~17 frames at the marker, got " .. tostring(f))
  check(firstWindow(w, 13, 2) ~= nil, "range 10 along the row")
end)

-- Boss windows (bot/boss.lua) -------------------------------------------------

local function bossModule(stage)
  KEY_W = 32
  setup(room(stage))
  local env = {
    rd8 = function(a) return E.peek(a) end,
    r16 = function(a) return E.peek(a) | (E.peek(a + 1) << 8) end,
    origin = function() return 0, 0 end, KEY_W = KEY_W, MAP_W = 32, MAP_H = 32,
  }
  emu = emu or {}
  return dofile(ROOT .. "/bot/boss.lua")(env)
end

test("boss windows: a later pass's early window is not merged away", function()
  -- object A moves right into cell (8,4) late; object B sits on it now
  local B = bossModule(0x26)
  local a = { base = 0x1040, state = 0, x = 40, y = 72, vx = 0.5, vy = 0, box = { -8, 8, -8, 8 } }
  local b = { base = 0x1080, state = 0, x = 136, y = 72, vx = 0, vy = 0, box = { -8, 8, -8, 8 } }
  local w = B.windows({ a, b }, 300, 0, 0)
  local f = firstWindow(w, 8, 4)
  check(f and f <= 4, "the cell is dangerous from now, got " .. tostring(f))
end)

test("1-8 hands: both columns below the body are danger, the middle is not", function()
  local B = bossModule(0x17)
  E.poke(0x1A, 0x17)
  local body = { base = 0x1000, state = 0xCC9BDD, x = 120, y = 64, vx = 0, vy = 0, box = { -32, 32, -32, 56 } }
  local w = B.windows({ body }, 60, 0, 0)
  -- hand columns x 120+-12..44: cells 5-6 and 8-9 (centres 88..104, 136..152); rows below +56
  check(firstWindow(w, 9, 10) ~= nil, "right hand column, 104 px below")
  check(firstWindow(w, 5, 10) ~= nil, "left hand column, 104 px below")
  check(firstWindow(w, 7, 10) == nil, "the gap under the body stays clear")
end)

-- Enemy model (bot/enemies.lua) ---------------------------------------------

local function enemyModule(text)
  KEY_W = 32
  setup(text)
  local env = {
    rd8 = function(a) return E.peek(a) end,
    r16 = function(a) return E.peek(a) | (E.peek(a + 1) << 8) end,
    MAP_W = 32, MAP_H = 32, KEY_W = KEY_W,
    tileCentre = function(c, r) return c * 16 + 8, r * 16 + 8 end,
  }
  emu = emu or {}
  return dofile(ROOT .. "/bot/enemies.lua")(env)
end
-- An enemy at slot base: type, cell, step (+$4A), moving, speed (+$46.$48).
local function enemy(base, typ, c, r, step, moving, speed)
  E.poke16(base, 0x9000)
  E.poke16(base + 0x7E, typ)
  E.poke16(base + 0xA8, r * 0x80 + c * 2)
  E.poke16(base + 0x4C, r * 0x80 + c * 2)
  E.poke16(base + 0x10, c * 16 + 8); E.poke16(base + 0x18, r * 16 + 8)
  E.poke16(base + 0x4A, step or 0)
  E.poke16(base + 0xC0, moving and 0x8000 or 0)
  local s = speed or 0.5
  E.poke16(base + 0x46, math.floor(s)); E.poke16(base + 0x48, math.floor((s % 1) * 0x10000))
  E.poke(base + 0x43, 1)
end
local function readGrid(text)
  local spec = Maps.parse(text)
  local g = {}
  for r = 0, #spec.grid do g[r] = {} for c = 0, 31 do g[r][c] = spec.grid[r][c] or 0xC6 end end
  return g
end

test("$B2 shots fly straight at 2 px/f and end at a wall (3-3)", function()
  local text = room(32)
  local En = enemyModule(text)
  enemy(0x8400, 0xB2, 4, 1, 0x0080, false, 0)  -- heading down, speed fields 0
  local g = readGrid(text)
  local list = En.read(g)
  check(#list == 1 and list[1].shot, "read as a shot")
  check(math.abs(list[1].cpf - 8) < 0.01, "16 px at 2 px/f: 8 frames a cell, got " .. list[1].cpf)
  local s = En.rollouts(g, list, 2, 3, 4, 200)
  local last = s[1][1][#s[1][1]]
  check(last[1] == 5 * KEY_W + 4, "ends at (4,5), the last cell before the wall")
  check(last[3] < 100, "and is gone after that, not parked for ever")
end)

test("$91: the cells next to it are deadly too (5-3)", function()
  local text = room(52)
  local En = enemyModule(text)
  enemy(0x8400, 0x91, 8, 3, 0, false, 0.5)
  local g = readGrid(text)
  local list = En.read(g)
  local w = En.windows(En.rollouts(g, list, 2, 3, 4, 60), list, 60, 0)
  check(w[3 * KEY_W + 9] and w[2 * KEY_W + 8], "neighbours (9,3) and (8,2) have windows")
end)

test("$86 dashes at 2.5 px/f, at most 4 cells (2-2)", function()
  local text = room(21)
  local En = enemyModule(text)
  enemy(0x8400, 0x86, 10, 3, 0, false, 0)
  E.poke16(0x8400 + 0x4E, 0)  -- rest over
  local g = readGrid(text)
  local list = En.read(g)
  local s = En.rollouts(g, list, 2, 3, 8, 200)
  -- some sample reaches 4 cells away within ~4 * 6.4 frames
  local fast = false
  for _, path in ipairs(s[1]) do
    for _, iv in ipairs(path) do
      local c, r = iv[1] % KEY_W, iv[1] // KEY_W
      if (math.abs(c - 10) + math.abs(r - 3)) == 4 and iv[2] <= 30 then fast = true end
    end
  end
  check(fast, "a dash covers 4 cells in under 30 frames")
end)

test("flashing: a $93 stays harmful (4-6), a hit $90 does not (3-3)", function()
  local text = room(45)
  local En = enemyModule(text)
  enemy(0x8400, 0x93, 8, 3, 0, false, 0.5); E.poke16(0x8400 + 4, 0x0020)
  enemy(0x8500, 0x90, 4, 5, 0, false, 0.5); E.poke16(0x8500 + 4, 0x0020)
  local list = En.read(readGrid(text))
  local by = {}
  for _, e in ipairs(list) do by[e.type] = e end
  check(by[0x93] and by[0x93].harmful, "flashing $93 harmful for planning")
  check(by[0x90] and not by[0x90].harmful, "flashing $90 not")
end)

print(string.format("%d passed, %d failed", passes, fails))
os.exit(fails == 0 and 0 or 1)
