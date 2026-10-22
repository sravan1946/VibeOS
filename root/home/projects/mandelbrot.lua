-- mandelbrot.lua - the Mandelbrot set in truecolor, two pixels per character cell (upper half block).
-- lua projects/mandelbrot.lua [cols [rows]]
local cols = tonumber(arg and arg[1]) or 76
local rows = tonumber(arg and arg[2]) or 22
local maxiter = 60

local function hsv(h, s, v)            -- h in [0,1)
  local i = math.floor(h * 6)
  local f = h * 6 - i
  local p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
  local r, g, b
  i = i % 6
  if i == 0 then r, g, b = v, t, p elseif i == 1 then r, g, b = q, v, p
  elseif i == 2 then r, g, b = p, v, t elseif i == 3 then r, g, b = p, q, v
  elseif i == 4 then r, g, b = t, p, v else r, g, b = v, p, q end
  return math.floor(r * 255), math.floor(g * 255), math.floor(b * 255)
end

local function pixel(px, py)           -- px in [0, cols), py in [0, rows*2)
  local cr = -2.2 + px / cols * 3.2
  local ci = -1.2 + py / (rows * 2) * 2.4
  local zr, zi, n = 0, 0, 0
  while n < maxiter and zr * zr + zi * zi <= 4 do
    zr, zi = zr * zr - zi * zi + cr, 2 * zr * zi + ci
    n = n + 1
  end
  if n == maxiter then return 0, 0, 0 end
  return hsv((n * 7 % maxiter) / maxiter * 0.85 + 0.55, 0.8, 1)
end

local t0 = vibe.uptime()
local out = {}
for y = 0, rows - 1 do
  local row = {}
  for x = 0, cols - 1 do
    local r1, g1, b1 = pixel(x, y * 2)
    local r2, g2, b2 = pixel(x, y * 2 + 1)
    row[#row + 1] = string.format("\27[38;2;%d;%d;%d;48;2;%d;%d;%dm\u{2580}", r1, g1, b1, r2, g2, b2)
  end
  out[#out + 1] = table.concat(row) .. "\27[0m"
end
print(table.concat(out, "\n"))
print(string.format("%dx%d cells, %d iterations max, %.1fs", cols, rows, maxiter, vibe.uptime() - t0))
