#!/usr/bin/env python3
"""Mods for the built game (Skyline plugins), prepared on their own when the game starts.

Install a mod as on a console: unpack its release into the SD folder, `sd\\` beside `build\\` (or the folder SSBU_SD names):

    sd\\atmosphere\\contents\\01006A800016E000\\romfs\\skyline\\plugins\\*.nro     the plugins (also sd\\ultimate\\mods\\<mod>\\plugin.nro)
    sd\\ultimate\\...                                                          the mod's own files

and start the game as always ("Play SSBU.bat"). Before the game starts, scripts/run.py --play calls prepare() here, which:

  1. lifts every plugin into build\\work\\plugin_bin (ro_study\\build_plugins.py) when one is new or changed;
  2. records where the plugins hook or patch the game: the game boots once without a window, with the normal game DLL,
     until its common fighter module is loaded (SSBU_SEQ "@module:lua2cpp_common 60 @exit"), and the site list the
     runtime writes (build\\work\\skyline_sites.log, runtime/skyline.c) is kept as plugin_bin\\sites.log;
  3. re-lifts the parts of the game those sites touch with the sites compiled in (relift_sites.py: ssbu_game_sky.dll,
     nro_bin_sky\\<module>.dll).

The exe loads ssbu_game_sky.dll and nro_bin_sky\\ by itself when the plugin folder exists (runtime/gamedll.c, nnro.c);
without plugins nothing here runs and the normal game starts. plugin_bin\\mods_state.json remembers what was prepared.
After a game with plugins, run.py keeps that run's site list as plugin_bin\\sites_last.log: sites not prepared yet (a
plugin that hooks something new in a mode played for the first time) are added to sites.log and step 3 runs again at the
next start. The first time takes a few minutes (step 3 compiles about 15 files of the game); nothing happens when nothing
changed. When a step fails, the game starts WITHOUT the plugins (SSBU_SKYLINE=0) and the reason is printed; the details
are in build\\logs\\mods.log.

    python scripts/mods.py [--build <folder>] [--sd <folder>] [--exe <name>] [--force] [--check]

--check reports what would be done and does nothing; --force lifts and re-lifts again.
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PLUGINS_DIR = os.path.join('atmosphere', 'contents', '01006A800016E000', 'romfs', 'skyline', 'plugins')
RECORD_SEQ = '@module:lua2cpp_common 60 @exit'      # boot until the common fighter module is loaded (the plugins hook it as it
RECORD_SECONDS = 300                                 # loads), a second more, then end; the time limit is only a safety net
STATE_NAME = 'mods_state.json'


class Failed(Exception):
    pass


def sd_default(build):
    """The runtime's default SD folder: <exe>\\..\\..\\sd, i.e. sd\\ beside build\\ (skyline.c)."""
    return os.path.normpath(os.path.join(os.path.abspath(build), os.pardir, 'sd'))


def plugin_files(sd):
    """The plugin NROs of an SD folder (the same two places tools/nro/plugin_map.py find_plugins looks in)."""
    found = sorted(glob.glob(os.path.join(sd, PLUGINS_DIR, '*.nro')))
    found += sorted(glob.glob(os.path.join(sd, 'ultimate', 'mods', '*', 'plugin.nro')))
    return found


def fingerprint(files, sd):
    """SHA-256 over the plugins' relative names and contents: what plugin_bin was lifted from."""
    h = hashlib.sha256()
    for f in files:
        h.update(os.path.relpath(f, sd).replace('\\', '/').encode('utf-8') + b'\0')
        with open(f, 'rb') as fh:
            for chunk in iter(lambda: fh.read(1 << 20), b''):
                h.update(chunk)
        h.update(b'\0')
    return h.hexdigest()


SITE_KINDS = ('patch', 'inline', 'midhook', 'dynimm', 'jtable', 'noinline', 'hook')
CODE_LIMIT = 0x7300000000                            # main and the script modules lie below; host placeholders above


def site_lines(path):
    """The sites of a site list (skyline_sites.log / sites.log) that a re-lift compiles in, with the comments stripped:
    {'kind addr [words]'}. The runtime's notes behind # (active / NOT ACTIVE, which plugin) are not part of a site. A
    plugin's writes into data (`data` lines) and hooks on host functions (addresses handed out at run time, which differ
    between runs) need no re-lift and are left out, so they cannot ask for one at every start."""
    out = set()
    try:
        f = open(path, encoding='utf-8', errors='replace')
    except OSError:
        return out
    with f:
        for line in f:
            t = line.split('#', 1)[0].split()
            if len(t) < 2 or t[0] not in SITE_KINDS:
                continue
            try:
                if int(t[1], 16) >= CODE_LIMIT:
                    continue
            except ValueError:
                continue
            out.add(' '.join(t))
    return out


def sites_digest(path):
    return set_digest(site_lines(path))


def set_digest(lines):
    return hashlib.sha256('\n'.join(sorted(lines)).encode('utf-8')).hexdigest()


def modules_hooked(path):
    """The script modules the site list names (`lua2cpp_common+0x...` in the notes): relift_sites.py --module names."""
    names = set()
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                site, _, note = line.partition('#')
                if site.split()[:1] and site.split()[0] in SITE_KINDS:            # not a data write, not the "; from <caller>" note
                    for m in re.finditer(r'\blua2cpp_(\w+)\+0x', note.split(';', 1)[0]):
                        names.add(m.group(1))
    except OSError:
        pass
    return sorted(names)


def relifted_dlls(work, modules):
    """{name: [size, mtime]} of the re-lifted DLLs (what a successful preparation records in the state)."""
    out = {}
    for n in ['ssbu_game_sky.dll'] + [os.path.join('nro_bin_sky', 'lua2cpp_%s.dll' % m) for m in modules]:
        try:
            st = os.stat(os.path.join(work, n))
            out[n] = [st.st_size, int(st.st_mtime)]
        except OSError:
            out[n] = None
    return out


def stale(work, done):
    """True when a re-lifted DLL is missing, is not the one the last successful preparation recorded (an interrupted
    re-lift left a truncated one), or is older than the base DLL it was made beside: an update or "Resume" of the
    installer relinked ssbu_game_eh_o1.dll (a new shim, new data objects, a new game-data interface), and the exe would
    refuse the old re-lift ("built for another version of this exe")."""
    modules = done.get('modules', [])
    have = relifted_dlls(work, modules)
    if any(v is None for v in have.values()):
        return True
    if done.get('dlls') is not None and done.get('dlls') != have:
        return True
    pairs = [(os.path.join(work, 'ssbu_game_sky.dll'), os.path.join(work, 'ssbu_game_eh_o1.dll'))]
    pairs += [(os.path.join(work, 'nro_bin_sky', 'lua2cpp_%s.dll' % m), os.path.join(work, 'nro_bin_eh_o2', 'lua2cpp_%s.dll' % m)) for m in modules]
    for sky, base in pairs:
        if os.path.isfile(base) and os.path.getmtime(sky) < os.path.getmtime(base):
            return True
    return False


def file_stamps(work, names):
    """{name: [size, mtime]} of files of the build (None when missing): what a failure memo is keyed on besides the
    plugins and the sites, so a repaired or updated build is tried again."""
    out = {}
    for n in names:
        try:
            st = os.stat(os.path.join(work, n))
            out[n] = [st.st_size, int(st.st_mtime)]
        except OSError:
            out[n] = None
    return out


def save_state(path, done):
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(done, f, indent=1)


def load_state(path):
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def tools_env(build, state):
    """The environment the lifter tools expect (as scripts/setup.py gives it): the build's clang, the game binaries, and
    RECOMP_NOCRT for a build made with LLVM alone (a runtime package: no MSVC), whose DLLs lld-link links."""
    env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}
    env['PYTHONIOENCODING'] = 'utf-8'
    env['PYTHONUNBUFFERED'] = '1'
    clang = (state.get('tools') or {}).get('clang')
    if clang:
        env['RECOMP_CLANG'] = clang
    env['RECOMP_BIN'] = os.path.join(build, 'binaries', state.get('game_version', '13.0.4'))
    if state.get('mode') == 'package' or os.path.isdir(os.path.join(build, 'work', 'obj_nocrt')):
        env['RECOMP_NOCRT'] = '1'
    return env


def run_logged(cmd, cwd, env, log_path):
    """Run cmd with its output appended to log_path; the exit code."""
    with open(log_path, 'ab') as log:
        log.write(('\n=== %s  (in %s)\n' % (' '.join(cmd), cwd)).encode('utf-8'))
        log.flush()
        r = subprocess.run(cmd, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
    return r.returncode


def prepare(build, sd=None, exe='ssbu_native.exe', force=False, check=False, say=print):
    """Prepare the plugins of the SD folder for the game of <build>. Returns (ok, env): environment variables for the
    game: SSBU_SD (the folder used), and SSBU_SKYLINE=0 when the plugins could not be prepared (the game runs without them).
    ok is True when there is nothing to do or everything was done."""
    build = os.path.abspath(build)
    work = os.path.join(build, 'work')
    sd = os.path.abspath(sd) if sd else sd_default(build)
    env = {'SSBU_SD': sd}
    files = plugin_files(sd)
    if not files:
        return True, env                                     # no plugins: the runtime finds none and the normal game runs
    state = load_state(os.path.join(build, 'state.json'))
    bindir = os.path.join(work, 'plugin_bin')
    logs = os.path.join(build, 'logs')
    state_path, sites, last = (os.path.join(bindir, n) for n in (STATE_NAME, 'sites.log', 'sites_last.log'))
    log_path = os.path.join(logs, 'mods.log')
    names = [os.path.relpath(f, sd) for f in files]
    tools = [os.path.join(work, 'ro_study', 'build_plugins.py'), os.path.join(work, 'relift_sites.py'), os.path.join(work, 'build.py'),
             os.path.join(work, 'obj_eh_o1'), os.path.join(work, 'gen_eh_o2', 'table.c'), os.path.join(work, 'ssbu_game_eh_o1.dll')]
    missing = [t for t in tools if not os.path.exists(t)]
    done = load_state(state_path)
    if os.path.isfile(sites + '.bak'):                       # a preparation ended between a merge and its end: the last good list
        try:
            shutil.move(sites + '.bak', sites)
        except OSError:
            pass
    fp = fingerprint(files, sd)
    lift = force or done.get('plugins') != fp or not os.path.isfile(os.path.join(bindir, 'plugins.txt'))
    relift = lift or done.get('sites') != sites_digest(sites) or stale(work, done)
    build_stamps = file_stamps(work, ['ssbu_game_eh_o1.dll', 'relift_sites.py', 'build.py', os.path.join('ro_study', 'build_plugins.py')])
    added, ignored = set(), set()
    if not lift and os.path.isfile(last):
        added = site_lines(last) - site_lines(sites)
        if added and not force and done.get('bad_added') == [fp, build_stamps, set_digest(added)]:
            ignored, added = added, set()                     # these could not be compiled in before: left out until something changes
        if added:
            relift = True
    # what this start would prepare; a failure is remembered so the same attempt is not repeated at every start
    attempt = {'plugins': fp, 'sites': sites_digest(sites), 'added': set_digest(added), 'lift': lift, 'build': build_stamps,
               'dlls': relifted_dlls(work, done.get('modules', []))}

    def usable():                                            # the last successful preparation still fits (checked again after a failure:
        return not lift and done.get('sites') == sites_digest(sites) and not stale(work, done)      # a re-lift may have replaced a DLL)
    if check:
        say('SD folder %s: %d plugin(s): %s' % (sd, len(files), ', '.join(names)))
        say('prepared: %s' % (('%s (%d sites, %s)' % (done.get('prepared'), done.get('site_count', 0), ', '.join(done.get('names', [])))) if done else 'never'))
        say('to do: %s' % ('lift the plugins, record their sites and re-lift the game' if lift else
                           'add %d new site(s) of the last game and re-lift the game' % len(added) if added else
                           're-lift the game' if relift else 'nothing'))
        if (lift or relift) and done.get('failed') == attempt:
            say('but not tried: it failed the last time (%s) and nothing changed since; the game %s (mods.py --force tries again)'
                % (done.get('failed_why', '?'), 'runs with the mods as prepared before' if usable() else 'starts without the plugins'))
        if missing:
            say('cannot: the build lacks %s' % ', '.join(missing))
        return not missing, env
    if not lift and not relift:
        return True, env
    say('Mods: %d plugin(s) in %s: %s' % (len(files), sd, ', '.join(names)))
    if not force and done.get('failed') == attempt:
        say('Preparing them failed the last time (%s) and nothing changed since: not tried again. Details: %s\n'
            'To try again: python scripts\\mods.py --force (or delete %s).' % (done.get('failed_why', '?'), log_path, state_path))
        if usable():
            say('The game runs with the mods as prepared before.')
            return True, env
        say('The game starts without the plugins.')
        env['SSBU_SKYLINE'] = '0'
        return False, env
    if missing:
        say('They cannot be prepared: this build lacks %s.\nThe game starts without them.' % ', '.join(missing))
        env['SSBU_SKYLINE'] = '0'
        return False, env
    say('Preparing them for this build (%s). This takes a few minutes the first time; the game starts when it is done.\n'
        'Details: %s' % ('lifting the plugins, recording where they hook the game, re-lifting those parts of the game'
                         if lift else 're-lifting the parts of the game the plugins change', log_path))
    os.makedirs(logs, exist_ok=True)
    os.makedirs(bindir, exist_ok=True)
    tenv = tools_env(build, state)
    t0 = time.time()
    try:
        if lift:
            say('  1/3  lifting the plugins ...')
            rc = run_logged([sys.executable, os.path.join(work, 'ro_study', 'build_plugins.py'), '--sd', sd], os.path.join(work, 'ro_study'), tenv, log_path)
            if rc or not os.path.isfile(os.path.join(bindir, 'plugins.txt')):
                raise Failed('the plugins could not be lifted (exit %d)' % rc)
            say('  2/3  recording where the plugins hook the game (a short boot without a window) ...')
            sites_log = os.path.join(work, 'skyline_sites.log')
            if os.path.exists(sites_log):
                os.remove(sites_log)
            os.makedirs(os.path.join(sd, PLUGINS_DIR), exist_ok=True)      # the runtime needs the folder even for plugins under ultimate\mods
            userdata = os.path.join(build, 'mods_record')      # a save folder of its own: the saves of the game are not touched
            saves = os.path.join(build, 'saves')
            if not os.path.isdir(userdata) and os.path.isdir(saves):
                shutil.copytree(saves, userdata)
            rec_log = os.path.join(logs, 'mods_record.log')
            cmd = [sys.executable, os.path.join(REPO, 'scripts', 'run.py'), '--build', build, '--userdata', userdata, '--exe', exe,
                   '--seconds', str(RECORD_SECONDS), '--log', rec_log]
            for kv in ('SSBU_SD=' + sd, 'SSBU_SKY_RELIFT=0', 'SSBU_SEQ=' + RECORD_SEQ, 'SSBU_SHOW=0', 'SSBU_VOLUME=0'):
                cmd += ['--env', kv]
            rc = run_logged(cmd, REPO, {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}, log_path)
            if not site_lines(sites_log):
                raise Failed('the recording boot wrote no site list (game exit code %d): see %s' % (rc, rec_log))
            shutil.copy2(sites_log, sites)
            if os.path.exists(last):
                os.remove(last)
            added = set()
        if added:
            shutil.copy2(sites, sites + '.bak')                     # the list of the last successful preparation
            with open(last, encoding='utf-8', errors='replace') as f:
                new_lines = [l.rstrip('\r\n') for l in f if ' '.join(l.split('#', 1)[0].split()) in added]
            with open(sites, 'a', encoding='utf-8') as f:
                f.write('# sites of a later game, added by scripts/mods.py %s\n' % time.strftime('%Y-%m-%d %H:%M'))
                f.writelines(l + '\n' for l in new_lines)
            say('  the last game hooked %d place(s) not prepared yet: added to the site list' % len(added))
        modules = modules_hooked(sites)
        say('  3/3  re-lifting the game with the plugins\' sites compiled in (main%s) ...' % ''.join(', ' + m for m in modules))
        relift_py = os.path.join(work, 'relift_sites.py')
        rc = run_logged([sys.executable, relift_py, 'build', sites], work, tenv, log_path)
        if rc or not os.path.isfile(os.path.join(work, 'ssbu_game_sky.dll')):
            raise Failed('the re-lift of the game failed (exit %d)' % rc)
        for m in modules:
            rc = run_logged([sys.executable, relift_py, 'build', sites, '--module=' + m], work, tenv, log_path)
            if rc:
                raise Failed('the re-lift of the %s module failed (exit %d)' % (m, rc))
        bad = done.get('bad_added')                          # kept while the plugins and the build are the same: the next game writes
        if force or not (isinstance(bad, list) and len(bad) == 3) or bad[0] != fp or bad[1] != build_stamps:      # those sites again (--force tries them again)
            bad = None
        done = {'plugins': fp, 'names': names, 'sites': sites_digest(sites), 'site_count': len(site_lines(sites)),
                'modules': modules, 'dlls': relifted_dlls(work, modules), 'prepared': time.strftime('%Y-%m-%d %H:%M')}
        if bad:
            done['bad_added'] = bad
        save_state(state_path, done)
        if ignored:
            say('  (%d site(s) of a later game that could not be compiled in before stay left out; mods.py --force tries them again)' % len(ignored))
        for p in (last, sites + '.bak'):                     # leftovers only: a failure here is no failure of the preparation
            try:
                if os.path.exists(p):
                    os.remove(p)
            except OSError as e:
                say('  (could not remove %s: %s)' % (p, e))
        say('Mods ready (%.0f s).' % (time.time() - t0))
        return True, env
    except (Failed, OSError, subprocess.SubprocessError, ValueError) as e:
        why = str(e) if isinstance(e, Failed) else '%s: %s' % (type(e).__name__, e)
        back = False
        try:
            if added and os.path.isfile(sites + '.bak') and not lift:
                shutil.move(sites + '.bak', sites)           # the site list of the earlier preparation
                if os.path.exists(last):
                    os.remove(last)
                back = True
            # the memo describes what is on disk NOW (the recording rewrote sites.log, a re-lift may have replaced a DLL):
            # the same start again is not tried; a change of the plugins, the sites, the DLLs or the tools is
            attempt['sites'] = sites_digest(sites)
            attempt['dlls'] = relifted_dlls(work, done.get('modules', []))
            done['failed'], done['failed_why'] = attempt, why
            if back:                                         # the added sites are left out from now on (the next game writes them again)
                done['bad_added'] = [fp, build_stamps, attempt['added']]
            save_state(state_path, done)
        except OSError as e2:
            why += '; and %s' % e2
        if back and usable():                                # the earlier preparation is still complete (false when the failed re-lift replaced a DLL)
            say('Mods: %s.\nThe %d site(s) the last game added could not be compiled in: the game runs with the mods as prepared '
                'before (those sites stay inactive). Details: %s' % (why, len(added), log_path))
            return True, env
        say('Mods NOT prepared: %s.\nThe game starts without the plugins. Details: %s' % (why, log_path))
        env['SSBU_SKYLINE'] = '0'
        return False, env


def after_run(build, started):
    """After a game with plugins: keep its site list (written by the runtime when the plugins loaded) for the next start,
    which re-lifts when it has sites that are not compiled in yet."""
    work = os.path.join(os.path.abspath(build), 'work')
    sites_log = os.path.join(work, 'skyline_sites.log')
    try:
        if os.path.getmtime(sites_log) >= started - 1 and site_lines(sites_log):
            shutil.copy2(sites_log, os.path.join(work, 'plugin_bin', 'sites_last.log'))
    except OSError:
        pass


def main():
    import build_location
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', default=None, help='build folder (default: the install location, else <repo>\\build)')
    ap.add_argument('--sd', default=os.environ.get('SSBU_SD'), help='the SD folder (default: sd\\ beside the build folder)')
    ap.add_argument('--exe', default='ssbu_native.exe', help='the exe of the recording boot (default ssbu_native.exe)')
    ap.add_argument('--force', action='store_true', help='lift the plugins and re-lift the game again')
    ap.add_argument('--check', action='store_true', help='say what would be done; do nothing')
    a = ap.parse_args()
    build = os.path.abspath(a.build or build_location.get())
    if not os.path.isfile(os.path.join(build, 'state.json')):
        print('no build in %s' % build)
        return 1
    ok, env = prepare(build, a.sd, a.exe, a.force, a.check)
    if not a.check:
        print('environment for the game: ' + ' '.join('%s=%s' % kv for kv in sorted(env.items())))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
