-- A stand-in for Mesen's `emu` API, so bot/bot.lua runs outside Mesen on a
-- WRAM image (port/hybrid -D N:FILE writes them). Memory is static: the bot
-- thinks about the same frame again and again unless a test changes WRAM.
--
--   local E = dofile("bot/test/emu.lua")
--   E.install{ wram = path, rom = "baserom.sfc", env = { SBM2_OUT = dir } }
--   local bot = E.loadBot("bot/bot.lua")  -- bot.lua's test exports (BOT_TEST)
--   E.frame()                             -- one endFrame callback
--   E.input()                             -- the last pad the bot set
local E = {}

local wram, rom = {}, ""
local events = {}         -- event type -> list of callbacks
local lastInput = {}
local env = {}

local MT = { snesWorkRam = 1, snesMemory = 2, snesPrgRom = 3 }

-- WRAM image as a byte string -> table indexed from 0 (string.byte per read
-- is slower than a table for the bot's ~10^5 reads a frame).
local function loadWram(path)
  local fh = assert(io.open(path, "rb"), "can't read " .. path)
  local data = fh:read("a"); fh:close()
  assert(#data == 0x20000, path .. ": not a 128 KiB WRAM image")
  local t = {}
  for i = 1, #data, 4096 do
    local chunk = { data:byte(i, i + 4095) }
    for j = 1, #chunk do t[i + j - 2] = chunk[j] end
  end
  return t
end

-- 24-bit bus read (HiROM): banks $7E/$7F WRAM, $00-$3F/$80-$BF low WRAM
-- mirror and ROM above $8000, $C0-$FF and $40-$7D ROM. I/O reads 0.
local function busRead(a)
  local bank, off = (a >> 16) & 0xFF, a & 0xFFFF
  if bank == 0x7E or bank == 0x7F then return wram[a - 0x7E0000] or 0 end
  local b = bank & 0x7F
  if b < 0x40 then
    if off < 0x2000 then return wram[off] or 0 end
    if off < 0x8000 then return 0 end
    return rom:byte((((b & 0x3F) << 16) | off) + 1) or 0
  end
  return rom:byte((((bank & 0x3F) << 16) | off) + 1) or 0
end

local function read8(a, t)
  if t == MT.snesWorkRam then return wram[a & 0x1FFFF] or 0 end
  if t == MT.snesPrgRom then return rom:byte(a + 1) or 0 end
  return busRead(a)
end

local api = {
  memType = MT,
  callbackType = { read = 0, write = 1, exec = 2 },
  eventType = { endFrame = 1, inputPolled = 2, startFrame = 3 },
  read = function(a, t, signed)
    local v = read8(a, t)
    if signed and v >= 0x80 then v = v - 0x100 end
    return v
  end,
  read16 = function(a, t, signed)
    local v = read8(a, t) | (read8(a + 1, t) << 8)
    if signed and v >= 0x8000 then v = v - 0x10000 end
    return v
  end,
  write = function(a, v, t) if t == MT.snesWorkRam then wram[a & 0x1FFFF] = v & 0xFF end end,
  write16 = function(a, v, t)
    if t == MT.snesWorkRam then wram[a & 0x1FFFF] = v & 0xFF; wram[(a + 1) & 0x1FFFF] = (v >> 8) & 0xFF end
  end,
  getMemorySize = function(t) return t == MT.snesPrgRom and #rom or 0x20000 end,
  addEventCallback = function(fn, ev)
    events[ev] = events[ev] or {}
    table.insert(events[ev], fn)
    return #events[ev]
  end,
  -- Memory callbacks never fire: no CPU runs. That covers onNextInstruction
  -- (savestates, checkpoints), the pad log and boss sense-point learning.
  addMemoryCallback = function() return 0 end,
  removeMemoryCallback = function() end,
  getState = function() return {} end,
  setInput = function(inp) lastInput = inp end,
  createSavestate = function() return "" end,
  loadSavestate = function() end,
  takeScreenshot = function() return "" end,
  getCdlData = function() return {} end,
  stop = function(code) E.stopped = code or 0 end,
  displayMessage = function() end,
  log = function(s) print(s) end,
}

-- opts: wram (path, or a table from E.readWram), rom (path), env (overrides
-- for os.getenv: the bot reads its settings there).
function E.install(opts)
  if opts.rom then
    local fh = assert(io.open(opts.rom, "rb")); rom = fh:read("a"); fh:close()
  end
  if opts.wram then E.setWram(opts.wram) end
  env = opts.env or {}
  local realGetenv = E.realGetenv or os.getenv
  E.realGetenv = realGetenv
  os.getenv = function(k)
    if env[k] ~= nil then return env[k] or nil end
    -- Bot settings come only from the test, not the shell running it.
    if k:match("^SBM2_") then return nil end
    return realGetenv(k)
  end
  events, lastInput, E.stopped = {}, {}, nil
  emu = api
end

function E.readWram(path) return loadWram(path) end
function E.setWram(w)
  if type(w) == "string" then w = loadWram(w) end
  -- A copy: tests poke the map, and the cached image must stay clean.
  wram = {}
  for i = 0, 0x1FFFF do wram[i] = w[i] end
end
function E.peek(a) return wram[a & 0x1FFFF] or 0 end
function E.poke(a, v) wram[a & 0x1FFFF] = v & 0xFF end
function E.poke16(a, v) E.poke(a, v); E.poke(a + 1, v >> 8) end

-- Loads bot.lua with BOT_TEST set; returns its exports.
function E.loadBot(path)
  BOT_TEST = {}
  dofile(path)
  return BOT_TEST.bot
end

function E.frame()
  for _, fn in ipairs(events[api.eventType.inputPolled] or {}) do fn() end
  for _, fn in ipairs(events[api.eventType.endFrame] or {}) do fn() end
end
function E.input() return lastInput end

return E
