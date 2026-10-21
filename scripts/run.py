#!/usr/bin/env python3
"""Start the built game (or run a scripted match) with the paths scripts/setup.py recorded.

    python scripts/run.py --userdata <folder for saves>                    play (window, sound)
    python scripts/run.py --userdata <folder> --seq tests/seq_battle_play.txt --seconds 240 --exe ssbu_match.exe
                                                                           scripted match: the input sequence is played,
                                                                           the window stays off-screen and muted when the exe
                                                                           is not named ssbu_native.exe / ssbu_user.exe

--linux runs the native Linux build (scripts/setup.py --target linux): build folder build-linux, exe ssbu_native. On Linux
it is the default when build-linux exists and build (a Windows cross build) does not; --userdata then defaults to
$XDG_DATA_HOME/ssbu-recomp/userdata (~/.local/share/ssbu-recomp/userdata).

--mac runs the native macOS build (scripts/setup.py --target macos): build folder build-macos, exe ssbu_native. On a Mac it is
the default when build-macos exists; --userdata then defaults to ~/Library/Application Support/ssbu-recomp/userdata, and
--play writes the game log to build-macos/logs/game_<time>.log (the last 10 are kept).

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
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_location  # noqa: E402


def ini_on(path, key):
    """`key = on` (or 1 / yes / true) in a graphics.ini; False when the file or the key is missing."""
    try:
        with open(path, encoding='utf-8-sig', errors='replace') as f:
            for line in f:
                k, sep, v = line.partition('=')
                if sep and k.strip().lower() == key:
                    return v.split('#')[0].split(';')[0].strip().lower() in ('on', '1', 'yes', 'true')
    except OSError:
        pass
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--userdata', help='the save folder (required, except with --play)')
    ap.add_argument('--play', action='store_true', help='what "Play SSBU.bat" does: the saves in <build>\\saves, until you quit; '
                                                        'a plain message when the game is not built yet')
    ap.add_argument('--build', default=None, help='build folder (default: the install location of build_location.txt, else <repo>/build; <repo>/build-linux with --linux)')
    ap.add_argument('--linux', action='store_true', help='the native Linux build (default build folder build-linux, exe ssbu_native, '
                                                        'saves in ~/.local/share/ssbu-recomp/userdata unless --userdata)')
    ap.add_argument('--seconds', type=int, default=0, help='stop after this many seconds (0 = play until you quit)')
    ap.add_argument('--seq', help='input sequence file (tests/seq_*.txt): lines starting with # are comments')
    ap.add_argument('--exe', help='exe name inside build/work (default ssbu_native.exe, ssbu_native with --linux); a copy of it is made if missing')
    ap.add_argument('--refresh', action='store_true', help='copy ssbu_native.exe over --exe again when the copy is older than it')
    ap.add_argument('--seed', help='SSBU_FIXED_SEED value (deterministic random numbers)')
    ap.add_argument('--log', help='write the game output to this file instead of the console')
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE, may be repeated')
    ap.add_argument('--mac', action='store_true', help='the native macOS build (default build folder build-macos, exe ssbu_native, '
                                                      'saves in ~/Library/Application Support/ssbu-recomp/userdata unless --userdata)')
    a = ap.parse_args()
    if sys.platform.startswith('linux') and not a.linux and not a.build and not os.path.exists(os.path.join(REPO, 'build', 'state.json')):
        a.linux = True
    if sys.platform == 'darwin' and not a.mac and not a.linux and not a.build and os.path.exists(os.path.join(REPO, 'build-macos', 'state.json')):
        a.mac = True
    if a.mac:
        a.build = os.path.abspath(a.build or os.path.join(REPO, 'build-macos'))
        if not a.userdata:
            a.userdata = os.path.expanduser('~/Library/Application Support/ssbu-recomp/userdata')
        if not os.path.exists(os.path.join(a.build, 'state.json')):
            print('no build in %s: run scripts/setup.py --target macos first (BUILDING_MAC.md)' % a.build)
            return 1
        if a.play and not a.log:                      # the game's log to a file (as --play does on Windows), the last 10 kept
            logs = os.path.join(a.build, 'logs')
            os.makedirs(logs, exist_ok=True)
            a.log = os.path.join(logs, time.strftime('game_%Y%m%d_%H%M%S.log'))
            old = sorted(n for n in os.listdir(logs) if n.startswith('game_') and n.endswith('.log'))
            for n in old[:-9]:
                try:
                    os.remove(os.path.join(logs, n))
                except OSError:
                    pass
            print('Playing. The game log goes to %s. Close the game window to quit.' % a.log)
    elif a.linux:
        a.build = os.path.abspath(a.build or os.path.join(REPO, 'build-linux'))
        if not a.userdata:
            a.userdata = os.path.join(os.environ.get('XDG_DATA_HOME') or os.path.expanduser('~/.local/share'), 'ssbu-recomp', 'userdata')
        if not os.path.exists(os.path.join(a.build, 'state.json')):
            print('no build in %s: run scripts/setup.py --target linux first (BUILDING_LINUX.md)' % a.build)
            return 1
    else:
        a.build = os.path.abspath(a.build or build_location.get())   # the game runs in build\work: a relative --build would point elsewhere there
        if a.play:
            a.userdata = a.userdata or os.path.join(a.build, 'saves')
            if not (os.path.isfile(os.path.join(a.build, 'work', 'ssbu_native.exe')) and os.path.isfile(os.path.join(a.build, 'state.json'))):
                print('The game is not built yet (looked in %s): double-click "Install SSBU.bat" first.' % a.build)
                return 1
            gone = [f for f in ('ssbu_game_eh_o1.dll', 'SDL3.dll', 'ssbu_glslang.dll') if not os.path.isfile(os.path.join(a.build, 'work', f))]
            if gone:
                print('The game files are incomplete: %s missing in %s.\n'
                      'Usually an antivirus removed it (Windows Security > Virus & threat protection > Protection history: restore it\n'
                      'and allow it), or the installation stopped before the end. Double-click "Install SSBU.bat" and press Resume:\n'
                      'it makes the missing files again in a few minutes.' % (', '.join(gone), os.path.join(a.build, 'work')))
                return 1
            if not a.log:                             # the game's output to a file, not the console: a console that keeps
                logs = os.path.join(a.build, 'logs')  # scrolling costs frames (testers, 2026-10-20); F1 > Log shows the file
                os.makedirs(logs, exist_ok=True)
                a.log = os.path.join(logs, time.strftime('game_%Y%m%d_%H%M%S.log'))
                old = sorted(n for n in os.listdir(logs) if n.startswith('game_') and n.endswith('.log'))
                for n in old[:-9]:                    # keep the last 10 game logs
                    try:
                        os.remove(os.path.join(logs, n))
                    except OSError:
                        pass
                print('Playing. The game log goes to %s (F1 > Log shows it). This window can stay minimised.' % a.log)
        if not a.userdata:
            ap.error('--userdata is required (or --play)')
    main_exe = 'ssbu_native' if (a.linux or a.mac) else 'ssbu_native.exe'
    a.exe = a.exe or main_exe
    mods_ok, mods_env = True, {}
    if a.play and not (a.linux or a.mac):
        # mods: Skyline plugins in the SD folder (sd\ beside build\, or SSBU_SD) are lifted, their hook sites recorded and the
        # game re-lifted with them before the start, when they are new or changed (scripts/mods.py, docs/SKYLINE.md); the exe
        # then loads ssbu_game_sky.dll by itself. Without plugins nothing happens here.
        import mods
        user_env = dict(kv.partition('=')[::2] for kv in a.env)
        mods_ok, mods_env = mods.prepare(a.build, sd=user_env.get('SSBU_SD'), exe=a.exe)
    state_file = os.path.join(a.build, 'state.json')
    with open(state_file) as f:
        state = json.load(f)
    work = os.path.join(a.build, 'work')
    rollback = False
    if a.play and a.exe == main_exe and not (a.linux or a.mac) and ini_on(os.path.join(a.build, 'config', 'graphics.ini'), 'rollback'):
        # F1 > Experimental > Rollback netcode: the normal exe and game DLL with SSBU_RB_NET (since R4 the ring saves the fight on
        # the normal build: runtime/rb_learned.c); the old pair ssbu_native_rb.exe + ssbu_game_r1.dll of an older install is not used
        rollback = True
        print('Rollback netcode on (the normal build; no second compile needed)')
    exe = os.path.join(work, a.exe)
    native = os.path.join(work, main_exe)
    if not os.path.exists(exe):
        shutil.copy2(native, exe)
    elif exe != native and os.path.getmtime(exe) < os.path.getmtime(native):
        # a copy made before the last link runs the old runtime; an exe linked on purpose with other sources
        # (RECOMP_EXE) is older too, so it is only replaced when asked
        if a.refresh:
            shutil.copy2(native, exe)
            print('%s was older than %s: copied again' % (a.exe, main_exe))
        else:
            print('note: %s is older than %s and may run an older runtime (--refresh copies it again)' % (a.exe, main_exe))
    # the window's icon and the loading animation's pictures (display.c / loading_anim.c read them beside the exe): newer
    # ones after an update are copied at once (the build copies them only at link)
    for name in ('opensmash.ico', 'loading_kong.png', 'loading_kong_back.png', 'loading_arm.png'):
        logo = os.path.join(REPO, 'media', name)
        if os.path.isfile(logo) and not (a.linux or a.mac):
            dst = os.path.join(work, name)
            try:
                if not os.path.isfile(dst) or open(dst, 'rb').read() != open(logo, 'rb').read():
                    shutil.copy2(logo, dst)
            except OSError:
                pass
    env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}
    romfs = state['dump']['romfs']
    if not os.path.isdir(romfs) and os.path.isdir(os.path.join(a.build, 'dump', 'romfs')):
        romfs = os.path.join(a.build, 'dump', 'romfs')       # the folder was moved: state.json still names the old place
    env['SSBU_ROMFS'] = romfs
    env['SSBU_USERDATA'] = os.path.abspath(a.userdata)
    if not (a.linux or a.mac) and not any(os.path.isfile(os.path.join(work, n)) for n in ('libopus-0.dll', 'opus.dll')):
        # libopus decodes the game's music and most sounds (testers' log 2026-10-20: "libopus NOT FOUND", no sound): the
        # copy install.py fetched into deps\opus is used when the one beside the exe is missing
        dep = os.path.join(REPO, 'deps', 'opus', 'libopus-0.dll')
        if os.path.isfile(dep):
            env['SSBU_OPUS_DLL'] = dep
        else:
            print('WARNING: libopus-0.dll is missing (beside the game and in deps\\opus): the music and most sounds will be silent.\n'
                  'Double-click "Install SSBU.bat" and press Resume: it downloads it again (an antivirus may have removed it).')
    for sub, var in (('fonts', 'SSBU_FONTS'), ('zoneinfo', 'SSBU_TZDIR')):
        p = os.path.join(a.build, 'sysdata', sub)
        if os.path.isdir(p):
            env[var] = p
    if a.seq:
        with open(a.seq) as f:
            env['SSBU_SEQ'] = ' '.join(l.strip() for l in f if l.strip() and not l.startswith('#'))
    if a.seed:
        env['SSBU_FIXED_SEED'] = a.seed
    if rollback and 'SSBU_RB_NET' not in env:
        env['SSBU_RB_NET'] = 'net'                    # rollback over the online connection (net_relay); needs online play on
    if a.mac and not (env.get('VK_DRIVER_FILES') or env.get('VK_ICD_FILENAMES')):
        # MoltenVK (the Vulkan driver on Metal) named for Homebrew's Vulkan loader, as play_mac.sh does: run.py started by hand
        icd = next((p for p in ('/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json', '/opt/homebrew/share/vulkan/icd.d/MoltenVK_icd.json',
                                '/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json', '/usr/local/share/vulkan/icd.d/MoltenVK_icd.json')
                    if os.path.isfile(p)), None)
        if icd:
            env['VK_DRIVER_FILES'] = icd
    env.update(mods_env)                              # SSBU_SD, and SSBU_SKYLINE=0 when the plugins could not be prepared
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v
    os.makedirs(a.userdata, exist_ok=True)
    os.makedirs(os.path.join(a.build, 'cache'), exist_ok=True)      # the runtime creates ..\cache\vk below it but not the folder itself (found 2026-10-13: with SSBU_VK_PREP=0 on a clean build nothing was saved)
    cmd = [exe, os.path.join(a.build, 'binaries', state.get('game_version', '13.0.4'), 'main.elf'), str(a.seconds)]
    t0 = time.time()
    out = open(a.log, 'wb') if a.log else None
    p = subprocess.run(cmd, cwd=work, env=env, stdout=out, stderr=subprocess.STDOUT if out else None)
    print('game exited with code %d after %.0f s' % (p.returncode, time.time() - t0))
    if mods_ok and mods_env and env.get('SSBU_SKYLINE') != '0':
        mods.after_run(a.build, t0)                   # this run's site list, for the next start (a plugin hooking something new)
    return p.returncode


if __name__ == '__main__':
    sys.exit(main())
