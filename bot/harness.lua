local WATCH_LUA = os.getenv("SBM2_WATCH_LUA")
if WATCH_LUA and WATCH_LUA ~= "" then dofile(WATCH_LUA) end
-- Headless harness: plays an input script, takes screenshots / WRAM dumps on
-- requested frames, writes the CDL at the end.
--
-- Env:
--   SBM2_INPUT   input script; lines "<frame> <buttons>" (buttons held from that
--                frame until the next line; "-" = none). Buttons: a b x y l r
--                start select up down left right, joined with '+'. A line
--                "<frame> end" stops the run.
--   SBM2_SHOTS   comma-separated frames to screenshot + dump WRAM
--   SBM2_OUT     output directory (must exist)
--   SBM2_LOAD    savestate file to load before frame 0 (frames count from it)
--   SBM2_SAVE    "<frame>:<path>" write a savestate at that frame
-- Every savestate written gets a "<state>.inputs" sidecar (bot/inputlog.lua),
-- the input of each logic frame since power-on (read from SBM2_LOAD's own
-- sidecar); <SBM2_OUT>/run.inputs gets the whole run.
local OUT = os.getenv("SBM2_OUT") or "build/run"
local DIR = debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$") or "."
local inputlog = dofile(DIR .. "/inputlog.lua")
local frame = 0

local script = {}
for line in io.lines(os.getenv("SBM2_INPUT")) do
  local f, b = line:match("^%s*(%d+)%s+(%S+)")
  if f then script[#script + 1] = { tonumber(f), b } end
end

local shots = {}
for f in (os.getenv("SBM2_SHOTS") or ""):gmatch("%d+") do shots[tonumber(f)] = true end

local function writeFile(path, data)
  local fh = assert(io.open(path, "wb"))
  fh:write(data)
  fh:close()
end

local function dumpWram(path)
  local t = {}
  for i = 0, 0x1FFFF do t[i + 1] = string.char(emu.read(i, emu.memType.snesWorkRam, false)) end
  writeFile(path, table.concat(t))
end

local function dumpCdl(path)
  local cdl = emu.getCdlData(emu.memType.snesPrgRom)
  local t = {}
  for i = 0, #cdl do t[#t + 1] = string.char(cdl[i] or 0) end
  writeFile(path, table.concat(t))
end

-- Savestate calls are only allowed inside a CPU exec callback, so run fn
-- from a one-shot hook on the next executed instruction.
local function onNextInstruction(fn)
  local id
  id = emu.addMemoryCallback(function()
    emu.removeMemoryCallback(id, emu.callbackType.exec, 0, 0xFFFFFF)
    local ok, err = pcall(fn)
    if not ok then
      writeFile(OUT .. "/error.txt", tostring(err))
      emu.stop(3)
    end
  end, emu.callbackType.exec, 0, 0xFFFFFF)
end

local LOAD = os.getenv("SBM2_LOAD")
if LOAD == "" then LOAD = nil end
local saveFrame, savePath = (os.getenv("SBM2_SAVE") or ""):match("^(%d+):(.+)$")
saveFrame = tonumber(saveFrame)
local loaded = LOAD == nil
if not LOAD then inputlog.start(nil) end

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
  if not loaded then
    local fh = assert(io.open(LOAD, "rb"))
    local data = fh:read("a")
    fh:close()
    onNextInstruction(function()
      emu.loadSavestate(data)
      inputlog.start(LOAD)
    end)
    loaded = true
    return
  end
  if frame == saveFrame then
    onNextInstruction(function() inputlog.saveState(savePath, emu.createSavestate()) end)
  end
  if shots[frame] then
    writeFile(string.format("%s/f%06d.png", OUT, frame), emu.takeScreenshot())
    dumpWram(string.format("%s/f%06d.wram", OUT, frame))
  end
  if held == "end" then
    inputlog.save(OUT .. "/run.inputs")
    dumpCdl(OUT .. "/run.cdl")
    emu.stop(0)
  end
  frame = frame + 1
end, emu.eventType.endFrame)
