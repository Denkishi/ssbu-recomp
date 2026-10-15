#!/usr/bin/env python3
"""Start the built game (or run a scripted match) with the paths scripts/setup.py recorded.

    python scripts/run.py --userdata <folder for saves>                    play (window, sound)
    python scripts/run.py --userdata <folder> --seq tests/seq_battle_play.txt --seconds 240 --exe ssbu_match.exe
                                                                           scripted match: the input sequence is played,
                                                                           the window stays off-screen and muted when the exe
                                                                           is not named ssbu_native.exe / ssbu_user.exe

--userdata must be a folder you can lose: the game writes its save data there (use a copy when testing).
Options go to the game as environment variables (listed in BUILDING.md); --env NAME=VALUE adds one.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--userdata', required=True)
    ap.add_argument('--build', default=os.path.join(REPO, 'build'))
    ap.add_argument('--seconds', type=int, default=0, help='stop after this many seconds (0 = play until you quit)')
    ap.add_argument('--seq', help='input sequence file (tests/seq_*.txt): lines starting with # are comments')
    ap.add_argument('--exe', default='ssbu_native.exe', help='exe name inside build/work; a copy of ssbu_native.exe is made if missing')
    ap.add_argument('--refresh', action='store_true', help='copy ssbu_native.exe over --exe again when the copy is older than it')
    ap.add_argument('--seed', help='SSBU_FIXED_SEED value (deterministic random numbers)')
    ap.add_argument('--log', help='write the game output to this file instead of the console')
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE, may be repeated')
    a = ap.parse_args()
    state_file = os.path.join(a.build, 'state.json')
    with open(state_file) as f:
        state = json.load(f)
    work = os.path.join(a.build, 'work')
    exe = os.path.join(work, a.exe)
    native = os.path.join(work, 'ssbu_native.exe')
    if not os.path.exists(exe):
        shutil.copy2(native, exe)
    elif exe != native and os.path.getmtime(exe) < os.path.getmtime(native):
        # a copy made before the last link runs the old runtime; an exe linked on purpose with other sources
        # (RECOMP_EXE) is older too, so it is only replaced when asked
        if a.refresh:
            shutil.copy2(native, exe)
            print('%s was older than ssbu_native.exe: copied again' % a.exe)
        else:
            print('note: %s is older than ssbu_native.exe and may run an older runtime (--refresh copies it again)' % a.exe)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}
    env['SSBU_ROMFS'] = state['dump']['romfs']
    env['SSBU_USERDATA'] = os.path.abspath(a.userdata)
    for sub, var in (('fonts', 'SSBU_FONTS'), ('zoneinfo', 'SSBU_TZDIR')):
        p = os.path.join(a.build, 'sysdata', sub)
        if os.path.isdir(p):
            env[var] = p
    if a.seq:
        with open(a.seq) as f:
            env['SSBU_SEQ'] = ' '.join(l.strip() for l in f if l.strip() and not l.startswith('#'))
    if a.seed:
        env['SSBU_FIXED_SEED'] = a.seed
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v
    os.makedirs(a.userdata, exist_ok=True)
    os.makedirs(os.path.join(a.build, 'cache'), exist_ok=True)      # the runtime creates ..\cache\vk below it but not the folder itself (found 2026-10-13: with SSBU_VK_PREP=0 on a clean build nothing was saved)
    cmd = [exe, os.path.join(a.build, 'binaries', '13.0.4', 'main.elf'), str(a.seconds)]
    t0 = time.time()
    out = open(a.log, 'wb') if a.log else None
    p = subprocess.run(cmd, cwd=work, env=env, stdout=out, stderr=subprocess.STDOUT if out else None)
    print('game exited with code %d after %.0f s' % (p.returncode, time.time() - t0))
    return p.returncode


if __name__ == '__main__':
    sys.exit(main())
