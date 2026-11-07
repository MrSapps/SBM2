-- Unit tests for the bot's route planner and steering, outside Mesen: each
-- test loads bot/bot.lua (with a stand-in emu, bot/test/emu.lua) on a text
-- map from bot/test/maps/ (format in bot/test/maps.lua).
--   build/tools/lua bot/test/test_planner.lua        (make bot-test)
local ROOT = arg[0]:match("^(.*)/bot/test/[^/]*$") or "."
local E = dofile(ROOT .. "/bot/test/emu.lua")
local Maps = dofile(ROOT .. "/bot/test/maps.lua")
local fails, passes = 0, 0
local OUT = os.tmpname(); os.remove(OUT)

local function setup(mapName)
  local spec = Maps.load(ROOT .. "/bot/test/maps/" .. mapName .. ".map")
  E.install { wram = Maps.toWram(spec), rom = ROOT .. "/baserom.sfc", env = { SBM2_OUT = OUT, SBM2_NOASSIST = "1" } }
  local bot = E.loadBot(ROOT .. "/bot/bot.lua")
  local g = bot.readMap()
  local c, r = bot.playerTile()
  return bot, g, c, r, spec
end

local function test(name, fn)
  local ok, err = pcall(fn)
  if ok then passes = passes + 1; print("ok   " .. name)
  else fails = fails + 1; print("FAIL " .. name .. ": " .. tostring(err)) end
end

local function eq(got, want, what)
  if got ~= want then error(string.format("%s: got %s, want %s", what, tostring(got), tostring(want)), 2) end
end

-- The planner's objective, cut down from think(): an open gate or a panel
-- to do, or a soft block next door (on the planner's own grid).
local DIRS = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } }
local function ctxFor(bot, hearts, goal)
  local todo = bot.panels()
  local KEY_W = bot.KEY_W
  return {
    objGoal = goal or function(c, r, grid)
      if todo[r * KEY_W + c] then return true end
      for _, d in ipairs(DIRS) do
        local n = grid[r + d[2]] and grid[r + d[2]][c + d[1]]
        if n == 0xC2 or n == 0x100 then return true end
      end
      return false
    end,
    avoid = {}, maxExp = 4000, slice = 1e9, hearts = hearts or 0,
  }
end
local function kinds(steps)
  local t = {}
  for _, s in ipairs(steps or {}) do t[#t + 1] = s.desc end
  return table.concat(t, " ")
end
local function count(steps, kind)
  local n = 0
  for _, s in ipairs(steps or {}) do if s.kind == kind then n = n + 1 end end
  return n
end

-- Steering ----------------------------------------------------------------

test("steer: 2 px off the row line still walks on (3-5, off the sub)", function()
  local bot = setup("sealed")
  -- origin 0: cell (2,2) centre is (40,40); the game walks rows at y = r*16+7.
  eq(bot.steer("right", 40, 38, 2, 2), "right", "y 2 px above")
  eq(bot.steer("right", 40, 42, 2, 2), "right", "y 2 px below")
  eq(bot.steer("down", 42, 40, 2, 2), "down", "x 2 px right")
end)

test("steer: 3 px off aligns first", function()
  local bot = setup("sealed")
  eq(bot.steer("right", 40, 37, 2, 2), "down", "y 3 px above")
  eq(bot.steer("left", 40, 43, 2, 2), "up", "y 3 px below")
  eq(bot.steer("up", 37, 40, 2, 2), "right", "x 3 px left")
end)

-- Routes and the stage planner ---------------------------------------------

test("bfs: no route out of a sealed room", function()
  local bot, g, c, r = setup("sealed")
  eq(bot.bfs(g, c, r, function(cc, rr) return cc == 6 and rr == 2 end), nil, "route")
end)

test("planner: a sealed room has no plan", function()
  local bot, g, c, r = setup("sealed")
  local steps = bot.stagePlan(g, c, r, ctxFor(bot, 0))
  eq(steps, nil, "plan " .. kinds(steps))
end)

test("planner: no bomb on a bit-6 tile (2-4: P1 sealed in on a lit panel $70)", function()
  -- BombButton C0:3082 places nothing when the tile under P1 has bit 6;
  -- the bot's planner sent P1 to (13,12), a lit panel, to bomb from.
  local bot, g, c, r = setup("2-4_sealed")
  local todo = bot.panels()
  local steps = bot.stagePlan(g, c, r, ctxFor(bot, 0, function(cc, rr) return todo[rr * bot.KEY_W + cc] end))
  for _, st in ipairs(steps or {}) do eq(st.desc == "bomb(13,12)", false, "plan " .. kinds(steps)) end
end)

test("planner: bomb pushes the block into the hole", function()
  local bot, g, c, r = setup("push_into_hole")
  local steps = bot.stagePlan(g, c, r, ctxFor(bot, 0))
  eq(kinds(steps), "bomb(3,2)", "plan")
  local ch = {}
  for _, x in ipairs(steps[1].changes) do ch[#ch + 1] = string.format("(%d,%d)=%02X", x[1], x[2], x[3]) end
  eq(table.concat(ch, " "), "(4,2)=00 (5,2)=00", "map changes")
end)

test("planner: a heart is kept when a bomb clears the trap", function()
  local bot, g, c, r = setup("trap_door")
  eq(kinds(bot.stagePlan(g, c, r, ctxFor(bot, 1))), "bomb(3,2)", "plan")
end)

test("planner: without a heart, a bomb clears the trap", function()
  local bot, g, c, r = setup("trap_door")
  eq(kinds(bot.stagePlan(g, c, r, ctxFor(bot, 0))), "bomb(3,2)", "plan")
end)

test("5-5 start: P1's pocket is small", function()
  local bot, g, c, r = setup("5-5_start")
  local n = 0
  for _ in pairs(bot.regionOf(g, c, r)) do n = n + 1 end
  assert(n < 12, "region has " .. n .. " cells")
end)

-- Panels as the only goal: what clearing 5-5 needs.
local function panelGoal(bot)
  local todo = bot.panels()
  return function(cc, rr) return todo[rr * bot.KEY_W + cc] == true end
end

-- A blast clears a $63 (5-5 bench run: bomb (4,10) fire 2, f115; (5,10)
-- $63 -> $01 -> $00), so 5-5 needs no hearts: the old model (traps only
-- by heart) wanted 8.
test("5-5 start: with no heart the planner reaches a panel, bombing the traps", function()
  local bot, g, c, r = setup("5-5_start")
  local steps, exp = bot.stagePlan(g, c, r, ctxFor(bot, 0, panelGoal(bot)))
  assert(steps, "no plan in " .. exp .. " states")
  eq(count(steps, "trap"), 0, "trap crossings")
end)

test("5-5 late: the last panel needs two pushes into holes", function()
  local bot, g, c, r = setup("5-5_last_panel")
  local _, done = bot.panels()
  local ctx = ctxFor(bot, 0, panelGoal(bot))
  ctx.avoid = done  -- as think() does: no blast on a finished panel
  eq(kinds(bot.stagePlan(g, c, r, ctx)), "bomb(7,4) bomb(5,4)", "plan")
end)

test("5-5 mid-stage: two pushes reach the next trap", function()
  local bot, g, c, r = setup("5-5_blocks")
  -- A trap to bomb is a goal, as in think(); the soft blocks here at
  -- (2,10)-(2,11) grew back, which think() leaves out (softGone), so no
  -- soft-block goals.
  local ctx = ctxFor(bot, 0, function(cc, rr, grid)
    for _, d in ipairs(DIRS) do
      if grid[rr + d[2]] and grid[rr + d[2]][cc + d[1]] == 0x63 then return true end
    end
    return false
  end)
  eq(kinds(bot.stagePlan(g, c, r, ctx)), "bomb(16,12) bomb(17,12)", "plan")
end)

test("5-5 mid-stage: a panel is still reachable within the bot's 4000 states", function()
  local bot, g, c, r = setup("5-5_blocks")
  local _, done = bot.panels()
  local ctx = ctxFor(bot, 0, panelGoal(bot))
  ctx.avoid = done
  local steps, exp = bot.stagePlan(g, c, r, ctx)
  assert(steps, "no plan in " .. exp .. " states")
end)

test("5-5 fire 4: a push may flip a finished panel (at a cost)", function()
  local bot, _, c, r = setup("5-5_fire4")
  E.frame()  -- think() once: the bot's blast range comes from P1's fire
  local g = bot.readMap()
  local _, done = bot.panels()
  local ctx = ctxFor(bot, 0, panelGoal(bot))
  ctx.avoid = done
  eq(kinds(bot.stagePlan(g, c, r, ctx)), "bomb(8,4) bomb(7,4) bomb(5,4)", "plan")
end)

print(string.format("%d passed, %d failed", passes, fails))
os.execute("rm -rf " .. OUT)
os.exit(fails == 0 and 0 or 1)
