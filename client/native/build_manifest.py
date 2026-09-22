"""Collect native release binaries with pinned source and patch provenance."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

SOURCE_COMMIT = 'dd5641492a71e966b96b8a91398b44bb3df67d88'
PATCH = Path(__file__).resolve().parents[1] / 'patches/otclient-4.1-autowalk.patch'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['windows', 'linux'], required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    revision = subprocess.check_output(['git', '-C', str(args.source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != SOURCE_COMMIT:
        raise SystemExit('Unexpected OTClient source revision')
    # A reverse check proves the delivered source contains the complete patch.
    subprocess.run(['git', 'apply', '--reverse', '--check', str(PATCH)], cwd=args.source, check=True)
    name = 'otclient.exe' if args.platform == 'windows' else 'otclient'
    source = args.source / 'build' / (args.platform + '-release') / 'bin' / name
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.output / name
    shutil.copy2(source, binary)
    manifest = dict(platform=args.platform, source=revision, patch_sha256=digest(PATCH),
                    binary=name, sha256=digest(binary))
    (args.output / 'native-build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))

if __name__ == '__main__':
    main()
