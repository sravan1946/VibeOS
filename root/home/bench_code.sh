# bench_code.sh - Code open/index/edit spans on serial: sh /home/bench_code.sh > /dev/kmsg
mkdir -p /tmp/cb
rm -f /tmp/cb/big.c
i=0
while [ $i -lt 12 ]; do cat /usr/include/*.h >> /tmp/cb/big.c; i=$((i+1)); done
bench read /tmp/cb/big.c
code --bench /tmp/cb/big.c
awk "{printf \"%s\\r\\n\", \$0}" /tmp/cb/big.c > /tmp/cb/crlf.c
bench read /tmp/cb/crlf.c
code --bench /tmp/cb/crlf.c
code --bench /home/projects/hello.c
echo BENCH-CODE-DONE
