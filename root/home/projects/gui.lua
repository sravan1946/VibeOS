-- gui.lua - a window you can draw into: bouncing balls and a mouse trail. Esc or the close button quits.
local W, H = 480, 320
local win = gui.open("Lua", W, H)
local calm = { 0xc9a46a, 0xb87860, 0x8fa98a, 0x7a90a8, 0x968496, 0xa8a296 }   -- muted tones that sit with the Graphite theme
local balls = {}
for i = 1, 6 do
  balls[i] = { x = math.random(30, W - 30), y = math.random(50, H - 30), dx = math.random(-3, 3) | 1, dy = math.random(-3, 3) | 1,
               r = math.random(8, 18), c = calm[i] }
end
local mx, my, frames, t0 = W // 2, H // 2, 0, vibe.uptime()
local trail = {}
while true do
  local ev = win:poll(0)
  while ev do
    if ev.type == "close" or (ev.type == "key" and ev.pressed and ev.key == 27) then return end
    if ev.type == "mouse_move" then
      mx, my = ev.x, ev.y
      trail[#trail + 1] = { mx, my }
      if #trail > 30 then table.remove(trail, 1) end
    end
    ev = win:poll(0)
  end
  win:clear(0x1b1c1f)
  for i = 1, #trail do
    local g = 40 + i * 6
    win:fill(trail[i][1] - 2, trail[i][2] - 2, 5, 5, gui.rgb(g, g, g))
  end
  for _, b in ipairs(balls) do
    b.x, b.y = b.x + b.dx, b.y + b.dy
    if b.x < b.r or b.x > W - b.r then b.dx = -b.dx end
    if b.y < b.r + 24 or b.y > H - b.r then b.dy = -b.dy end
    win:circle(b.x, b.y, b.r, b.c)
  end
  win:rect(0, 0, W, H, 0x34363b)
  win:line(0, 24, W, 24, 0x34363b)
  frames = frames + 1
  local fps = frames / math.max(vibe.uptime() - t0, 0.001)
  win:text(8, 4, string.format("Lua %s on VibeOS   %.0f fps   mouse %d,%d", _VERSION:sub(5), fps, mx, my), 0xe6e7e9, "ui")
  win:flip()
  vibe.sleep(16)
end
