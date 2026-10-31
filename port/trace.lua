-- Reference trace for port/verify and port/hybrid: runs an input script in
-- Mesen and dumps all 128 KiB of WRAM at the start of every logic frame, i.e.
-- each time the CPU leaves one of the game's NMI wait loops
--   lda $0036 / cmp $0036 / beq -5     (AD 36 00 CD 36 00 F0 FB)
-- at the instruction right after it (CC:0054 for the main loop). The frames
-- come from bot/inputlog.lua: the dump is taken when the NMI that ends the
-- wait writes NmiCount; from there to the loop exit only cmp/beq/rti run, so
-- WRAM is the same.
--
-- Env:
--   SBM2_INPUT   input script, same format as bot/harness.lua
--   SBM2_TRACE   output file: WRAM dumps concatenated; plus <SBM2_TRACE>.txt,
--                one line per logic frame:
--                "<n> <video frame> <exit pc> <master clock> <scanline> <late nmis>"
--                (n = logic frame since power-on; late = NMIs since the
--                previous frame that did not end a wait, i.e. lag)
--   SBM2_FRAMES  logic frames to dump (default 3000)
--   SBM2_LOAD    savestate to start from (needs its bot/inputlog.lua sidecar)
-- The run's pad log goes to <SBM2_TRACE>.inputs (replay: port/hybrid).
-- Usage: port/trace.sh (Mesen needs --timeout for long runs).
local OUT = assert(os.getenv("SBM2_TRACE"), "SBM2_TRACE not set")
local N = tonumber(os.getenv("SBM2_FRAMES") or "3000")

local inPath = assert(os.getenv("SBM2_INPUT"), "SBM2_INPUT not set")
local script = {}
for line in io.lines(inPath) do
  local f, b = line:match("^%s*(%d+)%s+(%S+)")
  if f then script[#script + 1] = { tonumber(f), b } end
end

local DIR = debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or "."
local inputlog = dofile(DIR .. "/../bot/inputlog.lua")

local bin = assert(io.open(OUT, "wb"))
local meta = assert(io.open(OUT .. ".txt", "w"))
local frame, dumped, stopping = 0, 0, false

local chr = {}
for i = 0, 255 do chr[i] = string.char(i) end

-- The callback may run before the NmiCount byte lands: patch it in.
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

local function finish()
  stopping = true
  inputlog.save(OUT .. ".inputs")
  bin:close()
  meta:close()
  emu.stop(0)
end

local LOAD = os.getenv("SBM2_LOAD")
if LOAD == "" then LOAD = nil end
local loaded = LOAD == nil
if loaded then inputlog.start(nil) end

inputlog.onFrame(function(n, value, late, lo)
  if stopping or not loaded then return end
  local s = emu.getState()
  dumpWram(0x36, value)
  meta:write(string.format("%d %d %06X %d %d %d\n", n, frame, lo + 8,
    s["masterClock"], s["ppu.scanline"], late))
  dumped = dumped + 1
  if dumped >= N then finish() end
end)

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
      loaded = true
    end, emu.callbackType.exec, 0, 0xFFFFFF)
    return
  end
  frame = frame + 1
  if held == "end" then finish() end
end, emu.eventType.endFrame)
