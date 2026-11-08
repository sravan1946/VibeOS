# bench_files.sh - Files listing + thumbnail spans on serial: sh /home/bench_files.sh > /dev/kmsg
mkdir -p /tmp/big
cd /tmp/big
i=0
while [ $i -lt 400 ]; do echo $i > f$i.txt; i=$((i+1)); done
i=0
while [ $i -lt 12 ]; do cp /home/Pictures/bubbles.png b$i.png; cp /home/Pictures/dunes.jpg d$i.jpg; cp /home/Pictures/mandelbrot.png m$i.png; i=$((i+1)); done
cd /
files --bench /home/Pictures
files --bench /tmp/big
echo BENCH-FILES-DONE
