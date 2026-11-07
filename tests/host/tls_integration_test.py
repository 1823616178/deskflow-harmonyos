#!/usr/bin/env python3
"""Exercise the app's native TLS transport against a real OpenSSL TLS server.

Needs Linux/WSL, Python 3.12+, make, g++ and openssl. No network downloads.
"""
# SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
import concurrent.futures
import hashlib
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tarfile
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
ARCHIVE_HASH = 'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6'


def run(command, **kwargs):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)
    if result.returncode:
        raise RuntimeError(f'{command[0]} failed ({result.returncode}):\n{result.stdout[-5000:]}')
    return result.stdout


def read_exact(connection, size):
    result = b''
    while len(result) < size:
        part = connection.recv(size - len(result))
        if not part:
            raise EOFError('TLS peer closed early')
        result += part
    return result


def read_frame(connection):
    size = struct.unpack('!I', read_exact(connection, 4))[0]
    if size > 4 * 1024 * 1024:
        raise ValueError('Invalid frame length')
    return read_exact(connection, size)


def frame(payload):
    return struct.pack('!I', len(payload)) + payload


class TlsIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cache = ROOT / '.hvigor' / 'tls-host'
        cache.mkdir(parents=True, exist_ok=True)
        archive = ROOT / 'third_party' / 'mbedtls-3.6.7.tar.bz2'
        if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_HASH:
            raise ValueError('Mbed TLS archive hash mismatch')
        source = cache / 'mbedtls-3.6.7'
        if not source.exists():
            with tarfile.open(archive) as contents:
                contents.extractall(cache, filter='data')
        print('Building native TLS regression driver...', flush=True)
        run(['make', '-C', str(source / 'library'), '-j4'], timeout=180)
        cls.binary = cache / 'tls_transport_test'
        protocol = ROOT / 'entry/src/main/cpp/protocol'
        run(['g++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-DDF_WITH_MBEDTLS=1',
             '-I', str(protocol), '-I', str(source / 'include'),
             str(ROOT / 'tests/host/tls_transport_test.cpp'),
             str(protocol / 'socket_stream.cpp'), str(protocol / 'tls_options.cpp'),
             str(protocol / 'tls_session.cpp'), '-L', str(source / 'library'),
             '-lmbedtls', '-lmbedx509', '-lmbedcrypto', '-o', str(cls.binary)], timeout=90)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='deskflow-tls-')
        self.root = Path(self.temp.name)
        self.identity = self.root / 'client.pem'
        self.cert = self.root / 'server.pem'
        self.key = self.root / 'server.key'
        run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(self.key),
             '-out', str(self.cert), '-days', '2', '-subj', '/CN=Deskflow test'], timeout=15)
        self.pin = hashlib.sha256(ssl.PEM_cert_to_DER_cert(self.cert.read_text())).hexdigest().upper()
        self.pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)

    def tearDown(self):
        self.pool.shutdown(wait=True)
        self.temp.cleanup()

    def exercise(self, pin, mode='normal', require_client=False, tls_version=None, timeout=1500, plain=False):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(8)
        port = listener.getsockname()[1]
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(self.cert), str(self.key))
        if tls_version:
            context.minimum_version = tls_version
            context.maximum_version = tls_version
        if require_client:
            context.verify_mode = ssl.CERT_REQUIRED
            context.load_verify_locations(cafile=str(self.identity))

        def server():
            with listener:
                connection, _ = listener.accept()
                connection.settimeout(6)
                if plain:
                    with connection:
                        # Read until the client times out / interrupts. No TLS or Deskflow response.
                        data = connection.recv(8192)
                        while connection.recv(8192):
                            pass
                        return data
                try:
                    secure = context.wrap_socket(connection, server_side=True)
                except ssl.SSLError:
                    connection.close()
                    return b''
                with secure:
                    if mode == 'reject':
                        try:
                            return secure.recv(8192)
                        except (ssl.SSLError, ConnectionResetError):
                            return b''
                    self.assertIsNotNone(secure.getpeercert(binary_form=True))
                    hello = b'Barrier\x00\x01\x00\x08'
                    secure.sendall(frame(hello))
                    self.assertEqual(read_frame(secure), hello + struct.pack('!I', 9) + b'HarmonyPC')
                    secure.sendall(frame(b'CALV') * 160)
                    totals = {b'A': 0, b'B': 0}
                    for _ in range(200):
                        payload = read_frame(secure)
                        self.assertEqual(len(payload), 4096)
                        self.assertIn(payload[:1], totals)
                        self.assertEqual(payload, payload[:1] * 4096)
                        totals[payload[:1]] += 1
                    self.assertEqual(totals, {b'A': 100, b'B': 100})
                    return b'frames verified'

        future = self.pool.submit(server)
        started = time.monotonic()
        result = run([str(self.binary), str(port), pin, str(self.identity), mode, str(timeout)], timeout=10)
        elapsed = time.monotonic() - started
        received = future.result(timeout=8)
        return result, received, elapsed

    def discover(self):
        result, received, _ = self.exercise('', mode='reject')
        self.assertIn('REJECT TLS_UNTRUSTED ' + self.pin, result)
        self.assertEqual(received, b'')
        self.assertTrue(self.identity.exists())
        self.assertEqual(os.stat(self.identity).st_mode & 0o777, 0o600)
        return self.identity.read_bytes()

    def test_first_connection_requires_explicit_trust(self):
        self.discover()

    def test_tls12_mutual_auth_and_concurrent_frames(self):
        identity = self.discover()
        result, received, _ = self.exercise(':'.join(self.pin[i:i+2].lower() for i in range(0, 64, 2)),
                                          require_client=True, tls_version=ssl.TLSVersion.TLSv1_2)
        self.assertTrue(result.startswith('OK CLIENT '))
        certificate = self.identity.read_text().split('-----END CERTIFICATE-----')[0] + '-----END CERTIFICATE-----'
        expected = hashlib.sha256(ssl.PEM_cert_to_DER_cert(certificate)).hexdigest().upper()
        self.assertEqual(result.strip(), 'OK CLIENT ' + expected)
        self.assertEqual(received, b'frames verified')
        self.assertEqual(identity, self.identity.read_bytes())

    def test_tls13_mutual_auth_and_concurrent_frames(self):
        self.discover()
        result, _, _ = self.exercise(self.pin, require_client=True, tls_version=ssl.TLSVersion.TLSv1_3)
        self.assertTrue(result.startswith('OK CLIENT '))

    def test_changed_certificate_rejected_without_application_data(self):
        self.discover()
        result, received, _ = self.exercise('0' * 64, mode='reject')
        self.assertIn('REJECT TLS_PIN_MISMATCH ' + self.pin, result)
        self.assertEqual(received, b'')

    def test_silent_peer_has_bounded_handshake_and_no_plaintext_fallback(self):
        result, received, elapsed = self.exercise(self.pin, mode='reject', plain=True, timeout=250)
        self.assertIn('REJECT TLS_HANDSHAKE_ERROR', result)
        self.assertLess(elapsed, 2)
        self.assertNotIn(b'Barrier', received)
        self.assertEqual(received[0], 0x16)  # TLS handshake record

    def test_stop_interrupts_handshake(self):
        result, _, elapsed = self.exercise(self.pin, mode='interrupt', plain=True, timeout=5000)
        self.assertIn('REJECT', result)
        self.assertLess(elapsed, 2)

    def test_expired_certificate_rejected_even_with_matching_pin(self):
        run(['openssl', 'x509', '-in', str(self.cert), '-signkey', str(self.key),
             '-days', '-1', '-out', str(self.cert)], timeout=10)
        self.pin = hashlib.sha256(ssl.PEM_cert_to_DER_cert(self.cert.read_text())).hexdigest().upper()
        result, received, _ = self.exercise(self.pin, mode='reject')
        self.assertIn('REJECT TLS_CONFIG_ERROR', result)
        self.assertEqual(received, b'')

    def test_invalid_fingerprint_is_not_treated_as_first_use(self):
        result, received, _ = self.exercise('not-a-fingerprint', mode='reject')
        self.assertIn('REJECT TLS_CONFIG_ERROR invalid SHA-256 fingerprint', result)
        self.assertEqual(received, b'')
        self.assertFalse(self.identity.exists())

    def test_corrupt_identity_is_not_silently_replaced(self):
        self.identity.write_bytes(b'corrupt identity')
        result, received, _ = self.exercise(self.pin, mode='reject')
        self.assertIn('REJECT TLS_CONFIG_ERROR client identity:', result)
        self.assertEqual(received, b'')
        self.assertEqual(self.identity.read_bytes(), b'corrupt identity')


if __name__ == '__main__':
    unittest.main(verbosity=2)
