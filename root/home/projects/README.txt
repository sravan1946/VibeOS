Programs here
=============
*.lua            run with  lua NAME.lua
*.py            run with  python NAME.py  (MicroPython 1.29; `python` alone is the REPL). paint.py is a gui app, in the launcher as Python Paint
*.c, sieve/      compiled inside VibeOS by tcc (TinyCC 0.9.27) against the system libc in /usr

  tcc hello.c -o hello && ./hello world        compile, then run
  tcc -run hello.c world                       compile in memory and run, no output file
  cd sieve && tcc main.c sieve.c -o sieve && ./sieve 100     several files at once
  tcc bounce.c -lgui -o bounce && ./bounce     a GUI program (-lgui = windows + ui.h widgets)
  tcc threads.c -o threads && ./threads        pthreads (in libc: create/join, mutexes, condvars)
  tcc -c a.c ; tcc a.o b.o -o prog             separate compile and link

Headers are in /usr/include (libc, gui.h, ui.h), crt1.o / libc.a / libgui.a in /usr/lib.
tcc is C99-ish with no gcc builtins; `tcc -h` lists options. Output is a static ELF linked
at 0x40001000, so the result runs like any program in /bin.
