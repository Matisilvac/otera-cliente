#!/usr/bin/env python3
"""Tests of the voice helper's audio engine (client/native/voice), without devices.

Builds otera-voice (or takes --binary), runs the jitter buffer checks and plays a
scripted conversation through the capture chain in several rooms: loud laptop
speakers, headphones, and echo that arrives late. Open mic only works if the
gate never opens on the other player's voice coming out of the speakers, so that
is what most of the thresholds are about.

    python3 tests/client_voice.py --cmake-arg=-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
    python3 tests/client_voice.py --binary /path/to/otera-voice

The two voices come from macOS `say`; elsewhere the rooms are skipped. Every system
runs the jitter checks and a smoke test of `serve` (status file in a folder with
accents, websocket, quit), which is what the game client does.
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SOURCES = REPO / 'client' / 'native' / 'voice'

FAR = ('Che, vamos a cazar dragones al norte de Thais. Traete pociones de mana y la runa '
       'de sudden death, que el lair esta lleno y hay dragon lords en el fondo de la cueva.')
NEAR = ('Dale, voy para el templo. Espera que compro flechas y te alcanzo en el puente, '
        'no te metas solo porque te matan.')

# (name, selftest options)
ROOMS = [
    ('parlantes', []),
    ('auriculares', ['--echo-gain', '0']),
    ('eco tardio', ['--echo-delay', '100']),
    ('eco tardio y fuerte', ['--echo-delay', '100', '--echo-gain', '2']),
    ('eco muy tardio', ['--echo-delay', '180']),
]

failures = []


def check(ok, what):
    print(('ok   ' if ok else 'FAIL ') + what)
    if not ok:
        failures.append(what)


def build(work, cmake_args):
    build_dir = work / 'build'
    subprocess.run(['cmake', '-S', str(SOURCES), '-B', str(build_dir), '-DCMAKE_BUILD_TYPE=Release', *cmake_args],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['cmake', '--build', str(build_dir), '--config', 'Release'], check=True, stdout=subprocess.DEVNULL)
    for candidate in (build_dir / 'otera-voice', build_dir / 'Release' / 'otera-voice.exe',
                      build_dir / 'otera-voice.exe'):
        if candidate.exists():
            return candidate
    raise SystemExit('no se encontro el binario compilado')


def serve_smoke(binary, work):
    """What the game does: launch serve, read the status file, connect, quit."""
    try:
        from websockets.sync.client import connect
    except ImportError:
        print('sin el modulo websockets: se saltea la prueba de serve')
        return
    folder = work / 'Instalación de José' / '.otera-voice'
    status = folder / 'status-1.json'
    process = subprocess.Popen([str(binary), 'serve', str(status), '--null-audio'])
    try:
        end = time.monotonic() + 15
        while not status.exists() and time.monotonic() < end and process.poll() is None:
            time.sleep(0.05)
        check(status.exists(), 'serve escribe su estado en una carpeta con tildes')
        if not status.exists():
            return
        info = json.loads(status.read_text())
        with connect(f'ws://127.0.0.1:{info["port"]}/{info["token"]}') as ws:
            ws.send(json.dumps({'t': 'quit'}))
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        check(process.poll() == 0, 'serve termina limpio con quit')
        check(not status.exists(), 'y borra su archivo de estado')
    finally:
        if process.poll() is None:
            process.kill()


def say(voice, text, path):
    subprocess.run(['say', '-v', voice, '--file-format=WAVE', '--data-format=LEI16@48000', '-o', str(path), text],
                   check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary')
    parser.add_argument('--cmake-arg', action='append', default=[])
    parser.add_argument('--keep', action='store_true', help='deja los wav de cada sala en la carpeta temporal')
    args = parser.parse_args()

    work = Path(tempfile.mkdtemp(prefix='otera-voice-'))
    try:
        binary = Path(args.binary) if args.binary else build(work, args.cmake_arg)

        jitter = subprocess.run([str(binary), 'selftest-jitter'], capture_output=True, text=True)
        print(jitter.stdout, end='')
        check(jitter.returncode == 0, 'jitter buffer')
        serve_smoke(binary, work)

        if not shutil.which('say'):
            print('sin `say`: se saltean las salas (solo macOS)')
            return
        far, near = work / 'far.wav', work / 'near.wav'
        say('Eddy (Español (México))', FAR, far)
        say('Flo (Español (España))', NEAR, near)

        for name, options in ROOMS:
            out = work / name.replace(' ', '-')
            out.mkdir()
            run = subprocess.run([str(binary), 'selftest', str(far), str(near), '--out', str(out), *options],
                                 capture_output=True, text=True, check=True)
            result = json.loads(run.stdout)
            seg, net = result['segments'], result['network']
            print(f'-- {name}')
            # The other player's voice must never open the gate: not while the
            # canceller learns, not once it converged, not after double talk.
            for part in ('far_learning', 'far', 'far_after'):
                check(seg[part]['gate_open'] <= 0.02, f'{name}: la voz del otro no abre ({part} '
                      f'{seg[part]["gate_open"]:.1%})')
            check(seg['silence']['gate_open'] == 0 and seg['silence_end']['gate_open'] == 0,
                  f'{name}: el ruido de la sala no abre')
            check(seg['near']['gate_open'] >= 0.9, f'{name}: tu voz abre ({seg["near"]["gate_open"]:.1%})')
            check(seg['double']['gate_open'] >= 0.9,
                  f'{name}: tu voz pasa mientras el otro habla ({seg["double"]["gate_open"]:.1%})')
            check(seg['near']['out_vs_in_db'] >= -3, f'{name}: tu voz no se atenua ({seg["near"]["out_vs_in_db"]} dB)')
            if options[:2] != ['--echo-gain', '0']:
                check(seg['far']['out_vs_in_db'] <= -20,
                      f'{name}: el eco se cancela ({seg["far"]["out_vs_in_db"]} dB)')
            check(18 <= net['kbps_while_talking'] <= 30, f'{name}: {net["kbps_while_talking"]} kbps hablando')
            check(net['played'] + net['recovered'] + net['concealed'] >= net['received'] * 0.95 and net['late'] <= 2,
                  f'{name}: con 10% de perdida se reproduce todo lo que llega')
        if args.keep:
            print(f'wav en {work}')
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    if failures:
        raise SystemExit(f'{len(failures)} fallas')


if __name__ == '__main__':
    main()
