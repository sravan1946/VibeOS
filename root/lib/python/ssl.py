# ssl for /bin/python: TLS client streams over the system's BearSSL (_ssl). Certificates are always checked
# against /etc/ssl/cert.pem and the host name; there is no server side.
from _ssl import wrap_socket, SSLSocket

PROTOCOL_TLS = PROTOCOL_TLS_CLIENT = 2
CERT_NONE, CERT_REQUIRED = 0, 2

class SSLContext:
    def __init__(self, protocol=PROTOCOL_TLS_CLIENT):
        self.verify_mode = CERT_REQUIRED
        self.check_hostname = True

    def wrap_socket(self, sock, server_side=False, server_hostname=None):
        if server_side:
            raise NotImplementedError("no TLS server side")
        if self.verify_mode == CERT_NONE or not self.check_hostname:
            raise ValueError("certificate checks can't be turned off")
        return wrap_socket(sock, server_hostname=server_hostname)

def create_default_context():
    return SSLContext()
