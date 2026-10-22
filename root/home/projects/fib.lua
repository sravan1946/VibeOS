-- fib.lua - Fibonacci three ways.   lua projects/fib.lua [n]
local n = tonumber(arg and arg[1]) or 27

local function fib(k) if k < 2 then return k end return fib(k - 1) + fib(k - 2) end

local memo = {}
local function fibm(k)
  if k < 2 then return k end
  memo[k] = memo[k] or fibm(k - 1) + fibm(k - 2)
  return memo[k]
end

local function fibi(k)
  local a, b = 0, 1
  for _ = 1, k do a, b = b, a + b end
  return a
end

local t0 = vibe.uptime()
print(string.format("fib(%d) recursive = %d  (%.2fs)", n, fib(n), vibe.uptime() - t0))
print(string.format("fib(%d) memoised  = %d", n, fibm(n)))
print(string.format("fib(%d) iterative = %d", n, fibi(n)))
print("fib(90) = " .. fibi(90) .. "  (64-bit integers)")
local line = {}
for i = 0, 15 do line[#line + 1] = fibi(i) end
print(table.concat(line, " "))
