"""Exercise the actual Vulkan compute state machine using completed submissions and ordinary host mappings.
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
    spec = importlib.util.spec_from_file_location('retire_build', work / 'build.py')
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    outdir = work / 'obj_host/nvn_memory_retire_test'
    outdir.mkdir(parents=True, exist_ok=True)
    testobj, bootobj = outdir / 'nvn_vk.obj', outdir / 'boot.obj'
    for source, obj, extra in [(ROOT / 'tests/nvn_memory_retire_test.c', testobj, []),
                               (work / 'boot.c', bootobj, ['-Dmain=retire_unused_game_main'])]:
        result = subprocess.run([build.CLANG, '-c', *build.HOST_FLAGS, *extra, str(source), '-o', str(obj)],
                                capture_output=True, text=True, errors="replace")
        if result.returncode:
            print(result.stdout + result.stderr)
            return result.returncode
    objdir = work / 'obj_host/ssbu_native'
    objects = [str(objdir / (Path(n).stem + '.obj')) for n in build.HOST_SOURCES + ['gamedll.c']
               if n not in ('boot.c', 'nvn_vk.c')]
    exe = work / 'nvn_memory_retire_test.exe'
    result = subprocess.run([build.CLANG, str(testobj), str(bootobj), *objects,
                             str(work / 'ssbu_game_eh_o1.lib'), str(Path(build.SDL_DIR) / 'SDL3.lib'),
                             *build.LIBS, *build.debug_link_flags(str(exe)), '-o', str(exe)],
                            capture_output=True, text=True, errors="replace")
    if result.returncode:
        print(result.stdout + result.stderr)
        return result.returncode
    old = subprocess.run([str(exe), '--reproduce-old-lifetime'], cwd=work, capture_output=True, timeout=20)
    assert old.returncode & 0xffffffff == 0xc0000005, ('expected old lifetime access violation', old.returncode, old.stdout, old.stderr)
    print('PASS missing retirement reproduces access violation 0xc0000005 in the real copy-back path', flush=True)
    subprocess.run([str(exe)], cwd=work, check=True, timeout=20)
    return 0


if __name__ == '__main__':
    sys.exit(main())
