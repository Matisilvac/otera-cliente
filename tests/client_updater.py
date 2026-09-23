#!/usr/bin/env python3
"""End-to-end tests of the native updater against a local HTTPS release server.

Builds a test copy of client/native/updater (local origin, throwaway signing key,
no relaunch), publishes releases with client/release_manifest.py and runs the real
helper through updates, tampering, failures and crashes.

    python3 tests/client_updater.py --cmake-arg=-DCMAKE_PREFIX_PATH=<vcpkg installed/triplet>
    python3 tests/client_updater.py --cmake-arg=-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake

Runs on macOS, Linux and Windows (the CI workflow runs it on the last two).
"""
import argparse
import datetime
import http.server
import ipaddress
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'client'))
import release_manifest as rm  # noqa: E402

PLATFORM = {'darwin': 'macos', 'win32': 'windows'}.get(sys.platform, 'linux')
EXE = '.exe' if PLATFORM == 'windows' else ''
INSTALLER = 6


# --------------------------------------------------------------------------
# Throwaway PKI, signing key and the helper built against them
# --------------------------------------------------------------------------

def pem(key):
    return key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                             serialization.NoEncryption())


def make_pki(folder):
    now = datetime.datetime.now(datetime.timezone.utc)
    ca_key = ec.generate_private_key(ec.SECP256R1())
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'Otera Test CA')])
    ca = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name)
          .public_key(ca_key.public_key()).serial_number(x509.random_serial_number())
          .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=30))
          .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
          .sign(ca_key, hashes.SHA256()))
    key = ec.generate_private_key(ec.SECP256R1())
    cert = (x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, '127.0.0.1')]))
            .issuer_name(ca_name).public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=30))
            .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address('127.0.0.1'))]), critical=False)
            .sign(ca_key, hashes.SHA256()))
    (folder / 'ca.pem').write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    other_key = ec.generate_private_key(ec.SECP256R1())
    other_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'Unrelated CA')])
    other_ca = (x509.CertificateBuilder().subject_name(other_name).issuer_name(other_name)
                .public_key(other_key.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=30))
                .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
                .sign(other_key, hashes.SHA256()))
    (folder / 'other-ca.pem').write_bytes(other_ca.public_bytes(serialization.Encoding.PEM))
    (folder / 'server.pem').write_bytes(cert.public_bytes(serialization.Encoding.PEM) + pem(key))
    signing = ec.generate_private_key(ec.SECP256R1())
    (folder / 'signing.pem').write_bytes(pem(signing))
    (folder / 'signing.pub.pem').write_bytes(signing.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    other = ec.generate_private_key(ec.SECP256R1())
    (folder / 'other.pem').write_bytes(pem(other))
    (folder / 'other.pub.pem').write_bytes(other.public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def build_helper(folder, port, cmake_args):
    public = (folder / 'signing.pub.pem').read_text().splitlines()
    key = ''.join(f'"{line}\\n"' for line in public)
    header = folder / 'test_config.h'
    header.write_text(f'#define OTERA_UPDATER_ORIGIN "https://127.0.0.1:{port}/releases/"\n'
                      f'#define OTERA_UPDATER_PUBLIC_KEY {key}\n'
                      '#define OTERA_UPDATER_NO_LAUNCH 1\n')
    build = folder / 'build'
    subprocess.run(['cmake', '-S', str(REPO / 'client/native/updater'), '-B', str(build),
                    '-DCMAKE_BUILD_TYPE=Release', f'-DOTERA_UPDATER_TEST_CONFIG={header.as_posix()}', *cmake_args],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['cmake', '--build', str(build), '--config', 'Release'], check=True, stdout=subprocess.DEVNULL)
    for candidate in (build / f'otera-updater{EXE}', build / 'Release' / f'otera-updater{EXE}'):
        if candidate.exists():
            return candidate
    raise FileNotFoundError('otera-updater was not built')


# --------------------------------------------------------------------------
# Release server with Range support and fault injection
# --------------------------------------------------------------------------

class Server:
    def __init__(self, root, cert, port):
        self.root, self.latest, self.log = root, None, []
        self.ignore_range = False
        self.corrupt = {}        # asset name -> byte offset to flip
        self.abort_once = False  # drop the next ranged response halfway
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'   # IXWebSocket, like GitHub, speaks 1.1 only

            def log_message(self, *args):
                pass

            def do_GET(self):
                parts = self.path.split('/')
                if parts[1:4] == ['releases', 'latest', 'download'] and server.latest:
                    self.send_response(302)
                    self.send_header('Location', f'/releases/download/{server.latest}/{parts[4]}')
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                    return
                if parts[1:3] != ['releases', 'download'] or len(parts) != 5:
                    return self.send_error(404)
                path = server.root / parts[3] / parts[4]
                if not path.is_file():
                    return self.send_error(404)
                data = bytearray(path.read_bytes())
                if parts[4] in server.corrupt:
                    data[server.corrupt[parts[4]]] ^= 0xFF
                ranged = self.headers.get('Range')
                server.log.append((parts[4], ranged))
                if ranged and not server.ignore_range:
                    first, last = (int(v) for v in ranged.split('=')[1].split('-'))
                    body = bytes(data[first:last + 1])
                    self.send_response(206)
                    self.send_header('Content-Range', f'bytes {first}-{last}/{len(data)}')
                else:
                    body = bytes(data)
                    self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                if ranged and server.abort_once:
                    server.abort_once = False
                    self.wfile.write(body[:len(body) // 2])
                    self.close_connection = True
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
                self.wfile.write(body)

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert)
        self.httpd.socket = context.wrap_socket(self.httpd.socket, server_side=True)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def get(self, url):
        """Direct file access for the publisher's previous-release lookup."""
        import urllib.error
        parts = url.split('/releases/')[1].split('/')
        if parts[0] == 'latest':
            if not self.latest:
                raise urllib.error.HTTPError(url, 404, 'not found', None, None)
            parts = ['download', self.latest, parts[2]]
        return (self.root / parts[1] / parts[2]).read_bytes()


# --------------------------------------------------------------------------
# Fixtures
# --------------------------------------------------------------------------

class World:
    def __init__(self, folder, helper, server, port):
        self.folder, self.helper, self.server, self.port = folder, helper, server, port
        self.origin = f'https://127.0.0.1:{port}/releases/'
        self.ca = (folder / 'ca.pem').read_bytes()

    def package(self, name, version, generation, files, executable=()):
        root = self.folder / 'packages' / name
        shutil.rmtree(root, ignore_errors=True)
        for path, data in files.items():
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            if path in executable:
                target.chmod(0o755)
        (root / 'cacert.pem').write_bytes(self.ca)
        (root / 'otera-version.json').write_text(json.dumps(
            {'version': version, 'generation': generation, 'installer': INSTALLER}))
        return root

    def publish(self, package, version, generation, key='signing', chain=True, latest=True):
        previous = {}
        if chain:
            previous, _ = rm.previous_blobs(self.origin, self.server.get, self.folder / 'signing.pub.pem')
        rm.build({PLATFORM: package}, version, generation, INSTALLER, self.folder / 'www' / f'v{version}',
                 key_path=self.folder / f'{key}.pem', previous=previous,
                 public_key=self.folder / f'{key}.pub.pem')
        if latest:
            self.server.latest = f'v{version}'

    def resign(self, tag, mutate):
        """Publish a hostile manifest under a valid signature: the helper itself must refuse it."""
        folder = self.folder / 'www' / tag
        envelope = json.loads((folder / 'otera-release.json').read_text())
        payload = rm.verify(envelope, self.folder / 'signing.pub.pem')
        entry = payload['manifests'][PLATFORM]
        manifest = json.loads((folder / entry['name']).read_bytes())
        mutate(manifest)
        data = json.dumps(manifest).encode()
        (folder / entry['name']).write_bytes(data)
        entry.update(sha256=rm.sha256(data), size=len(data))
        (folder / 'otera-release.json').write_text(json.dumps(
            rm.sign(payload, self.folder / 'signing.pem', self.folder / 'signing.pub.pem')))

    def install(self, package):
        target = self.folder / 'instalación con espacios'   # Unicode and spaces, like real Windows profiles
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(package, target)
        version = json.loads((target / 'otera-version.json').read_text())
        rm.write_installed(target, version['version'], version['generation'])
        return target

    def run(self, command, install, pid=None, env=None):
        status = self.folder / 'status.json'
        if status.exists():
            status.unlink()
        args = [str(self.helper), command, str(install), str(status)] + ([str(pid)] if pid is not None else [])
        process = subprocess.run(args, timeout=180, env=dict(os.environ, **(env or {})))
        result = json.loads(status.read_text()) if status.exists() else {}
        result['exit'] = process.returncode
        return result


def dead_pid():
    process = subprocess.Popen([sys.executable, '-c', 'pass'])
    process.wait()
    return process.pid


def tree(root):
    """Package-owned content of a folder, to compare installations exactly."""
    return {f['path']: (f['sha256'], f['exec'] if PLATFORM != 'windows' else False) for f in rm.scan(root)}


def check(condition, message):
    if not condition:
        raise AssertionError(message)
    print('  ok:', message)


# --------------------------------------------------------------------------
# Scenarios
# --------------------------------------------------------------------------

def scenarios(w):
    big = os.urandom(300_000)
    v1 = w.package('v1', '1.0.0', 1, {
        'data/big.bin': big,
        'modules/game_a/a.lua': b'print("a1")\n' * 50,
        'modules/game_old/old.lua': b'retired\n',
        'Case.txt': b'case\n',
        'launch': b'#!/bin/sh\n',
        'data/images/logo[64x64].png': b'\x89PNG fake\n',
        'data/sonidos/canción ñ.txt': b'uno\n',
    }, executable={'launch'})
    v2 = w.package('v2', '1.1.0', 2, {
        'data/big.bin': big,                                # unchanged: never downloaded
        'modules/game_a/a.lua': b'print("a2")\n' * 50,      # changed
        'modules/game_new/new.lua': b'fresh\n',             # added
        'case.txt': b'case\n',                              # renamed only by case
        'launch': b'#!/bin/sh\necho 2\n',                   # changed, stays executable
        'data/images/logo[64x64].png': b'\x89PNG fake\n',
        'data/sonidos/canción ñ.txt': b'dos\n',             # non-ASCII name, changed
    }, executable={'launch'})

    print('first release: check, stage only what changed, apply')
    w.publish(v1, '1.0.0', 1, chain=False, latest=False)
    w.publish(v2, '1.1.0', 2)
    install = w.install(v1)
    result = w.run('check', install)
    check(result['state'] == 'done' and result['update'] and result['version'] == '1.1.0', 'check sees 1.1.0')
    w.server.log.clear()
    result = w.run('stage', install)
    check(result['state'] == 'done' and result['staged'], 'stage completes')
    ranged = [entry for entry in w.server.log if entry[1]]
    check(result['downloaded'] < 10_000 and ranged, f"stage downloads only changed files ({result['downloaded']} bytes, ranged)")
    check(tree(install) == tree(v1), 'staging does not touch the installation')
    result = w.run('apply', install, pid=dead_pid())
    check(result['state'] == 'done' and result['version'] == '1.1.0', 'apply completes')
    check(tree(install) == tree(v2), 'installation equals the 1.1.0 package, byte for byte and mode')
    check(not (install / 'modules/game_old/old.lua').exists(), 'retired file deleted')
    check((install / '.otera-update/launched').read_text() == rm.LAUNCH[PLATFORM], 'game relaunched after apply')
    installed = json.loads((install / 'otera-installed.json').read_text())
    check(installed['generation'] == 2 and 'modules/game_new/new.lua' in installed['files'], 'installed list updated')
    check(not (install / '.otera-update/blobs').exists() and not (install / '.otera-update/backup').exists(), 'staging cleaned up')
    (install / '.otera-update/status-99-1.json').write_text('{}')   # left by an apply
    check(w.run('check', install)['update'] is False, 'up to date afterwards')
    check(not (install / '.otera-update/status-99-1.json').exists(), 'leftover status files cleaned')

    print('chained release: new pack holds only new content, old packs still serve the rest')
    v3 = w.package('v3', '1.2.0', 3, {**{p: (v2 / p).read_bytes() for p in ('data/big.bin', 'case.txt', 'launch',
                                                                        'data/sonidos/canción ñ.txt')},
                                        'modules/game_a/a.lua': b'print("a3")\n' * 50,
                                        'data/images/logo[64x64].png': b'\x89PNG fake\n',
                                        'modules/game_new/new.lua': b'fresh\n'}, executable={'launch'})
    w.publish(v3, '1.2.0', 3)
    packs = sorted(p.name for p in (w.folder / 'www/v1.2.0').glob('otera-pack-*'))
    check(len(packs) == 1 and (w.folder / 'www/v1.2.0' / packs[0]).stat().st_size < 2_000, 'the 1.2.0 pack only holds the changed file')
    install = w.install(v1)                                 # skip a version: 1.0.0 -> 1.2.0
    check(w.run('stage', install)['staged'], 'stage from 1.0.0 straight to 1.2.0')
    check(w.run('apply', install, pid=dead_pid())['state'] == 'done' and tree(install) == tree(v3), 'skipped a version cleanly')

    print('the helper refuses anything that is not signed and whole')
    install = w.install(v2)
    evil = w.package('evil', '9.0.0', 9, {'modules/game_a/a.lua': b'os.execute("evil")\n'})
    w.publish(evil, '9.0.0', 9, key='other', chain=False)
    result = w.run('check', install)
    check(result['state'] == 'error' and result['code'] == 'signature', 'release signed by another key rejected')
    check(tree(install) == tree(v2), 'nothing changed')

    envelope = json.loads((w.folder / 'www/v1.2.0/otera-release.json').read_text())
    import base64
    payload = json.loads(base64.b64decode(envelope['payload']))
    payload['generation'] = 99
    envelope['payload'] = base64.b64encode(json.dumps(payload).encode()).decode()
    forged = w.folder / 'www/v9.9.9'
    forged.mkdir(parents=True, exist_ok=True)
    (forged / 'otera-release.json').write_text(json.dumps(envelope))
    w.server.latest = 'v9.9.9'
    check(w.run('check', install)['code'] == 'signature', 'payload edited after signing rejected')

    w.server.latest = 'v1.2.0'
    manifest = next((w.folder / 'www/v1.2.0').glob('otera-manifest-*'))
    original = manifest.read_bytes()
    manifest.write_bytes(original.replace(b'"generation":3', b'"generation":3 '))
    check(w.run('stage', install)['code'] == 'integrity', 'manifest not matching the signed hash rejected')
    manifest.write_bytes(original)

    pack = sorted((w.folder / 'www/v1.2.0').glob('otera-pack-*'))[0].name
    w.server.corrupt[pack] = 5
    result = w.run('stage', install)
    check(result['code'] == 'integrity' and tree(install) == tree(v2), 'corrupt pack bytes rejected, installation untouched')
    w.server.corrupt.clear()

    w.server.ignore_range = True
    check(w.run('stage', install)['code'] == 'network', 'server ignoring Range rejected')
    w.server.ignore_range = False

    w.server.abort_once = True
    check(w.run('stage', install)['code'] == 'network', 'connection dropped mid-download reported')
    check(w.run('stage', install)['staged'], 'retry after a dropped connection completes')

    hostile = w.package('hostile', '1.3.0', 4, {'x.txt': b'x'})
    for name, mutate in [
        ('path traversal', lambda m: m['files'][0].update(path='../escape.txt')),
        ('absolute path', lambda m: m['files'][0].update(path='/tmp/escape.txt')),
        ('Windows drive path', lambda m: m['files'][0].update(path='C:/escape.txt')),
        ('write into the updater state', lambda m: m['files'][0].update(path='.otera-update/lock')),
        ('duplicate path by case', lambda m: m['files'].append(dict(m['files'][0], path=m['files'][0]['path'].upper()))),
        ('blob outside its pack', lambda m: next(iter(m['blobs'].values())).update(offset=10**9)),
    ]:
        w.publish(hostile, '1.3.0', 4, chain=False)
        w.resign('v1.3.0', mutate)
        fresh = w.install(v2)
        result = w.run('stage', fresh)
        check(result['code'] == 'integrity' and tree(fresh) == tree(v2) and not (w.folder / 'escape.txt').exists(),
              f'{name} rejected')

    w.server.latest = 'v1.0.0'
    check(w.run('check', w.install(v2))['update'] is False, 'an older release never downgrades')

    print('failures while swapping roll back to the previous version')
    w.server.latest = 'v1.2.0'
    install = w.install(v2)
    w.run('stage', install)
    result = w.run('apply', install, pid=dead_pid(), env={'OTERA_TEST_FAIL_AFTER': '2'})
    check(result['code'] == 'apply' and tree(install) == tree(v2), 'failure mid-swap restores every file')
    check(json.loads((install / '.otera-update/result.json').read_text())['ok'] is False, 'the relaunched game learns it failed')
    check(w.run('apply', install, pid=dead_pid())['state'] == 'done' and tree(install) == tree(v3), 'the staged files are reused on retry')

    install = w.install(v2)
    w.run('stage', install)
    result = w.run('apply', install, pid=dead_pid(), env={'OTERA_TEST_CRASH_AFTER': '2'})
    check(result.get('exit') == 3 and (install / '.otera-update/journal.json').exists(), 'crash mid-swap leaves a journal')
    check(tree(install) != tree(v2), 'the crash left a mixed installation')
    w.run('check', install)
    check(tree(install) == tree(v2), 'the next run rolls the crash back')

    print('the helper waits for the game and runs alone')
    install = w.install(v2)
    w.run('stage', install)
    # Nobody reaps this "game" while the helper runs: once it exits it is a zombie,
    # which must count as gone (launchers and shells do not always reap).
    game = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(2)'])
    started = time.monotonic()
    result = w.run('apply', install, pid=game.pid)
    elapsed = time.monotonic() - started
    game.wait()
    check(result['state'] == 'done' and 1.5 <= elapsed < 20, f'apply waited for the game to exit, zombie included ({elapsed:.1f}s)')
    lock = install / '.otera-update/lock'
    lock.write_text(str(os.getpid()))
    check(w.run('check', install)['code'] == 'busy', 'a second helper is turned away')
    lock.unlink()

    check(w.run('version', install)['testing'] is True, 'test build identifies itself')

    print('TLS only trusts the bundled certificates')
    install = w.install(v2)
    (install / 'cacert.pem').write_bytes((w.folder / 'other-ca.pem').read_bytes())
    check(w.run('check', install)['code'] == 'network', 'server certificate from another CA rejected')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cmake-arg', action='append', default=[])
    parser.add_argument('--keep', action='store_true', help='keep the temporary folder')
    args = parser.parse_args()
    folder = Path(tempfile.mkdtemp(prefix='otera-updater-test-'))
    try:
        make_pki(folder)
        port = free_port()
        print('building the test helper...')
        helper = build_helper(folder, port, args.cmake_arg)
        (folder / 'www').mkdir()
        server = Server(folder / 'www', folder / 'server.pem', port)
        scenarios(World(folder, helper, server, port))
        server.httpd.shutdown()
        print('PASS: updater end to end on', PLATFORM)
    finally:
        if args.keep:
            print('kept', folder)
        else:
            shutil.rmtree(folder, ignore_errors=True)


if __name__ == '__main__':
    main()
