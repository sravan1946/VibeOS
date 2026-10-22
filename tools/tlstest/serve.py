import http.server, ssl, sys, threading, functools, os
d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "www")
H = functools.partial(http.server.SimpleHTTPRequestHandler, directory=d)
for port, name in ((18443, "good"), (18444, "rogue"), (18445, "wrongname")):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(f"{name}.pem", f"{name}.key")
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
threading.Event().wait(int(sys.argv[1]))
