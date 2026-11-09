#!/usr/bin/env python3
"""Local server for the iframe header tests: python3 tools/frametest/server.py [PORT]   (the guest reaches the host at 10.0.2.2).
Serves root/home/Documents/frames/ at /, plus pages that say who may frame them and an /echo that shows a form POST:
  /ok.html  no restriction          /deny.html  X-Frame-Options: DENY          /same.html  X-Frame-Options: SAMEORIGIN
  /fa-self.html  CSP frame-ancestors 'self'    /fa-none.html  frame-ancestors 'none'    /fa-other.html  frame-ancestors https://other.example
  /fa-host.html  frame-ancestors with this server's own host:port     /echo  the method and body of the request
Each framed page posts 'ok:NAME' to its parent, so the framing page can tell which ones were let in."""
import http.server, os, sys

ROOT = os.path.join(os.path.dirname(__file__), '..', '..', 'root', 'home', 'Documents', 'frames')
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
HDR = {
    'ok': {}, 'deny': {'X-Frame-Options': 'DENY'}, 'same': {'X-Frame-Options': 'SAMEORIGIN'},
    'fa-self': {'Content-Security-Policy': "frame-ancestors 'self'"}, 'fa-none': {'Content-Security-Policy': "frame-ancestors 'none'"},
    'fa-other': {'Content-Security-Policy': 'frame-ancestors https://other.example'},
    'fa-host': {'Content-Security-Policy': 'frame-ancestors 10.0.2.2:%d' % PORT},
}

class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=ROOT, **k)
    def log_message(self, *a): pass
    def page(self, body, hdrs={}, ctype='text/html'):
        b = body.encode()
        self.send_response(200); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(b)))
        for k, v in hdrs.items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        name = self.path.split('?')[0].strip('/')
        if name.endswith('.html') and name[:-5] in HDR:
            n = name[:-5]
            return self.page('<body>framed page %s<script>parent.postMessage("ok:%s", "*")</script></body>' % (n, n), HDR[n])
        if name == 'echo': return self.page('GET ' + self.path, ctype='text/plain')
        super().do_GET()
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
        self.page('POST %s ct=%s body=%s' % (self.path, self.headers.get('Content-Type'), body), ctype='text/plain')

http.server.ThreadingHTTPServer(('0.0.0.0', PORT), H).serve_forever()
