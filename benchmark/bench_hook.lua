#!/usr/bin/env lua
--[[
  Performance benchmark for cluacov hook

  Usage:
    lua benchmark/bench_hook.lua [iterations]

  This benchmark measures the overhead of the coverage hook by:
  1. Running code without coverage (baseline)
  2. Running the same code with cluacov hook enabled
  3. Comparing the times

  The test code exercises different patterns:
  - Tight loops (many line hits)
  - Function calls (stack walking)
  - Nested functions (different stack depths)
  - Multiple files (file lookup overhead)
]]

local iterations = tonumber(arg[1]) or 100000

-- Try to load cluacov hook
local hook_ok, hook = pcall(require, "cluacov.hook")
if not hook_ok then
  print("ERROR: Could not load cluacov.hook")
  print("Make sure to run: luarocks make")
  os.exit(1)
end

-------------------------------------------------------------------------------
-- Mock runner module (mimics LuaCov's runner structure)
-------------------------------------------------------------------------------
local mock_runner = {
  initialized = true,
  paused = false,
  tick = false,  -- Disable periodic saves for benchmark
  data = {},
  configuration = {
    codefromstrings = false,
    savestepsize = 100,
  },
  file_included = function(filename)
    return true  -- Include all files
  end,
  save_stats = function() end,
}

-------------------------------------------------------------------------------
-- Test workloads
-------------------------------------------------------------------------------

-- Workload 1: Tight loop (tests per-line overhead)
local function tight_loop(n)
  local sum = 0
  for i = 1, n do
    sum = sum + i
  end
  return sum
end

-- Workload 2: Function calls (tests stack walking)
local function recursive_sum(n)
  if n <= 1 then
    return n
  end
  return n + recursive_sum(n - 1)
end

-- Workload 3: Nested functions (tests different stack depths)
local function nested_calls(n)
  local function level1(x)
    local function level2(y)
      local function level3(z)
        return z * 2
      end
      return level3(y) + 1
    end
    return level2(x) + 1
  end
  local sum = 0
  for i = 1, n do
    sum = sum + level1(i)
  end
  return sum
end

-- Workload 4: Table operations (common in real code)
local function table_ops(n)
  local t = {}
  for i = 1, n do
    t[i] = i * 2
    t["key" .. i] = i
  end
  local sum = 0
  for k, v in pairs(t) do
    if type(v) == "number" then
      sum = sum + v
    end
  end
  return sum
end

-- Combined workload
local function combined_workload(n)
  local r1 = tight_loop(n)
  local r2 = recursive_sum(math.min(n, 500))  -- Limit recursion depth
  local r3 = nested_calls(n)
  local r4 = table_ops(n)
  return r1 + r2 + r3 + r4
end

-------------------------------------------------------------------------------
-- Timing utilities
-------------------------------------------------------------------------------

local function get_time()
  -- Use os.clock for CPU time (more consistent)
  return os.clock()
end

local function format_time(seconds)
  if seconds < 0.001 then
    return string.format("%.2f us", seconds * 1000000)
  elseif seconds < 1 then
    return string.format("%.2f ms", seconds * 1000)
  else
    return string.format("%.2f s", seconds)
  end
end

local function format_rate(count, seconds)
  local rate = count / seconds
  if rate > 1000000 then
    return string.format("%.2f M/s", rate / 1000000)
  elseif rate > 1000 then
    return string.format("%.2f K/s", rate / 1000)
  else
    return string.format("%.2f /s", rate)
  end
end

-------------------------------------------------------------------------------
-- Hook setup
-------------------------------------------------------------------------------

local debug_hook = hook.new(mock_runner)

local function enable_hook()
  -- Reset data for clean measurement
  mock_runner.data = {}
  debug.sethook(function(event, line)
    if event == "line" then
      debug_hook(event, line, 2)
    end
  end, "l")
end

local function disable_hook()
  debug.sethook()
end

-------------------------------------------------------------------------------
-- Benchmark runner
-------------------------------------------------------------------------------

local function run_benchmark(name, workload_fn, n, warmup_runs, timed_runs)
  warmup_runs = warmup_runs or 3
  timed_runs = timed_runs or 5

  -- Warmup without hook
  for _ = 1, warmup_runs do
    workload_fn(n)
  end

  -- Measure baseline (no hook)
  local baseline_start = get_time()
  for _ = 1, timed_runs do
    workload_fn(n)
  end
  local baseline_time = (get_time() - baseline_start) / timed_runs

  -- Warmup with hook
  enable_hook()
  for _ = 1, warmup_runs do
    workload_fn(n)
  end
  disable_hook()

  -- Measure with hook
  local hooked_times = {}
  for run = 1, timed_runs do
    mock_runner.data = {}  -- Reset between runs
    enable_hook()
    local start = get_time()
    workload_fn(n)
    local elapsed = get_time() - start
    disable_hook()
    hooked_times[run] = elapsed
  end

  -- Calculate statistics
  table.sort(hooked_times)
  local hooked_median = hooked_times[math.ceil(timed_runs / 2)]
  local hooked_min = hooked_times[1]
  local hooked_max = hooked_times[timed_runs]

  -- Count line hits
  local total_hits = 0
  for filename, file_data in pairs(mock_runner.data) do
    for line, hits in pairs(file_data) do
      if type(line) == "number" then
        total_hits = total_hits + hits
      end
    end
  end

  local overhead = hooked_median - baseline_time
  local overhead_pct = (overhead / baseline_time) * 100
  local overhead_per_hit = total_hits > 0 and (overhead / total_hits) * 1000000000 or 0  -- ns

  return {
    name = name,
    iterations = n,
    baseline = baseline_time,
    hooked_median = hooked_median,
    hooked_min = hooked_min,
    hooked_max = hooked_max,
    overhead = overhead,
    overhead_pct = overhead_pct,
    total_hits = total_hits,
    overhead_per_hit_ns = overhead_per_hit,
  }
end

local function print_result(r)
  print(string.format("\n=== %s ===", r.name))
  print(string.format("  Iterations:     %d", r.iterations))
  print(string.format("  Baseline:       %s", format_time(r.baseline)))
  print(string.format("  With hook:      %s (min: %s, max: %s)",
    format_time(r.hooked_median), format_time(r.hooked_min), format_time(r.hooked_max)))
  print(string.format("  Overhead:       %s (+%.1f%%)", format_time(r.overhead), r.overhead_pct))
  print(string.format("  Line hits:      %d (%s)", r.total_hits, format_rate(r.total_hits, r.hooked_median)))
  print(string.format("  Per-hit cost:   %.1f ns", r.overhead_per_hit_ns))
end

-------------------------------------------------------------------------------
-- Main
-------------------------------------------------------------------------------

print("=" .. string.rep("=", 60))
print("Cluacov Hook Performance Benchmark")
print("=" .. string.rep("=", 60))
print(string.format("Lua version: %s", _VERSION))
print(string.format("Iterations:  %d", iterations))
print(string.format("Hook module: cluacov.hook"))

local results = {}

print("\nRunning benchmarks...")

table.insert(results, run_benchmark("Tight Loop", tight_loop, iterations))
table.insert(results, run_benchmark("Recursive Sum", recursive_sum, math.min(iterations, 500)))
table.insert(results, run_benchmark("Nested Functions", nested_calls, iterations))
table.insert(results, run_benchmark("Table Operations", table_ops, iterations))
table.insert(results, run_benchmark("Combined Workload", combined_workload, math.floor(iterations / 10)))

for _, r in ipairs(results) do
  print_result(r)
end

-- Summary
print("\n" .. string.rep("=", 61))
print("SUMMARY")
print(string.rep("=", 61))

local total_hits = 0
local total_overhead = 0
for _, r in ipairs(results) do
  total_hits = total_hits + r.total_hits
  total_overhead = total_overhead + r.overhead
end

local avg_per_hit = (total_overhead / total_hits) * 1000000000

print(string.format("Total line hits:     %d", total_hits))
print(string.format("Total overhead:      %s", format_time(total_overhead)))
print(string.format("Avg per-hit cost:    %.1f ns", avg_per_hit))
print("")
print("To compare after optimization, run again and check per-hit cost.")
print("Lower per-hit cost = better performance.")
