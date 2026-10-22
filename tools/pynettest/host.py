"""Host side of tools/pynettest/run.sh: an echo server (TCP 18700, UDP 18701) and the report port 18702 that
`python pytest.py net` in the VM talks to. When the VM says "listening" it connects to the forwarded 18081 (-> VM :8081),
and it checks the VM's checksum of the 300000-byte HTTPS blob. Exits 0 only on a clean report."""
import socket, sys, threading, time, os

def serve_tcp():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 18700)); s.listen(5)
    def echo(c):
        while (d := c.recv(65536)): c.sendall(d)
        c.close()
    while True:
        threading.Thread(target=echo, args=(s.accept()[0],), daemon=True).start()

def serve_udp():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind(("0.0.0.0", 18701))
    while True:
        d, a = u.recvfrom(2000); u.sendto(d, a)

for f in (serve_tcp, serve_udp): threading.Thread(target=f, daemon=True).start()
rep = socket.socket(); rep.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
rep.bind(("0.0.0.0", 18702)); rep.listen(1); rep.settimeout(float(sys.argv[1]))
want = sum(open(os.path.join(sys.argv[2], "www/blob"), "rb").read()) & 0xffffff
try: c = rep.accept()[0]
except socket.timeout: print("pynettest: the VM never reported"); sys.exit(1)
c.settimeout(60)
buf, ok = b"", True
while b"RESULT" not in buf:
    try: d = c.recv(1000)
    except socket.timeout: d = b""
    if not d: print("pynettest: report ended early:", buf); sys.exit(1)
    buf += d
    if b"listening" in buf and b"fwd-done" not in buf:
        buf += b"fwd-done"
        for _ in range(20):
            try:
                f = socket.create_connection(("127.0.0.1", 18081), 5); f.sendall(b"ping from host")
                got = f.recv(100); f.close(); break
            except OSError: got = None; time.sleep(0.5)
        print("pynettest: hostfwd ->", got); ok &= got == b"PING FROM HOST"
line = [l for l in buf.decode().replace("fwd-done", "\n").splitlines() if l.startswith("RESULT")][0]
passed, failed = map(int, line.split()[1:])
sums = [l for l in buf.decode().splitlines() if l.startswith("blobsum")]
print("pynettest: VM reports", passed, "passed,", failed, "failed; blob checksum", "ok" if sums and int(sums[0].split()[1]) == want else f"BAD {sums} != {want}")
ok &= failed == 0 and bool(sums) and int(sums[0].split()[1]) == want
sys.exit(0 if ok else 1)
