# bench.sh - app speed numbers on serial: sh /home/bench.sh > /dev/kmsg   (see user/bin/bench.c; first-frame times need the gui.c probe)
for a in about calc clock notepad paint sysmon snake tetris mines piano code chess 2048 solitaire settings viewer music files browser; do
  bench launch /bin/$a
done
bench launch /bin/python /home/projects/paint.py
echo BENCH-LAUNCH-DONE
for a in about calc clock notepad paint sysmon snake tetris mines piano code chess 2048 solitaire settings viewer music files browser desktop; do
  /bin/$a &
done
sleep 4
bench idle 10
for a in about calc clock notepad paint sysmon snake tetris mines piano code chess 2048 solitaire settings viewer music files browser; do bench kill $a; done
echo BENCH-IDLE-DONE
