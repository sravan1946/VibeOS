# bench_browser.sh - browser load/layout/paint spans on serial: sh /home/bench_browser.sh > /dev/kmsg
for p in about:home file:///home/csstest/bigtable.html file:///home/csstest/tables.html file:///home/csstest/linked.html file:///home/Documents/test.html file:///home/Documents/css.html file:///home/Documents/js.html file:///home/Documents/jstest.html file:///home/Documents/forms.html file:///home/; do
  browser --bench $p
done
echo BENCH-BROWSER-DONE
