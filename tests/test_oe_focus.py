"""Exercise focus notifications while the guest waits instead of polling, using real runtime events.
Requires an existing game build (scripts/setup.py --dump <dump> --only link). No game session or GPU is started.
"""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    state = json.loads((ROOT / 'build/state.json').read_text())
    work = ROOT / 'build/work'
    os.environ.update(RECOMP_CLANG=state['tools']['clang'], SDL3_ROOT=state['tools']['sdl3'],
                      VULKAN_HEADERS=state['tools']['vulkan'])
    sys.path.insert(0, str(work))
    spec = importlib.util.spec_from_file_location('focus_build', work / 'build.py')
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    outdir = work / 'obj_host/oe_focus_test'
    outdir.mkdir(parents=True, exist_ok=True)
    testobj, bootobj = outdir / 'oe_focus.obj', outdir / 'boot.obj'
    for source, obj, extra in [(ROOT / 'tests/oe_focus_test.c', testobj, []),
                               (work / 'boot.c', bootobj, ['-Dmain=focus_unused_game_main'])]:
        result = subprocess.run([build.CLANG, '-c', *build.HOST_FLAGS, *extra, str(source), '-o', str(obj)],
                                capture_output=True, text=True, errors="replace")
        if result.returncode:
            print(result.stdout + result.stderr)
            return result.returncode
    network = []
    if (work / "build_network.py").is_file():
        import build_network
        _, network = build_network.prepare(build.CLANG, str(work))
    objdir = work / 'obj_host/ssbu_native'
    objects = [str(objdir / (Path(n).stem + '.obj')) for n in build.HOST_SOURCES + ['gamedll.c']
               if n not in ('boot.c', 'nnmisc.c', 'os_threads.c')]
    exe = work / 'oe_focus_test.exe'
    result = subprocess.run([build.CLANG, str(testobj), str(bootobj), *objects, *network,
                             str(work / 'ssbu_game_eh_o1.lib'), str(Path(build.SDL_DIR) / 'SDL3.lib'),
                             *build.LIBS, *build.debug_link_flags(str(exe)), '-o', str(exe)],
                            capture_output=True, text=True, errors="replace")
    if result.returncode:
        print(result.stdout + result.stderr)
        return result.returncode
    subprocess.run([str(exe)], cwd=work, check=True, timeout=20)
    subprocess.run([str(exe), '--guest-bootstrap'], cwd=work, check=True, timeout=20)
    subprocess.run([str(exe), '--disabled'], cwd=work, check=True, timeout=20)
    return 0


if __name__ == '__main__':
    sys.exit(main())
