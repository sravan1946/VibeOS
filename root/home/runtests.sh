# runtests.sh - every in-VM suite, one summary line each, on the terminal and in the kernel log (serial). tools/vmtest.py
# `suites` starts it from the kernel's fw_cfg autostart; the lines go to /dev/kmsg (writable) from here, so nothing is redirected
o() { echo "$*"; echo "$*" > /dev/kmsg; }
r() { o "SUITE $*: $("$@" 2>/dev/null | tee /tmp/suite.out | tail -1)"; grep '^FAIL' /tmp/suite.out > /dev/kmsg; }   # failing checks follow the summary
r proctest
r memtest
r sigtest
r sh /home/shtest.sh
r sh /home/texttest.sh
r sh /home/filestest.sh
r sh /home/maketest.sh
r python /home/projects/pytest.py
r lua /home/projects/luatest.lua
r qjs /home/projects/test.js
o ALL-SUITES-DONE
