# runtests.sh - every in-VM suite, one summary line each; tools/vmtest.py `suites` sends this to /dev/kmsg (serial)
r() { echo "SUITE $*: $("$@" 2>/dev/null | tail -1)"; }
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
echo ALL-SUITES-DONE
