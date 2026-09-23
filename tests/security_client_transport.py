#!/usr/bin/env python3
"""Test the real native Connection without a display, game assets or user credentials."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading

PAYLOAD = b'otera-tls-transport-probe-v1'


def certificate(directory, name, openssl):
    cert, key = directory / (name + '.crt'), directory / (name + '.key')
    subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                    '-keyout', str(key), '-out', str(cert)], check=True, capture_output=True)
    return cert, key


def run(binary, source, output):
    openssl = shutil.which('openssl')
    if not openssl:
        raise RuntimeError('OpenSSL CLI is required to create disposable test certificates')
    report = []
    with tempfile.TemporaryDirectory(prefix='otera-tls-') as temporary:
        directory = Path(temporary)
        cert, key = certificate(directory, 'server', openssl)
        other, _ = certificate(directory, 'other', openssl)
        for case in ('tls12', 'tls13', 'wrong-name', 'unknown-ca', 'plaintext', 'missing-ca'):
            listener = socket.socket()
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(15)
            port = listener.getsockname()[1]
            observed = {'application_bytes': 0}

            def serve():
                try:
                    raw, _ = listener.accept()
                    with raw:
                        raw.settimeout(15)
                        if case == 'plaintext':
                            raw.sendall(b'HTTP/1.0 400 TLS required\r\n\r\n')
                            observed['wire_data'] = raw.recv(4096)
                            return
                        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                        context.minimum_version = ssl.TLSVersion.TLSv1_2
                        if case == 'tls12':
                            context.maximum_version = ssl.TLSVersion.TLSv1_2
                        if case == 'tls13':
                            context.minimum_version = ssl.TLSVersion.TLSv1_3
                        context.load_cert_chain(cert, key)
                        with context.wrap_socket(raw, server_side=True) as stream:
                            observed['version'] = stream.version()
                            data = b''
                            while len(data) < len(PAYLOAD):
                                chunk = stream.recv(len(PAYLOAD) - len(data))
                                if not chunk:
                                    break
                                data += chunk
                            observed['application_bytes'] = len(data)
                            if data == PAYLOAD:
                                stream.sendall(data)
                except (OSError, ssl.SSLError):
                    pass
                finally:
                    listener.close()

            worker = threading.Thread(target=serve, daemon=True)
            worker.start()
            env = os.environ.copy()
            env['OTERA_TLS_SERVER_NAME'] = 'wrong.invalid' if case == 'wrong-name' else 'localhost'
            env['OTERA_TLS_CA_FILE'] = str(other if case == 'unknown-ca' else cert)
            if case == 'missing-ca':
                env['OTERA_TLS_CA_FILE'] = str(directory / 'absent.crt')
            env.pop('OTERA_RSA_PUBLIC', None)
            result = subprocess.run([str(binary), f'--otera-tls-probe-port={port}'],
                                    cwd=source, env=env, capture_output=True, timeout=20)
            if case == 'missing-ca':
                # The native client must fail before opening a socket.
                listener.close()
            worker.join(timeout=16)
            expected = 0 if case in ('tls12', 'tls13') else 2
            valid = result.returncode == expected and not worker.is_alive()
            if expected == 0:
                valid &= observed['application_bytes'] == len(PAYLOAD)
                valid &= observed.get('version') == ('TLSv1.2' if case == 'tls12' else 'TLSv1.3')
            else:
                valid &= observed['application_bytes'] == 0
                valid &= PAYLOAD not in observed.get('wire_data', b'')
            report.append({'case': case, 'passed': bool(valid), 'exit_code': result.returncode,
                           'application_bytes': observed['application_bytes'],
                           'tls_version': observed.get('version')})
            print(json.dumps(report[-1]), flush=True)
            if not valid:
                print(result.stdout.decode(errors='replace')[-4000:])
                print(result.stderr.decode(errors='replace')[-4000:])
                break
    record = {'passed': len(report) == 6 and all(r['passed'] for r in report),
              'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'cases': report,
              'scope': 'native Connection TLS and encrypted bidirectional I/O; not full game/GUI QA'}
    output.write_text(json.dumps(record, indent=2) + '\n')
    if not record['passed']:
        raise SystemExit('Native TLS transport checks failed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    run(args.binary.resolve(), args.source.resolve(), args.output.resolve())
