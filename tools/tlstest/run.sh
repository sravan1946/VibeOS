#!/bin/sh
# TLS end-to-end check, no internet: a throwaway CA and three HTTPS servers on the host (trusted, unknown CA,
# wrong name), an image whose bundle trusts the CA, then wget in the VM must accept, reject, reject.
set -e
cd "$(dirname "$0")/../.."
W=build/tlstest; rm -rf $W; mkdir -p $W/www
cp tools/tlstest/serve.py $W/
cd $W
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 2 -subj "/CN=VibeOS Test CA" 2>/dev/null
leaf() {   # name dns-name
    openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout $1.key -out $1.csr -subj "/CN=$2" 2>/dev/null
    printf "subjectAltName=DNS:$2\nextendedKeyUsage=serverAuth\n" > $1.ext
    openssl x509 -req -in $1.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out $1.pem -days 2 -extfile $1.ext 2>/dev/null
}
leaf good 10.0.2.2; leaf wrongname wrong.example
openssl req -x509 -newkey rsa:2048 -nodes -keyout rogue.key -out rogue.pem -days 2 -subj "/CN=10.0.2.2" -addext "subjectAltName=DNS:10.0.2.2" 2>/dev/null
head -c 300000 /dev/urandom > www/blob
cat /etc/ssl/certs/ca-certificates.crt ca.pem > bundle.pem
python3 serve.py 150 & SRV=$!
cd ../..
make -s CA_BUNDLE=$W/bundle.pem
python3 tools/vmtest.py --timeout 60 --hostfwd tcp:127.0.0.1:18080-:80 'wait:[desktop] ready' \
    'type:wget -O /tmp/blob https://10.0.2.2:18443/blob; wget https://10.0.2.2:18444/ > /dev/null; wget https://10.0.2.2:18445/ > /dev/null; httpd /tmp > /dev/null &\n' \
    sleep:6 shot:build/tlstest.png \
    "host:curl -s http://127.0.0.1:18080/blob | cmp - $W/www/blob && echo 'tls: download intact'"
kill $SRV 2>/dev/null || true
make -s                                    # back to the stock bundle
echo "tls: see build/tlstest.png for the two rejections (not trusted / different host)"
