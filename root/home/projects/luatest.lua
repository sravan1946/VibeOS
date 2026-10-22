-- luatest.lua - exercises Lua 5.4 on VibeOS: numbers, strings, tables, io, os, coroutines, metatables.
-- Run:  lua projects/luatest.lua [-v]   (-v prints every PASS; FAIL lines and the summary always print;
-- exit status 1 on failure)
local passed, failed = 0, 0
local verbose = arg and arg[1] == "-v"

local function check(name, cond, detail)
  if cond then
    passed = passed + 1
    if verbose then print("PASS " .. name) end
  else
    failed = failed + 1
    print("FAIL " .. name .. (detail and ("  -> " .. tostring(detail)) or ""))
  end
end

local function eq(name, got, want)
  check(name, got == want, "got [" .. tostring(got) .. "] want [" .. tostring(want) .. "]")
end

-- ---- numbers and formatting ----
eq("version", _VERSION, "Lua 5.4")
eq("pi %.3f", string.format("%.3f", math.pi), "3.142")
eq("tostring(pi)", tostring(math.pi), "3.1415926535898")
eq("%g small", string.format("%g", 0.0001), "0.0001")
eq("%g exp", string.format("%g", 1e20), "1e+20")
eq("%.14g", string.format("%.14g", 0.1), "0.1")
eq("%.17g", string.format("%.17g", 0.1), "0.10000000000000001")
eq("%e", string.format("%e", 12345.6789), "1.234568e+04")
eq("%5.1f|%-8d|%05d", string.format("%5.1f|%-8d|%05d", 3.14159, 42, 42), "  3.1|42      |00042")
eq("%x %X %o", string.format("%x %X %o", 255, 255, 8), "ff FF 10")
eq("%a", string.format("%a", 1.5), "0x1.8p+0")
eq("%q float", string.format("%q", 0.5), "0x1p-1")
eq("tostring(1e300)", tostring(1e300), "1e+300")
eq("tostring(-1e-300)", tostring(-1e-300), "-1e-300")
eq("tostring(1e15)", tostring(1e15), "1e+15")
eq("tostring(2^53)", tostring(2^53), "9.007199254741e+15")
eq("2^53 as integer", string.format("%d", 2^53), "9007199254740992")
eq("2^63 float", tostring(2^63), "9.2233720368548e+18")
eq("1/0", tostring(1/0), "inf")
eq("-1/0", tostring(-1/0), "-inf")
check("nan ~= nan", (0/0) ~= (0/0))
eq("3 // 2", 3 // 2, 1)
eq("-3 // 2", -3 // 2, -2)
eq("3.5 // 1", 3.5 // 1, 3.0)
eq("7 % 3", 7 % 3, 1)
eq("-7 % 3", -7 % 3, 2)
eq("7 % -3", 7 % -3, -2)
eq("5.5 % 2", 5.5 % 2, 1.5)
eq("10 / 2 is float", math.type(10 / 2), "float")
eq("3 | 4", 3 | 4, 7)
eq("1 << 62", 1 << 62, 4611686018427387904)
eq("maxinteger", math.maxinteger, 9223372036854775807)
eq("maxinteger+1 wraps", math.maxinteger + 1, math.mininteger)
eq("int division by zero errors", select(2, pcall(function() return 1 // 0 end)):match("by zero") ~= nil, true)
eq("0x10", 0x10, 16)
eq("0xA.8p0", 0xA.8p0, 10.5)
eq("tonumber hex float", tonumber("0x1p4"), 16.0)
eq("tonumber('  12  ')", tonumber("  12  "), 12)
eq("tonumber('1e2')", tonumber("1e2"), 100.0)
eq("tonumber('0x')", tonumber("0x"), nil)
eq("tonumber('abc')", tonumber("abc"), nil)
eq("tonumber('z', 36)", tonumber("z", 36), 35)
eq("math.tointeger", math.tointeger(3.0), 3)
eq("math.floor(-2.5)", math.floor(-2.5), -3)
eq("math.ceil(2.1)", math.ceil(2.1), 3)
eq("math.sqrt(2)", math.sqrt(2), 1.4142135623730951)
eq("math.sin(1)", string.format("%.15f", math.sin(1)), "0.841470984807897")
eq("math.cos(1)", string.format("%.15f", math.cos(1)), "0.540302305868140")
eq("math.exp(1)", math.exp(1), 2.718281828459045)
eq("math.log(8, 2)", math.log(8, 2), 3.0)
eq("math.log(100, 10)", math.log(100, 10), 2.0)
eq("math.fmod(-7, 3)", math.fmod(-7, 3), -1)
eq("math.abs(minint)", math.abs(math.mininteger), math.mininteger)
eq("math.max", math.max(3, 9, 2), 9)
eq("math.ult", math.ult(1, -1), true)
eq("0.1 + 0.2 ~= 0.3", 0.1 + 0.2 == 0.3, false)
eq("0.1 + 0.2 %.17g", string.format("%.17g", 0.1 + 0.2), "0.30000000000000004")
eq("1e308 * 10 = inf", 1e308 * 10, math.huge)
eq("5e-324 > 0", 5e-324 > 0, true)
eq("float key normalisation", ({[1.0] = "a"})[1], "a")
math.randomseed(42)
local r1, r2 = math.random(1, 100), math.random()
check("math.random range", r1 >= 1 and r1 <= 100 and r2 >= 0 and r2 < 1)

-- ---- strings ----
eq("#s", #"hello", 5)
eq("upper", ("hello"):upper(), "HELLO")
eq("rep", ("ab"):rep(3, "-"), "ab-ab-ab")
eq("sub", ("hello world"):sub(-5), "world")
eq("reverse", ("abc"):reverse(), "cba")
eq("byte/char", string.char(("A"):byte(), 66), "AB")
eq("find plain", ("a.b"):find(".", 1, true), 2)
eq("find pattern", select(2, ("hello world"):find("o w")), 7)
eq("match captures", select(2, ("key=value"):match("(%w+)=(%w+)")), "value")
eq("gsub count", select(2, ("a b c"):gsub(" ", "_")), 2)
eq("gsub fn", ("hello"):gsub("l", function(c) return c:upper() end), "heLLo")
eq("gsub table", ("$x $y"):gsub("%$(%w+)", {x = "1", y = "2"}), "1 2")
eq("gsub anchors", ("  trim  "):gsub("^%s+", ""):gsub("%s+$", ""), "trim")
eq("%b()", ("f(a(b)c)d"):match("%b()"), "(a(b)c)")
eq("%f frontier", ("THE (quick) fox"):find("%f[%a]%a+%f[%A]", 5), 6)
do
  local words = {}
  for w in ("one two  three"):gmatch("%a+") do words[#words + 1] = w end
  eq("gmatch", table.concat(words, ","), "one,two,three")
end
do
  local k = {}
  for key, val in ("a=1, b=2"):gmatch("(%w+)=(%w+)") do k[key] = tonumber(val) end
  eq("gmatch pairs", k.a + k.b, 3)
end
eq("format %s %q", string.format("%s %q", "x", 'a"b\n'), 'x "a\\"b\\\n"')
eq("format %10.3s|", string.format("%10.3s|", "abcdef"), "       abc|")
eq("format %c", string.format("%c%c", 72, 105), "Hi")
eq("format %i", string.format("%i", 42), "42")
eq("concat number", 1 .. 2, "12")
eq("tostring(1.0)", tostring(1.0), "1.0")
eq("tostring(-0.0)", tostring(-0.0), "-0.0")
eq("string compare", "a" < "b" and "Z" < "a" and not ("b" < "a"), true)
eq("utf8.char", utf8.char(228, 8364), "\u{e4}\u{20ac}")
eq("utf8.len", utf8.len("h\u{e4}llo\u{20ac}"), 6)
eq("string.pack/unpack", string.unpack("<i4", string.pack("<i4", -2)), -2)
eq("string.pack size", #string.pack("<d", 1.5), 8)
eq("%s with nil via tostring", string.format("%s", nil), "nil")

-- ---- tables ----
do
  local t = {}
  for i = 1, 1000 do t[i] = (i * 7919) % 1009 end
  table.sort(t)
  local ok = true
  for i = 2, #t do if t[i - 1] > t[i] then ok = false end end
  check("table.sort 1000 ascending", ok and #t == 1000)
  table.sort(t, function(a, b) return a > b end)
  ok = true
  for i = 2, #t do if t[i - 1] < t[i] then ok = false end end
  check("table.sort 1000 descending", ok)
  local names = {"pear", "apple", "fig", "banana"}
  table.sort(names)
  eq("sort strings", table.concat(names, " "), "apple banana fig pear")
end
do
  local t = {1, 2, 3}
  table.insert(t, 4)
  table.insert(t, 1, 0)
  eq("insert", table.concat(t, ","), "0,1,2,3,4")
  eq("remove", table.remove(t, 1), 0)
  eq("remove last", table.remove(t), 4)
  eq("unpack", select("#", table.unpack(t)), 3)
  eq("table.move", table.concat(table.move({1, 2, 3}, 1, 3, 2), ","), "1,1,2,3")
  eq("table.pack n", table.pack(nil, nil).n, 2)
  local sparse = {10, 20, nil, 40}
  eq("# with hole is a border", #sparse == 4 or #sparse == 2, true)
  local cnt = 0
  for _ in pairs({a = 1, b = 2, c = 3, 4, 5}) do cnt = cnt + 1 end
  eq("pairs count", cnt, 5)
  local big = {}
  for i = 1, 20000 do big[i] = i end
  for i = 1, 20000 do big[i] = nil end
  eq("big table clear", next(big), nil)
end

-- ---- closures, metatables, goto, varargs ----
do
  local function counter()
    local n = 0
    return function() n = n + 1; return n end
  end
  local a, b = counter(), counter()
  a(); a()
  eq("closures independent", a() * 10 + b(), 31)
end
do
  local Vec = {}
  Vec.__index = Vec
  Vec.__add = function(p, q) return setmetatable({x = p.x + q.x, y = p.y + q.y}, Vec) end
  Vec.__eq = function(p, q) return p.x == q.x and p.y == q.y end
  Vec.__tostring = function(p) return "(" .. p.x .. "," .. p.y .. ")" end
  Vec.__len = function() return 2 end
  Vec.__call = function(self, k) return self[k] end
  Vec.__lt = function(p, q) return p.x < q.x end
  function Vec.new(x, y) return setmetatable({x = x, y = y}, Vec) end
  function Vec:len2() return self.x * self.x + self.y * self.y end
  local v = Vec.new(1, 2) + Vec.new(3, 4)
  eq("__add/__tostring", tostring(v), "(4,6)")
  eq("__eq", Vec.new(1, 1) == Vec.new(1, 1), true)
  eq("__len", #v, 2)
  eq("__call", v("y"), 6)
  eq("method", v:len2(), 52)
  eq("__lt", Vec.new(1, 0) < Vec.new(2, 0), true)
  local defaults = setmetatable({}, {__index = function(_, k) return k .. "!" end})
  eq("__index fn", defaults.hi, "hi!")
  local log = {}
  local proxy = setmetatable({}, {__newindex = function(t, k, v) rawset(t, k, v); log[#log + 1] = k end})
  proxy.a = 1; proxy.b = 2
  eq("__newindex", table.concat(log), "ab")
  local closed = false
  do local _ <close> = setmetatable({}, {__close = function() closed = true end}) end
  eq("<close>", closed, true)
end
do
  local function va(...) return select("#", ...), (select(2, ...)) end
  local n, second = va("a", "b", nil)
  eq("varargs", n * 10 + (second == "b" and 1 or 0), 31)
  local out = {}
  for i = 1, 3 do
    for j = 1, 3 do
      if j == 2 then goto continue end
      out[#out + 1] = i * 10 + j
      ::continue::
    end
  end
  eq("goto continue", table.concat(out, ","), "11,13,21,23,31,33")
end

-- ---- errors ----
do
  local ok, err = pcall(error, "boom")
  check("pcall error string", not ok and err == "boom", err)
  ok, err = pcall(error, {code = 7})
  check("pcall error table", not ok and err.code == 7)
  ok, err = pcall(function() local x = nil; return x.y end)
  check("index nil message", not ok and err:find("attempt to index a nil value", 1, true), err)
  ok, err = pcall(function() return 1 + {} end)
  check("arith on table", not ok and err:find("attempt to perform arithmetic", 1, true), err)
  ok, err = pcall(string.rep, "x", -1)
  check("string.rep(-1) is empty", ok and err == "")
  eq("error with level 2", select(2, pcall(function() local function f() error("lvl", 2) end f() end)):match("lvl") ~= nil, true)
  local tb = select(2, xpcall(function() error("traced") end, debug.traceback))
  check("xpcall traceback", tb:find("stack traceback:", 1, true) and tb:find("traced", 1, true))
  eq("assert returns args", select("#", assert(1, 2, 3)), 3)
  eq("assert message", select(2, pcall(assert, false, "nope")), "nope")
  eq("tostring(error obj)", select(2, pcall(error, setmetatable({}, {__tostring = function() return "E!" end}))) ~= nil, true)
  check("load syntax error", select(2, load("x = = 1")):find("unexpected symbol", 1, true))
  eq("load chunk", load("return 6 * 7")(), 42)
  eq("load env", load("return x", "c", "t", {x = 5})(), 5)
  check("stack overflow is caught", not pcall(function() local function r() return 1 + r() end return r() end))
  local s = string.rep("a", 100000)
  eq("100k string", #(s .. s), 200000)
end

-- ---- coroutines ----
do
  local co = coroutine.create(function(a, b)
    local c = coroutine.yield(a + b)
    local d, e = coroutine.yield(c * 2)
    return d + e
  end)
  local _, x = coroutine.resume(co, 1, 2)
  local _, y = coroutine.resume(co, 10)
  local _, z = coroutine.resume(co, 3, 4)
  eq("coroutine passing values", x * 10000 + y * 100 + z, 30000 + 2000 + 7)
  eq("coroutine dead", coroutine.status(co), "dead")
  check("resume dead fails", not coroutine.resume(co))
  local gen = coroutine.wrap(function() for i = 1, 5 do coroutine.yield(i * i) end end)
  local sum = 0
  for _ = 1, 5 do sum = sum + gen() end
  eq("coroutine.wrap generator", sum, 55)
  local co2 = coroutine.create(function() error("in co") end)
  local ok, msg = coroutine.resume(co2)
  check("error inside coroutine", not ok and msg:find("in co", 1, true))
  eq("isyieldable outside", coroutine.isyieldable(), false)
  local many = {}
  for i = 1, 200 do many[i] = coroutine.wrap(function() coroutine.yield(i); return i * 2 end) end
  local total = 0
  for i = 1, 200 do total = total + many[i]() + many[i]() end
  eq("200 coroutines", total, 200 * 201 // 2 * 3)
end

-- ---- garbage collection ----
do
  collectgarbage()
  local before = collectgarbage("count")
  local junk = {}
  for i = 1, 20000 do junk[i] = {i, tostring(i)} end
  local during = collectgarbage("count")
  junk = nil
  collectgarbage()
  local after = collectgarbage("count")
  check("gc frees memory", during > before + 500 and after < during - 500, string.format("%.0f -> %.0f -> %.0f KB", before, during, after))
  local weak = setmetatable({}, {__mode = "v"})
  weak[1] = {}
  collectgarbage()
  eq("weak table", weak[1], nil)
  local finalised = false
  setmetatable({}, {__gc = function() finalised = true end})
  collectgarbage()
  eq("__gc runs", finalised, true)
end

-- ---- os ----
do
  local t0 = os.time()
  check("os.time is recent", t0 > 1700000000, t0)
  local fixed = os.time({year = 2024, month = 2, day = 29, hour = 12, min = 30, sec = 15})
  local d = os.date("*t", fixed)
  eq("os.time/date roundtrip", d.year * 10000 + d.month * 100 + d.day, 20240229)
  eq("os.date fields", d.hour * 10000 + d.min * 100 + d.sec, 123015)
  eq("os.date yday/wday", d.yday * 10 + d.wday, 60 * 10 + 5)
  eq("os.date format", os.date("%Y-%m-%d %H:%M:%S", fixed), "2024-02-29 12:30:15")
  eq("os.date %A %B", os.date("%A %B %j", fixed), "Thursday February 060")
  eq("os.date ! utc", os.date("!%Y-%m-%d", 86400 * 365), "1971-01-01")
  eq("os.date %V %G", os.date("!%V %G", 1609632000), "53 2020")
  eq("normalising mktime", os.date("%Y-%m-%d", os.time({year = 2023, month = 14, day = 1, hour = 12})), "2024-02-01")
  eq("difftime", os.difftime(fixed + 90, fixed), 90.0)
  local c0 = os.clock()
  local x = 0
  for i = 1, 200000 do x = x + i end
  check("os.clock is a number", type(os.clock()) == "number" and os.clock() >= c0)
  check("os.getenv", os.getenv("NO_SUCH_VAR_XYZ") == nil)
  check("os.tmpname", type(os.tmpname()) == "string")
end

-- ---- io ----
do
  local path = "/tmp/luatest.txt"
  local f = assert(io.open(path, "w"))
  f:write("line one\n", "line ", 2, "\n", 3.5, " ", 42, "\nlast line without newline")
  f:close()
  f = assert(io.open(path, "r"))
  eq("read l", f:read("l"), "line one")
  eq("read L", f:read("L"), "line 2\n")
  eq("read n", f:read("n"), 3.5)
  eq("read n int", f:read("n"), 42)
  f:read("l")
  eq("read rest", f:read("a"), "last line without newline")
  eq("read at eof", f:read("l"), nil)
  eq("seek set/cur", f:seek("set", 5), 5)
  eq("read 3 bytes", f:read(3), "one")
  eq("seek end", f:seek("end"), 48)
  f:close()
  check("closed file errors", not pcall(f.read, f))
  eq("io.type", io.type(f), "closed file")
  local lines = {}
  for l in io.lines(path) do lines[#lines + 1] = l end
  eq("io.lines count", #lines, 4)
  f = assert(io.open(path, "a")); f:write("\nappended"); f:close()
  f = assert(io.open(path, "rb")); local all = f:read("a"); f:close()
  eq("append mode", all:sub(-8), "appended")
  f = assert(io.open(path, "r+")); f:seek("set", 5); f:write("ONE"); f:seek("set", 0)
  eq("r+ overwrite", f:read("l"), "line ONE"); f:close()
  local bin = {}
  for i = 0, 255 do bin[#bin + 1] = string.char(i) end
  bin = table.concat(bin)
  f = assert(io.open(path, "wb")); f:write(bin:rep(20)); f:close()
  f = assert(io.open(path, "rb")); local back = f:read("a"); f:close()
  eq("binary roundtrip 5120 bytes", back == bin:rep(20), true)
  local nf, err = io.open("/nonexistent/dir/file", "r")
  check("io.open failure", nf == nil and err:find("No such file", 1, true), err)
  eq("io.write returns file", io.write("") == io.stdout, true)
  check("io.output is stdout", io.output() == io.stdout)
  f = io.tmpfile(); f:write("tmp"); f:seek("set"); eq("io.tmpfile", f:read("a"), "tmp"); f:close()
  check("rename + remove", os.rename(path, path .. ".2") and os.remove(path .. ".2"))
  local okr, rerr = os.remove("/tmp/definitely-not-here")
  check("remove missing", okr == nil and rerr:find("No such file", 1, true), rerr)
  f = assert(io.open(path, "w")); f:setvbuf("no"); f:write("x"); f:close(); os.remove(path)
end

-- ---- modules and dofile ----
do
  local path = "/tmp/luatest_mod.lua"
  local f = assert(io.open(path, "w"))
  f:write("local M = {} function M.hi() return 'hi from module' end return M")
  f:close()
  package.path = "/tmp/?.lua;" .. package.path
  eq("require", require("luatest_mod").hi(), "hi from module")
  eq("dofile", dofile(path).hi(), "hi from module")
  eq("loadfile", loadfile(path)().hi(), "hi from module")
  os.remove(path)
  eq("package.loaded", package.loaded["luatest_mod"] ~= nil, true)
end

-- ---- vibe ----
do
  local si = vibe.sysinfo()
  check("vibe.sysinfo", si.mem_total_kb > 0 and si.screen_w > 0 and si.uptime_ms > 0)
  local procs = vibe.ps()
  local me = false
  for _, p in ipairs(procs) do if p.name:find("lua", 1, true) then me = true end end
  check("vibe.ps lists lua", #procs > 0 and me)
  local t0 = vibe.uptime()
  vibe.sleep(100)
  local dt = vibe.uptime() - t0
  check("vibe.sleep/uptime", dt >= 0.09 and dt < 1, dt)
  check("vibe.listdir", #vibe.listdir("/bin") > 10 and vibe.isdir("/bin") and not vibe.isdir("/bin/lua"))
  check("vibe.getcwd", type(vibe.getcwd()) == "string")
end

print(string.format("\n%d passed, %d failed", passed, failed))
if failed == 0 then print("ALL PASS") else print("SOME FAILED") end
os.exit(failed == 0 and 0 or 1)
