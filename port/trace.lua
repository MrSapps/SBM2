-- Reference trace for port/verify: runs from power-on with an input script and
-- dumps all 128 KiB of WRAM at the start of every logic frame, i.e. each time
-- the CPU leaves one of the game's NMI wait loops
--   lda $0036 / cmp $0036 / beq -5     (AD 36 00 CD 36 00 F0 FB)
-- at the instruction right after it (CC:0054 for the main loop). The loops are
-- found by scanning PRG-ROM for that idiom, so nothing here is game data.
--
-- A frame starts when an NMI that interrupted a wait loop (past its lda)
-- writes NmiCount: from there to the loop exit only cmp/beq/rti run, so WRAM
-- is the same.
-- (One write watch instead of an exec watch per loop keeps Mesen fast.)
--
-- Env:
--   SBM2_INPUT   input script, same format as bot/harness.lua
--   SBM2_TRACE   output file: WRAM dumps concatenated; plus <SBM2_TRACE>.txt,
--                one line per logic frame:
--                "<n> <video frame> <exit pc> <master clock> <scanline> <late nmis> <pad reads>"
--                (late = NMIs since the previous frame that did not end a
--                wait, i.e. lag)
--   SBM2_FRAMES  logic frames to dump (default 3000)
--   SBM2_LOAD    savestate to start from (needs its bot/inputlog.lua sidecar);
--                <n> then counts from the state, and <pad reads> (the pad-read
--                log length, from power-on) is what places the frames
-- The run's logic-frame input log is written to <SBM2_TRACE>.inputs.
-- Usage: port/trace.sh (Mesen needs --timeout for long runs).
local OUT = assert(os.getenv("SBM2_TRACE"), "SBM2_TRACE not set")
local N = tonumber(os.getenv("SBM2_FRAMES") or "3000")

local inPath = assert(os.getenv("SBM2_INPUT"), "SBM2_INPUT not set")
local script = {}
for line in io.lines(inPath) do
  local f, b = line:match("^%s*(%d+)%s+(%S+)")
  if f then script[#script + 1] = { tonumber(f), b } end
end

local bin = assert(io.open(OUT, "wb"))
local meta = assert(io.open(OUT .. ".txt", "w"))
local n, frame, late, dumped = 0, 0, 0, 0

local DIR = debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or "."
local inputlog = dofile(DIR .. "/../bot/inputlog.lua")
local LOAD = os.getenv("SBM2_LOAD")
if LOAD == "" then LOAD = nil end
local loaded = LOAD == nil
if loaded then inputlog.start(nil) end

local chr = {}
for i = 0, 255 do chr[i] = string.char(i) end

local function dumpWram(patchAddr, patchValue)
  local rd, wram = emu.read, emu.memType.snesWorkRam
  for base = 0, 0x1FFFF, 0x4000 do
    local t = {}
    for i = 0, 0x3FFF do t[i + 1] = chr[rd(base + i, wram, false)] end
    if patchAddr >= base and patchAddr < base + 0x4000 then
      t[patchAddr - base + 1] = chr[patchValue]
    end
    bin:write(table.concat(t))
  end
end

local stopping = false
local function finish()
  stopping = true
  inputlog.save(OUT .. ".inputs")
  bin:close()
  meta:close()
  emu.stop(0)
end

-- Wait loops: CPU address ranges [lo, lo+8).
local rom = emu.memType.snesPrgRom
local pat = { 0xAD, 0x36, 0x00, 0xCD, 0x36, 0x00, 0xF0, 0xFB }
local loops = {}
for o = 0, emu.getMemorySize(rom) - #pat do
  if emu.read(o, rom, false) == 0xAD then
    local ok = true
    for k = 2, #pat do
      if emu.read(o + k - 1, rom, false) ~= pat[k] then ok = false; break end
    end
    if ok then
      loops[#loops + 1] = 0xC00000 + o
      -- Code in $8000-$FFFF can also run from the bank $00-$3F mirror.
      if o % 0x10000 >= 0x8000 and o < 0x400000 then loops[#loops + 1] = o end
    end
  end
end

-- An NMI ends the wait only if it returns to the cmp or the beq: returning
-- to the lda, the loop reloads the new count and waits for the next NMI.
local function waitLoopAt(pc)
  for _, lo in ipairs(loops) do
    if pc > lo and pc < lo + 8 then return lo end
  end
  return nil
end

-- NMI entry: the interrupted PC is on the stack (native mode: P, PCL, PCH, PB).
local mem = emu.memType.snesMemory
local nmiVec = emu.read(0xFFEA, mem, false) + emu.read(0xFFEB, mem, false) * 0x100
local pending, writes = nil, 0
emu.addMemoryCallback(function()
  writes = 0
  local sp = emu.getState()["cpu.sp"]
  local function rd(k) return emu.read(sp + k, mem, false) end
  pending = waitLoopAt(rd(4) * 0x10000 + rd(3) * 0x100 + rd(2))
  if not pending then late = late + 1 end
end, emu.callbackType.exec, nmiVec, nmiVec)

-- NmiCount write: the 16-bit inc writes both bytes; dump on the second. The
-- callback may run before that byte lands, so the dump patches it in.
emu.addMemoryCallback(function(addr, value)
  if not pending or stopping or not loaded then return end
  writes = writes + 1
  if writes < 2 then return end
  local s = emu.getState()
  dumpWram(addr, value)
  meta:write(string.format("%d %d %06X %d %d %d %d\n", n, frame, pending + 8,
    s["masterClock"], s["ppu.scanline"], late, inputlog.mark()))
  n, late, pending, dumped = n + 1, 0, nil, dumped + 1
  if dumped >= N then finish() end
end, emu.callbackType.write, 0x0036, 0x0037)

local idx, held = 1, "-"
emu.addEventCallback(function()
  while script[idx] and script[idx][1] <= frame do
    held = script[idx][2]
    idx = idx + 1
  end
  local input = {}
  for b in held:gmatch("[^+]+") do input[b] = true end
  emu.setInput(input, 0)
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  if stopping then return end
  if not loaded then
    local fh = assert(io.open(LOAD, "rb"))
    local data = fh:read("a")
    fh:close()
    local id
    id = emu.addMemoryCallback(function()
      emu.removeMemoryCallback(id, emu.callbackType.exec, 0, 0xFFFFFF)
      emu.loadSavestate(data)
      inputlog.start(LOAD)
      pending = nil
      loaded = true
    end, emu.callbackType.exec, 0, 0xFFFFFF)
    return
  end
  frame = frame + 1
  if held == "end" then finish() end
end, emu.eventType.endFrame)
