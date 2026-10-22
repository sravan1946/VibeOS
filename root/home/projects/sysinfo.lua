-- sysinfo.lua - what the kernel says about itself, through the `vibe` module.
local si = vibe.sysinfo()
local function mb(kb) return string.format("%.1f MiB", kb / 1024) end
print(string.format("uptime   %d s", si.uptime_ms // 1000))
print(string.format("memory   %s free of %s", mb(si.mem_free_kb), mb(si.mem_total_kb)))
print(string.format("heap     %s used of %s", mb(si.heap_used_kb), mb(si.heap_total_kb)))
print(string.format("cpu idle %.0f%%", si.idle_ms * 100 / math.max(si.uptime_ms, 1)))
print(string.format("screen   %dx%d", si.screen_w, si.screen_h))
print(string.format("\n%-5s %-5s %-9s %8s %8s  %s", "PID", "PPID", "STATE", "MEM KB", "CPU ms", "NAME"))
local procs = vibe.ps()
table.sort(procs, function(a, b) return a.pid < b.pid end)
for _, p in ipairs(procs) do
  print(string.format("%-5d %-5d %-9s %8d %8d  %s", p.pid, p.ppid, p.state, p.mem_kb, p.cpu_ms, p.name))
end
vibe.beep(880, 80)
