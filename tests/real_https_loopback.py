#!/usr/bin/env python3
"""Exercise the production transport source against native SDK Mbed TLS."""

import pathlib
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time


def run_openssl(executable: str, *arguments: str) -> None:
    subprocess.run([executable, *arguments], check=True, stdout=subprocess.PIPE,
                   stderr=subprocess.PIPE)


def certificates(directory: pathlib.Path, openssl: str) -> tuple[pathlib.Path, pathlib.Path, pathlib.Path, pathlib.Path]:
    ca_config = directory / "ca.cnf"
    ca_config.write_text("[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ca\n"
                         "[dn]\nCN=OTA loopback CA\n[ca]\nbasicConstraints=critical,CA:TRUE\n"
                         "keyUsage=critical,keyCertSign,cRLSign\n", encoding="ascii")
    ca = directory / "ca.pem"
    ca_key = directory / "ca.key"
    run_openssl(openssl, "req", "-new", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-days", "2", "-config", str(ca_config), "-keyout", str(ca_key),
                "-out", str(ca))

    wrong_ca_config = directory / "wrong-ca.cnf"
    wrong_ca_config.write_text(ca_config.read_text(encoding="ascii").replace(
        "OTA loopback CA", "Unrelated loopback CA"), encoding="ascii")
    wrong_ca = directory / "wrong-ca.pem"
    run_openssl(openssl, "req", "-new", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-days", "2", "-config", str(wrong_ca_config),
                "-keyout", str(directory / "wrong-ca.key"), "-out", str(wrong_ca))

    leaf_key = directory / "leaf.key"
    leaf_request = directory / "leaf.csr"
    leaf = directory / "leaf.pem"
    extension = directory / "leaf.ext"
    extension.write_text("basicConstraints=critical,CA:FALSE\n"
                         "keyUsage=critical,digitalSignature,keyEncipherment\n"
                         "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost\n",
                         encoding="ascii")
    run_openssl(openssl, "req", "-new", "-newkey", "rsa:2048", "-nodes",
                "-subj", "/CN=localhost", "-keyout", str(leaf_key),
                "-out", str(leaf_request))
    run_openssl(openssl, "x509", "-req", "-in", str(leaf_request),
                "-CA", str(ca), "-CAkey", str(ca_key), "-CAcreateserial",
                "-days", "2", "-extfile", str(extension), "-out", str(leaf))
    return ca, wrong_ca, leaf, leaf_key


class LoopbackServer:
    def __init__(self, certificate: pathlib.Path, key: pathlib.Path, mode: str,
                 tls_version: ssl.TLSVersion):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(3)
        self.port = self.listener.getsockname()[1]
        self.mode = mode
        self.sni = None
        self.request_received = False
        self.errors = []
        self.closed_at = None
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.minimum_version = tls_version
        self.context.maximum_version = tls_version
        self.context.load_cert_chain(str(certificate), str(key))
        self.context.set_servername_callback(self.record_sni)
        self.thread = threading.Thread(target=self.serve)
        self.thread.start()

    def record_sni(self, _connection, server_name, _context):
        self.sni = server_name

    def serve(self):
        try:
            connection, _ = self.listener.accept()
            with connection:
                connection.settimeout(2)
                if self.mode == "slow_handshake":
                    time.sleep(0.7)
                    return
                with self.context.wrap_socket(connection, server_side=True) as tls:
                    request = bytearray()
                    while b"\r\n\r\n" not in request and len(request) < 1024:
                        part = tls.recv(1024)
                        if not part:
                            return
                        request.extend(part)
                    self.request_received = b"GET / HTTP/1.1" in request
                    response = b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"
                    if self.mode == "trickle":
                        for byte in response:
                            tls.sendall(bytes([byte]))
                            time.sleep(0.06)
                    else:
                        tls.sendall(response)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass  # Expected when the client rejects a certificate or deadline.
        except Exception as error:  # Surface any unexpected server failure.
            self.errors.append(error)
        finally:
            self.closed_at = time.monotonic()
            self.listener.close()

    def join(self):
        self.thread.join(timeout=3)
        assert not self.thread.is_alive(), "loopback server did not finish"
        assert not self.errors, self.errors


def case(name: str, client: str, certificate: pathlib.Path, key: pathlib.Path,
         ca: pathlib.Path, hostname: str, server_mode: str,
         total_ms: int, idle_ms: int, expected: str, expected_sni: str | None,
         tls_version: ssl.TLSVersion = ssl.TLSVersion.TLSv1_2) -> None:
    server = LoopbackServer(certificate, key, server_mode, tls_version)
    started = time.monotonic()
    result = subprocess.run([client, hostname, str(server.port), str(ca),
                             str(total_ms), str(idle_ms)],
                            capture_output=True, text=True, timeout=3)
    client_finished_at = time.monotonic()
    elapsed_ms = (client_finished_at - started) * 1000
    server.join()
    actual = result.stdout.strip()
    assert actual.startswith(expected), (
        f"{name}: expected {expected}, got exit={result.returncode} "
        f"stdout={actual!r} stderr={result.stderr!r} "
        f"request={server.request_received} sni={server.sni!r}")
    assert result.returncode == (0 if expected == "success read" else 2), name
    assert server.sni == expected_sni, f"{name}: SNI {server.sni!r}"
    client_ms = int(actual.split()[2])
    if expected == "success read":
        assert server.request_received, f"{name}: HTTP request missing"
    if server_mode in ("slow_handshake", "trickle"):
        assert client_ms >= total_ms, f"{name}: failed before deadline ({client_ms}ms)"
    if server_mode == "slow_handshake":
        assert server.closed_at is not None and client_finished_at < server.closed_at, (
            f"{name}: client waited for the server to close")
    if server_mode == "trickle":
        assert server.request_received, f"{name}: HTTP request missing"
    print(f"{name}: {actual}; wall={elapsed_ms:.0f}ms; SNI={server.sni}")


def main() -> None:
    client, openssl = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="eota-real-https-") as temporary:
        ca, wrong_ca, leaf, leaf_key = certificates(pathlib.Path(temporary), openssl)
        case("valid", client, leaf, leaf_key, ca, "localhost", "normal",
             2000, 1000, "success read", "localhost")
        case("TLS 1.3 ticket", client, leaf, leaf_key, ca, "localhost", "normal",
             2000, 1000, "success read", "localhost", ssl.TLSVersion.TLSv1_3)
        case("wrong CA", client, leaf, leaf_key, wrong_ca, "localhost", "normal",
             2000, 1000, "failure connect", "localhost")
        case("wrong hostname", client, leaf, leaf_key, ca, "wrong.local", "normal",
             2000, 1000, "failure connect", "wrong.local")
        case("slow handshake", client, leaf, leaf_key, ca, "localhost", "slow_handshake",
             250, 250, "failure connect", None)
        case("HTTP trickle", client, leaf, leaf_key, ca, "localhost", "trickle",
             450, 450, "failure read", "localhost")


if __name__ == "__main__":
    main()
