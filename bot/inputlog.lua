-- Pad-read log, for replaying Mesen runs in the port's frame-step verifier
-- (port/hybrid.c <log>.inputs): from power-on, in order, the value of every
-- pad read the game made and every NMI.
--  - The game reads the pad only in ReadJoypads (C0:1797: $4219, then $4218),
--    from mainline code; frame-step mode hands each read the next value. (One
--    value per logic frame is not enough: during a loading screen a read sees
--    whatever frame the real machine is on.)
--  - NMIs: the one that ends a wait in one of the game's NMI wait loops
--    (lda $0036 / cmp $0036 / beq, found by scanning PRG-ROM; it must return
--    past the lda) ends a logic frame. Others (lag, loading) still run part
--    of the handler, e.g. the $32 frame counter that bomb updates key off,
--    so frame-step mode replays them too.
--
-- Savestate sidecars: "<state without .mss>.inputs", the log up to the state.
-- Format: little-endian 16-bit words, no header: pad reads ($4218 | $4219 <<
-- 8, low nibble always 0), $0001 = logic-frame NMI, $0002 | 4 if $16 != 0 |
-- 8 if $18 != 0 | $10 if it hit a wait loop's lda = any other NMI.
--
-- Use:
--   local inputlog = dofile(dir .. "/inputlog.lua")
--   inputlog.start(statePath)   -- nil = from power-on; else call it right
--                               -- after loading the state: reads its sidecar
--   local n = inputlog.mark()   -- current length (keep with an in-memory state)
--   inputlog.truncate(n)        -- after reloading that state
--   inputlog.save(path)         -- false if the start state had no sidecar
--   inputlog.saveState(path, data)  -- writes a .mss and its sidecar
--   inputlog.frames()           -- logic frames since power-on
--   inputlog.onFrame(fn)        -- fn(n, nmiCountLowByte, extraNmis, loop) at the
--                               -- end of each logic frame (in the NmiCount write;
--                               -- loop = address of the wait loop's lda)
local M = {}

local mem = emu.memType.snesMemory
local wram = emu.memType.snesWorkRam
local log, valid = {}, true
local installed = false
local loops
local frames, extras = 0, 0
local frameFns = {}

function M.sidecar(statePath)
  return (statePath:gsub("%.mss$", "")) .. ".inputs"
end

-- The read is in progress; the auto-joypad registers are stable by then
-- (ReadJoypads waits for $4212 bit 0).
local function onPadRead()
  log[#log + 1] = emu.read(0x4218, mem, false) + emu.read(0x4219, mem, false) * 0x100
end

local function scan()
  local rom = emu.memType.snesPrgRom
  local pat = { 0xAD, 0x36, 0x00, 0xCD, 0x36, 0x00, 0xF0, 0xFB }
  loops = {}
  for o = 0, emu.getMemorySize(rom) - #pat do
    if emu.read(o, rom, false) == 0xAD then
      local ok = true
      for k = 2, #pat do
        if emu.read(o + k - 1, rom, false) ~= pat[k] then ok = false; break end
      end
      if ok then
        loops[#loops + 1] = 0xC00000 + o
        if o % 0x10000 >= 0x8000 and o < 0x400000 then loops[#loops + 1] = o end
      end
    end
  end
end

-- The loop whose wait an NMI returning to pc ends, if any; and whether pc
-- is a loop's lda (the NMI came just as the frame's logic finished).
local function endsWait(pc)
  for _, lo in ipairs(loops) do
    if pc > lo and pc < lo + 8 then return lo end
  end
  return nil
end
local function atLda(pc)
  for _, lo in ipairs(loops) do
    if pc == lo then return true end
  end
  return false
end

local function word(a) return emu.read(a, wram, false) + emu.read(a + 1, wram, false) * 0x100 end

-- The handler (00:FD84) pushes DB, A, X, Y (16-bit) before its inc of
-- NmiCount, so the interrupted PB:PC is at SP+9..SP+11 then. Stateless, so
-- savestate loads at any point are safe. Other writes (the reset code's WRAM
-- clear) are not an inc.w (Mesen's PC is already past the instruction).
local function onNmiCount(addr, value)
  local st = emu.getState()
  local pc = st["cpu.k"] * 0x10000 + ((st["cpu.pc"] - 3) & 0xFFFF)
  if emu.read(pc, mem, false) ~= 0xEE then return end
  local sp = st["cpu.sp"]
  local function rd(k) return emu.read((sp + k) & 0xFFFF, mem, false) end
  local ret = rd(11) * 0x10000 + rd(10) * 0x100 + rd(9)
  local lo = endsWait(ret)
  if lo then
    log[#log + 1] = 1
    for _, fn in ipairs(frameFns) do fn(frames, value, extras, lo) end
    frames, extras = frames + 1, 0
  else
    log[#log + 1] = 2 | (word(0x16) ~= 0 and 4 or 0) | (word(0x18) ~= 0 and 8 or 0) |
      (atLda(ret) and 0x10 or 0)
    extras = extras + 1
  end
end

local function countFrames()
  frames = 0
  for _, v in ipairs(log) do if v == 1 then frames = frames + 1 end end
end

function M.frames() return frames end
function M.onFrame(fn) frameFns[#frameFns + 1] = fn end

local function readLog(path)
  local fh = io.open(path, "rb")
  if not fh then return nil end
  local data = fh:read("a")
  fh:close()
  local t = {}
  for i = 1, #data - 1, 2 do
    t[#t + 1] = data:byte(i) + data:byte(i + 1) * 0x100
  end
  return t
end

function M.start(statePath)
  if not loops then scan() end
  log, valid, extras = {}, true, 0
  if statePath then
    local t = readLog(M.sidecar(statePath))
    if t then log = t else valid = false end
  end
  countFrames()
  if not installed then
    emu.addMemoryCallback(onPadRead, emu.callbackType.read, 0x004219, 0x004219)
    emu.addMemoryCallback(onPadRead, emu.callbackType.read, 0x804219, 0x804219)
    emu.addMemoryCallback(onNmiCount, emu.callbackType.write, 0x0036, 0x0036)
    installed = true
  end
end

function M.mark() return #log end

function M.truncate(n)
  for i = #log, n + 1, -1 do log[i] = nil end
  countFrames()
  extras = 0
end

function M.save(path)
  if not valid then return false end
  local t = {}
  for i, v in ipairs(log) do t[i] = string.char(v & 0xFF, v >> 8) end
  local fh = assert(io.open(path, "wb"))
  fh:write(table.concat(t))
  fh:close()
  return true
end

function M.saveState(path, data)
  local fh = assert(io.open(path, "wb"))
  fh:write(data)
  fh:close()
  return M.save(M.sidecar(path))
end

return M
