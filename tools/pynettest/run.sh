#!/bin/sh
# /bin/python network end to end, no internet: `python projects/pytest.py net` in the VM talks to servers on the host
# (TCP/UDP echo, three HTTPS servers behind a throwaway CA from tools/tlstest) and serves one connection that the host
# opens through QEMU's hostfwd. The VM reports its result to host.py over TCP; see build/pynettest.png.
set -e
cd "$(dirname "$0")/../.."
W=build/tlstest; rm -rf $W; mkdir -p $W/www
cp tools/tlstest/serve.py $W/
cd $W
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 2 -subj "/CN=VibeOS Test CA" 2>/dev/null
leaf() {
    openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout $1.key -out $1.csr -subj "/CN=$2" 2>/dev/null
    printf "subjectAltName=DNS:$2\nextendedKeyUsage=serverAuth\n" > $1.ext
    openssl x509 -req -in $1.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out $1.pem -days 2 -extfile $1.ext 2>/dev/null
}
leaf good 10.0.2.2; leaf wrongname wrong.example
openssl req -x509 -newkey rsa:2048 -nodes -keyout rogue.key -out rogue.pem -days 2 -subj "/CN=10.0.2.2" -addext "subjectAltName=DNS:10.0.2.2" 2>/dev/null
head -c 300000 /dev/urandom > www/blob
cat /etc/ssl/certs/ca-certificates.crt ca.pem > bundle.pem
python3 serve.py 200 & SRV=$!
cd ../..
python3 tools/pynettest/host.py 150 $W & HOST=$!
make -s CA_BUNDLE=$W/bundle.pem
python3 tools/vmtest.py --timeout 120 --hostfwd tcp:127.0.0.1:18081-:8081 'wait:[desktop] ready' \
    'type:python projects/pytest.py net\n' sleep:60 shot:build/pynettest.png || true
R=0; wait $HOST || R=$?
kill $SRV 2>/dev/null || true
make -s                                    # back to the stock bundle
[ $R = 0 ] && echo "pynettest: ok" || { echo "pynettest: FAILED"; exit 1; }
