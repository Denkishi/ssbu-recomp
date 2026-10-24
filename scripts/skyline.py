#!/usr/bin/env python3
"""The Skyline plugin steps of docs/SKYLINE.md, run in a finished build with the same environment scripts/setup.py gives
its own steps (compiler, target, library names, tool folders from build/state.json), on Windows, Linux and macOS.

    python scripts/skyline.py [--target linux|macos] [--build <folder>] plugins --sd <sd folder> [build_plugins.py options]
    python scripts/skyline.py [--target ...] [--build ...] relift [--jobs=N] [relift_sites.py options]
    python scripts/skyline.py [--target ...] [--build ...] link
    python scripts/skyline.py [--target ...] [--build ...] play --sd <sd folder> [run.py options]

  plugins  step 1: tools/nro/build_plugins.py in build/work/ro_study: the plugins lifted into plugin_bin/<name>.dll / .so
  (play)   step 2 is a run of the normal exe with the plugins into a match: python scripts/run.py --env SSBU_SD=<sd> ...
           (--linux / --mac for those builds); it writes build/work/skyline_sites.log
  relift   step 3: tools/lifter/relift_sites.py build skyline_sites.log, for the game and for --module=common
  link     step 4: build.py link with RECOMP_DLL=ssbu_game_sky.<dll|so|dylib> and RECOMP_EXE=ssbu_sky(.exe)
  play     step 5: scripts/run.py with ssbu_sky, the plugins (SSBU_SD), a window and sound (SSBU_SHOW, SSBU_VOLUME)

--target and --build as for scripts/setup.py (the default build folder follows from --target). The build must be finished
(setup.py's link step done): this script only adds to it. Run setup.py once after updating the sources so the tools in
build/work are the current ones (its stage step copies them; nothing else is rebuilt then).
"""
import argparse
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'scripts'))
import setup  # noqa: E402


class Args(argparse.Namespace):
    """setup.py's options as its deps step reads them: every one not given here is unset (None)"""
    def __getattr__(self, name):
        return None


def context(a):
    """setup.py's context for the build folder, with the tools found the way setup.py finds them: a Windows build keeps
    them in build/state.json; a Linux / macOS build looks them up on every run (setup.py's deps step, without the dump)"""
    ns = Args(target=a.target, build=a.build, jobs=None, nocrt=False, runtime=None, no_inline=False, tools_only=True)
    cx = setup.Ctx(ns)
    if not cx.state.get('done', {}).get('link'):
        raise SystemExit('%s: no finished build (scripts/setup.py%s --dump <folder> first)' % (cx.build, ' --target ' + a.target if a.target != 'windows' else ''))
    cx.tools = dict(cx.state.get('tools', {}))
    cx.nocrt = bool(cx.state.get('nocrt')) or 'RECOMP_NOCRT' in os.environ
    if cx.posix and 'clang' not in cx.tools:
        try:
            setup.step_deps(cx)
        except setup.Fail as e:
            raise SystemExit('the build tools were not found: %s' % e)
    return cx


def run(argv, cwd, env):
    print('$ (in %s) %s' % (cwd, ' '.join(argv)), flush=True)
    return subprocess.run(argv, cwd=cwd, env=env).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--target', choices=('windows', 'linux', 'macos'), default='windows')
    ap.add_argument('--build')
    ap.add_argument('step', choices=('plugins', 'relift', 'link', 'play'))
    a, rest = ap.parse_known_args()
    cx = context(a)
    env = setup.build_env(cx)
    ext = setup.lib_ext(cx)
    sky_exe = 'ssbu_sky' if cx.posix else 'ssbu_sky.exe'
    py = sys.executable
    if a.step == 'plugins':
        return run([py, 'build_plugins.py'] + rest, os.path.join(cx.work, 'ro_study'), env)
    if a.step == 'relift':
        log = os.path.join(cx.work, 'skyline_sites.log')
        if not os.path.exists(log):
            raise SystemExit('%s is missing: play into a match once with the plugins first (docs/SKYLINE.md, step 2)' % log)
        for extra in ([], ['--module=common']):
            rc = run([py, 'relift_sites.py', 'build', log] + extra + rest, cx.work, env)
            if rc:
                return rc
        return 0
    if a.step == 'link':
        return run([py, 'build.py', 'link'], cx.work, dict(env, RECOMP_DLL='ssbu_game_sky' + ext, RECOMP_EXE=sky_exe))
    argv = [py, os.path.join(REPO, 'scripts', 'run.py'), '--build', cx.build, '--exe', sky_exe, '--env', 'SSBU_SHOW=1', '--env', 'SSBU_VOLUME=100']
    argv += ['--linux'] if cx.linux else ['--mac'] if cx.mac else []
    if '--sd' in rest:
        i = rest.index('--sd')
        argv += ['--env', 'SSBU_SD=' + os.path.abspath(rest[i + 1])]
        rest = rest[:i] + rest[i + 2:]
    return run(argv + rest, REPO, None)


if __name__ == '__main__':
    sys.exit(main())
