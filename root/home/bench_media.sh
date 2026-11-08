# bench_media.sh - image decode / thumbnail, audio decode, cold and warm file reads, interpreter startup: sh /home/bench_media.sh > /dev/kmsg
cd /home/Pictures
bench image bubbles.png dunes.jpg dunes.png dunes-rotated.jpg mandelbrot.png mandelbrot-progressive.jpg orbits.gif bubbles.png dunes.jpg
music --bench /home/Music/low-tide.mp3 /home/Music/pixel-rain.ogg /home/Music/arpeggio.wav
bench read /bin/browser
bench read /bin/vfetch
bench run /bin/python -c pass
bench run /bin/qjs -e 0
bench run /bin/lua -e ""
echo BENCH-MEDIA-DONE
