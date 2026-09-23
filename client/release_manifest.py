"""Signed, content-addressed release metadata for the Otera updater.

A release publishes, next to the full packages:

    otera-release.json                 signed envelope, the only thing checked at startup
    otera-manifest-<platform>-g<N>.json every file of that platform: path, sha256, size, exec
    otera-pack-g<N>-<i>.bin            the file contents that no earlier release already holds

Files are stored once, by content (sha256), across platforms and across releases. A
manifest points every blob at the pack and byte range that holds it, possibly in an
older release, so an update downloads only the files that changed, with HTTP Range.
The envelope's payload is signed with ECDSA P-256 / SHA-256 and names each platform
manifest by sha256, which binds the manifests, and through them every file, to the
signature. `client/native/otera_updater.cpp` verifies all of it before writing a byte.
"""
import base64
import hashlib
import json
import os
import re
import urllib.error
import urllib.request
import zlib
from datetime import datetime, timezone
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parent
PUBLIC_KEY = ROOT / 'firma-actualizaciones.pub.pem'
DEFAULT_KEY = Path(os.environ.get('OTERA_SIGNING_KEY', '~/.config/otera/firma-actualizaciones.pem')).expanduser()
ORIGIN = 'https://github.com/Matisilvac/otera-cliente/releases/'
FORMAT = 1
PLATFORMS = ('windows', 'macos', 'linux')
LAUNCH = {'windows': 'Otera.exe', 'macos': 'Otera.app', 'linux': 'Otera'}

# Written by the updater and the packager, never part of what a release owns.
INSTALLED = 'otera-installed.json'
STATE_DIR = '.otera-update'
UNOWNED = {INSTALLED, 'otera.log', 'packet.log', 'otera-update-journal.json'}

PACK_LIMIT = 1_500_000_000        # GitHub caps assets at 2 GiB
INCOMPRESSIBLE = 0.97             # store as-is when deflate saves less than 3%
_BAD_CHARS = re.compile(r'[\x00-\x1f\x7f\\:*?"<>|]')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def valid_path(path):
    """Same rule as the native updater: relative, no traversal, portable names."""
    if not isinstance(path, str) or not path or len(path) > 400 or _BAD_CHARS.search(path):
        return False
    parts = path.split('/')
    if any(p in ('', '.', '..') or p != p.strip(' ') or p.endswith('.') for p in parts):
        return False
    return parts[0] != STATE_DIR


def scan(root):
    """Every file a package owns, sorted, with the data needed to rebuild it."""
    root = Path(root)
    files, seen = [], set()
    for path in sorted(root.rglob('*')):
        rel = path.relative_to(root).as_posix()
        if path.is_symlink():
            raise ValueError(f'{root}: symlinks cannot be updated safely: {rel}')
        if not path.is_file() or rel in UNOWNED or rel.split('/')[0] == STATE_DIR:
            continue
        if not valid_path(rel):
            raise ValueError(f'{root}: unsupported file name: {rel}')
        folded = rel.lower()
        if folded in seen:
            raise ValueError(f'{root}: two files differ only in case: {rel}')
        seen.add(folded)
        data = path.read_bytes()
        files.append({'path': rel, 'sha256': sha256(data), 'size': len(data),
                      'exec': bool(path.stat().st_mode & 0o111)})
    return files


def write_installed(root, version, generation):
    """The list of paths the package owns, so an update can delete what it retired."""
    files = [f['path'] for f in scan(root)]
    Path(root, INSTALLED).write_text(json.dumps(
        {'format': FORMAT, 'version': version, 'generation': generation, 'files': files},
        indent=1) + '\n')


# --------------------------------------------------------------------------
# Signing
# --------------------------------------------------------------------------

def _public_key(path=PUBLIC_KEY):
    return serialization.load_pem_public_key(Path(path).read_bytes())


def key_id(public_key):
    der = public_key.public_bytes(serialization.Encoding.DER,
                                  serialization.PublicFormat.SubjectPublicKeyInfo)
    return sha256(der)[:16]


def sign(payload, key_path=DEFAULT_KEY, public_key=PUBLIC_KEY):
    key = serialization.load_pem_private_key(Path(key_path).read_bytes(), None)
    if key_id(key.public_key()) != key_id(_public_key(public_key)):
        raise ValueError(f'{key_path} is not the key whose public half ships in the client')
    body = json.dumps(payload, sort_keys=True, separators=(',', ':')).encode()
    signature = key.sign(body, ec.ECDSA(hashes.SHA256()))
    return {'format': 'otera-signed/1', 'payload': base64.b64encode(body).decode(),
            'signatures': [{'keyid': key_id(key.public_key()), 'algorithm': 'ecdsa-p256-sha256',
                            'signature': base64.b64encode(signature).decode()}]}


def verify(envelope, public_key=PUBLIC_KEY):
    """Return the payload of a signed envelope, or raise."""
    if not isinstance(envelope, dict) or envelope.get('format') != 'otera-signed/1':
        raise ValueError('not an Otera signed envelope')
    body = base64.b64decode(envelope['payload'], validate=True)
    public = _public_key(public_key)
    for entry in envelope.get('signatures', []):
        if entry.get('keyid') != key_id(public) or entry.get('algorithm') != 'ecdsa-p256-sha256':
            continue
        try:
            public.verify(base64.b64decode(entry['signature'], validate=True), body, ec.ECDSA(hashes.SHA256()))
        except InvalidSignature:
            break
        return json.loads(body)
    raise ValueError('release signature does not verify')


# --------------------------------------------------------------------------
# Previous releases: which blobs already live somewhere
# --------------------------------------------------------------------------

def _get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'Otera-Publisher'}), timeout=60) as r:
        return r.read()


def previous_blobs(origin=ORIGIN, get=_get, public_key=PUBLIC_KEY):
    """Blob locations of the latest published release, verified. Empty on first use."""
    try:
        release = verify(json.loads(get(origin + 'latest/download/otera-release.json')), public_key)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return {}, 0
        raise
    blobs = {}
    for platform, entry in release['manifests'].items():
        data = get(f"{origin}download/v{release['version']}/{entry['name']}")
        if sha256(data) != entry['sha256']:
            raise ValueError(f'{platform} manifest of the latest release does not match its signature')
        manifest = json.loads(data)
        for digest, blob in manifest['blobs'].items():
            pack = manifest['packs'][blob['pack']]
            blobs[digest] = dict(tag=pack['tag'], name=pack['name'], size=pack['size'],
                                 offset=blob['offset'], length=blob['length'], encoding=blob['encoding'])
    return blobs, release['generation']


# --------------------------------------------------------------------------
# Building a release
# --------------------------------------------------------------------------

def _encode(data):
    packed = zlib.compress(data, 6)
    if len(packed) < len(data) * INCOMPRESSIBLE:
        return packed, 'deflate'
    return data, 'identity'


def build(packages, version, generation, installer, out_dir, key_path=DEFAULT_KEY, previous=None,
          public_key=PUBLIC_KEY):
    """Write packs, manifests and the signed envelope. Returns the asset paths."""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    tag = f'v{version}'
    previous = dict(previous or {})
    scans = {platform: scan(root) for platform, root in packages.items()}
    sources = {}
    for platform, files in scans.items():
        for f in files:
            sources.setdefault(f['sha256'], Path(packages[platform], f['path']))

    # New blobs in path order, so that a module's files sit next to each other and
    # an update to one module coalesces into a single range request.
    fresh = [d for d in dict.fromkeys(f['sha256'] for files in scans.values() for f in files)
             if d not in previous]
    assets, located, pack, index, offset = [], dict(previous), None, 0, 0
    for digest in fresh:
        data = sources[digest].read_bytes()
        if sha256(data) != digest:
            raise ValueError(f'{sources[digest]} changed while packaging')
        stored, encoding = _encode(data)
        if pack is None or offset + len(stored) > PACK_LIMIT:
            if pack:
                pack.close()
            name = f'otera-pack-g{generation}-{index}.bin'
            index, offset = index + 1, 0
            pack = open(out_dir / name, 'wb')
            assets.append(out_dir / name)
        pack.write(stored)
        located[digest] = dict(tag=tag, name=Path(pack.name).name, offset=offset,
                               length=len(stored), encoding=encoding)
        offset += len(stored)
    if pack:
        pack.close()
    for path in assets:
        size = path.stat().st_size
        for blob in located.values():
            if blob['name'] == path.name:
                blob['size'] = size

    manifests = {}
    for platform, files in scans.items():
        packs, blobs = {}, {}
        for f in files:
            blob = located[f['sha256']]
            key = f"{blob['tag']}/{blob['name']}"
            if key not in packs:
                packs[key] = dict(id=f'p{len(packs)}', tag=blob['tag'], name=blob['name'], size=blob['size'])
            blobs[f['sha256']] = dict(pack=packs[key]['id'], offset=blob['offset'],
                                      length=blob['length'], encoding=blob['encoding'])
        manifest = {'type': 'otera-manifest', 'format': FORMAT, 'platform': platform,
                    'version': version, 'generation': generation, 'launch': LAUNCH[platform],
                    'files': files,
                    'packs': {p.pop('id'): p for p in packs.values()},
                    'blobs': blobs}
        name = f'otera-manifest-{platform}-g{generation}.json'
        data = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
        (out_dir / name).write_bytes(data)
        assets.append(out_dir / name)
        manifests[platform] = {'name': name, 'sha256': sha256(data), 'size': len(data)}

    payload = {'type': 'otera-release', 'format': FORMAT, 'version': version,
               'generation': generation, 'installer': installer,
               'published': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
               'manifests': manifests}
    envelope = out_dir / 'otera-release.json'
    envelope.write_text(json.dumps(sign(payload, key_path, public_key), indent=1) + '\n')
    assets.append(envelope)
    return assets
