"""Check packaged texture ownership and the real Vulkan prewarmer without a game, window or GPU.
Requires an existing generated game build (scripts/setup.py --dump <dump> --only link).
"""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    state = json.loads((ROOT / "build/state.json").read_text(encoding="utf-8"))
    work = ROOT / "build/work"
    os.environ.update(RECOMP_CLANG=state["tools"]["clang"], SDL3_ROOT=state["tools"]["sdl3"],
                      VULKAN_HEADERS=state["tools"]["vulkan"])
    spec = importlib.util.spec_from_file_location("texture_build", work / "build.py")
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    out = work / "obj_host/nvn_packaged_texture_test"
    out.mkdir(parents=True, exist_ok=True)
    flags = ["-I", str(ROOT / "runtime"), *build.HOST_FLAGS]

    def compile_source(source, obj, extra=()):
        result = subprocess.run([build.CLANG, "-c", *flags, *extra, str(source), "-o", str(obj)],
                                capture_output=True, text=True, errors="replace")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    testobj, bootobj = out / "nvn_gl.obj", out / "boot.obj"
    compile_source(ROOT / "tests/nvn_packaged_texture_test.c", testobj)
    compile_source(work / "boot.c", bootobj, ["-Dmain=texture_unused_game_main"])
    objdir = work / "obj_host/ssbu_native"
    objects = [str(objdir / (Path(n).stem + ".obj")) for n in build.HOST_SOURCES + ["gamedll.c"]
               if n not in ("boot.c", "nvn_gl.c", "nvn_vk.c")]
    # Simulate a stale header, without depending on a Git revision or a private build.
    # Both variants execute the actual Vulkan prewarmer; linking it alone cannot detect layout corruption.
    header = (ROOT / "runtime/nvn_front.h").read_text(encoding="utf-8")
    field = next(line for line in header.splitlines(keepends=True) if "uint8_t *packaged_copy;" in line)
    stale = out / "stale_nvn_front.h"
    stale.write_text(header.replace(field, "", 1), encoding="utf-8")
    executables = []
    for bad_layout in (True, False):
        vkobj = out / ("stale_nvn_vk.obj" if bad_layout else "nvn_vk.obj")
        compile_source(ROOT / "tests/nvn_texture_prewarm_probe.c", vkobj,
                       ["-include", str(stale)] if bad_layout else [])
        exe = out / ("stale_layout.exe" if bad_layout else "nvn_packaged_texture_test.exe")
        result = subprocess.run([build.CLANG, str(testobj), str(bootobj), str(vkobj), *objects,
                                 str(work / "ssbu_game_eh_o1.lib"), str(Path(build.SDL_DIR) / "SDL3.lib"),
                                 *build.LIBS, *build.debug_link_flags(str(exe)), "-o", str(exe)],
                                capture_output=True, text=True, errors="replace")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        executables.append(exe)
    env = dict(os.environ, SSBU_VK_TEXAHEAD="1", PATH=str(work) + os.pathsep + os.environ.get("PATH", ""))
    mismatch = subprocess.run([str(executables[0])], cwd=work, env=env, capture_output=True, timeout=20)
    (out / "stale-layout.log").write_bytes(mismatch.stdout + mismatch.stderr)
    assert mismatch.returncode == 86 and b"inconsistent Tex layout" in mismatch.stderr, (
        mismatch.returncode, mismatch.stdout, mismatch.stderr)
    print("PASS old Vulkan layout reproduces packaged-pointer overwrite in the actual prewarmer", flush=True)
    old = subprocess.run([str(executables[1]), "--reproduce-borrowed-source"], cwd=work, env=env,
                         capture_output=True, timeout=20)
    (out / "borrowed-source.log").write_bytes(old.stdout + old.stderr)
    assert old.returncode & 0xffffffff == 0xc0000005, (old.returncode, old.stdout, old.stderr)
    print("PASS borrowing a released source reproduces 0xc0000005 in the real texture hash path", flush=True)
    fixed = subprocess.run([str(executables[1])], cwd=work, env=env, capture_output=True, timeout=20)
    (out / "owned-source.log").write_bytes(fixed.stdout + fixed.stderr)
    print(fixed.stdout.decode("utf-8", "replace"), end="")
    print(fixed.stderr.decode("utf-8", "replace"), end="", file=sys.stderr)
    assert fixed.returncode == 0, fixed.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
