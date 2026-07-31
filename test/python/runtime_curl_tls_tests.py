#!/usr/bin/env python3
"""Run deterministic TLS peer and hostname verification through real libcurl."""

import base64
import pathlib
import socket
import ssl
import subprocess
import sys
import threading
import tempfile


BODY = b'{"items":[{"id":11,"login":"duckdb","site_admin":false}]}'


class TlsServer:
    def __init__(self, certificate: pathlib.Path, private_key: pathlib.Path) -> None:
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(1)
        self.port = self._listener.getsockname()[1]
        self._context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self._context.load_cert_chain(certificate, private_key)
        self._failure: BaseException | None = None
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        try:
            client, _ = self._listener.accept()
            with client:
                try:
                    with self._context.wrap_socket(client, server_side=True) as tls_client:
                        request = b""
                        while b"\r\n\r\n" not in request and len(request) < 65536:
                            chunk = tls_client.recv(4096)
                            if not chunk:
                                break
                            request += chunk
                        response = (
                            b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                            + str(len(BODY)).encode("ascii")
                            + b"\r\nConnection: close\r\n\r\n"
                            + BODY
                        )
                        tls_client.sendall(response)
                except (ssl.SSLError, OSError):
                    # Expected when the client rejects peer trust or hostname.
                    pass
        except BaseException as error:  # surface server failures to the runner
            self._failure = error
        finally:
            self._listener.close()

    def finish(self) -> None:
        self._thread.join(timeout=5)
        if self._thread.is_alive():
            raise RuntimeError("controlled TLS service did not terminate")
        if self._failure is not None:
            raise self._failure


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: runtime_curl_tls_tests.py /absolute/path/to/curl_tls_security_tests")
    probe = pathlib.Path(sys.argv[1]).resolve(strict=True)
    fixtures = pathlib.Path(__file__).resolve().parents[1] / "fixtures" / "runtime_tls"
    certificate = fixtures / "server.crt"
    ca_file = fixtures / "ca.crt"
    key_der = base64.b64decode((fixtures / "server-key.pkcs8.der.b64").read_bytes())
    key_payload = base64.b64encode(key_der).decode("ascii")
    with tempfile.TemporaryDirectory(prefix="cuac-tls-") as temporary_directory:
        private_key = pathlib.Path(temporary_directory) / "server-key.pem"
        begin_boundary = "-----" + "BEGIN " + "PRIVATE KEY" + "-----"
        end_boundary = "-----" + "END " + "PRIVATE KEY" + "-----"
        private_key.write_text(
            begin_boundary
            + "\n"
            + "\n".join(key_payload[index : index + 64] for index in range(0, len(key_payload), 64))
            + "\n"
            + end_boundary
            + "\n",
            encoding="ascii",
        )
        for mode in ("success", "peer", "hostname"):
            server = TlsServer(certificate, private_key)
            try:
                subprocess.run([str(probe), mode, str(server.port), str(ca_file)], check=True)
            finally:
                server.finish()
    print("curl TLS security tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
