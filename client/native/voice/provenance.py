#!/usr/bin/env python3
"""Tie an otera-voice binary to the exact sources it was built from.

The same scheme as client/native/updater/provenance.py: CI (Windows, Linux) and
the local macOS build record `otera-voice-build.json` next to the binary, and
client/empaquetar.py refuses a binary whose record does not match the current
sources or whose bytes changed since.

    python3 provenance.py --platform linux --binary build/otera-voice --output artifacts
"""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCES = ('CMakeLists.txt', 'vcpkg.json', 'miniaudio_impl.c', 'otera_voice.cpp', 'voice_dsp.cpp', 'voice_dsp.h',
           'voice_engine.cpp', 'voice_engine.h', 'voice_net.cpp', 'voice_net.h', 'voice_serve.cpp')
RECORD = 'otera-voice-build.json'


def sources_sha256():
    digest = hashlib.sha256()
    for name in SOURCES:
        # Line endings are normalized: a Windows checkout must hash like the others.
        digest.update(name.encode() + b'\0' + (HERE / name).read_bytes().replace(b'\r\n', b'\n') + b'\0')
    return digest.hexdigest()


def binary_name(platform):
    return 'otera-voice.exe' if platform == 'windows' else 'otera-voice'


def record(platform, binary, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    target = output / binary_name(platform)
    if Path(binary).resolve() != target.resolve():
        shutil.copy2(binary, target)
    data = dict(platform=platform, sources_sha256=sources_sha256(), binary=target.name,
                sha256=hashlib.sha256(target.read_bytes()).hexdigest())
    (output / RECORD).write_text(json.dumps(data, indent=2) + '\n')
    return data


def check(platform, binary):
    binary = Path(binary)
    data = json.loads((binary.parent / RECORD).read_text())
    expected = dict(platform=platform, sources_sha256=sources_sha256(), binary=binary_name(platform),
                    sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    if data != expected:
        raise RuntimeError(f'{binary}: built from other voice sources or modified since '
                           f'(record {data}, expected {expected})')
    return data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--platform', choices=['windows', 'linux', 'macos'], required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(record(args.platform, args.binary, args.output), indent=2))
