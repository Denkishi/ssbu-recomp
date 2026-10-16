"""Parallel build of the lifted game.

    python build.py compile [jobs] [--force]    gen/*.c -> obj/*.obj (only what changed)
    python build.py link                        obj/*.obj + host sources -> ssbu_native.exe
    python build.py nro <module> [...]          lift + compile + link one DLL per run-time module (see build_nro.py)
"""
import concurrent.futures
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# P3 (2026-10-09): the OPTIMISED set is the default: gen_eh_o2 (lifter with the fcvt-half and FMAX/FMIN fixes) -> obj_eh_o1
# (clang 9 -O1 through build_opt.py) -> ssbu_game_eh_o1.dll. The unoptimised set stays buildable under its own names:
#     RECOMP_GEN=gen_eh RECOMP_OBJ=obj_eh RECOMP_DLL=ssbu_game_eh.dll python build.py compile --opt=-O0   (and: ... python build.py link)
GEN = os.path.join(HERE, os.environ.get('RECOMP_GEN', 'gen_eh_o2'))      # side-by-side builds: RECOMP_GEN / RECOMP_OBJ / RECOMP_DLL / RECOMP_SHIM
OBJ = os.path.join(HERE, os.environ.get('RECOMP_OBJ', 'obj_eh_o1'))
CLANG = os.environ.get('RECOMP_CLANG', r'C:\llvm-8.0.0\bin\clang.exe')      # scripts\setup.py sets RECOMP_CLANG (LLVM/clang 9.0.0)
CFLAGS = ['-c', '-O1', '-w', '-fno-strict-aliasing', '-I', HERE, '-I', GEN]
HOST_SOURCES = ['hostrt.c', 'boot.c', 'nnsdk.c', 'libcxx.c', 'os_threads.c', 'nvn_stub.c', 'nvn_gl.c', 'nvn_vk.c', 'mx_translate.c', 'tex_decode.c', 'nnfs.c', 'clib.c', 'nntime.c', 'nnhid.c', 'nnpad_sdl.c','sdkdata.c', 'display.c', 'nnpl.c', 'nnfontll.c', 'nnmovie.c', 'nnmisc.c', 'nnaudio.c', 'nnro.c', 'nro_registry.c', 'nnmovie_mf.c', 'nniostream.c', 'nncxxrt.c', 'nncxxlib.c', 'nnlibc2.c',
                'sdklift_gen.c', 'sdklift_rt.c', 'nnsdk2.c', 'nnmii.c', 'nnapplet.c', 'nnalbum.c', 'nnnet.c', 'nnfsdir.c', 'nnunwind.c', 'nvn_sdk.c', 'nvn_c4.c', 'nvn_c5.c', 'hitch.c', 'gfx_options.c', 'debug_mode.c', 'r1_state.c']
# A handful of files hold functions so large that clang -O1 needs more than an hour on them;
# pass --opt=-O0 to compile whatever is still missing without optimization.
OPT = next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--opt=')), '-O1')
GEN_HEADERS = ['runtime.h', 'neon.h', 'fpexact.h']
SLOW_FILES = {'c_019e.c', 'c_00a3.c', 'c_013a.c', 'c_0099.c', 'c_0367.c', 'c_03f7.c', 'c_00b3.c', 'c_03d9.c', 'c_012f.c', 'c_00cf.c', 'c_040e.c', 'c_0180.c', 'c_04a5.c'}


def compile_one(name):
    src = os.path.join(GEN, name)
    obj = os.path.join(OBJ, name[:-2] + '.obj')
    t = time.time()
    # the dozen largest files hold functions clang 8 cannot optimise in reasonable time: keep them at -O0
    opt = '-O0' if name in SLOW_FILES else OPT
    flags = [opt if f == '-O1' else f for f in CFLAGS]
    r = subprocess.run([CLANG] + flags + [src, '-o', obj], capture_output=True, text=True)
    return name, r.returncode, r.stderr[-2000:], time.time() - t


def cmd_compile(jobs, force):
    os.makedirs(OBJ, exist_ok=True)
    sources = sorted(n for n in os.listdir(GEN) if n.endswith('.c'))
    # only the headers the generated code includes; hostrt.h belongs to the host sources, which cmd_link rebuilds when they are stale
    newest_header = max(os.path.getmtime(os.path.join(HERE, h)) for h in GEN_HEADERS)
    todo = []
    for n in sources:
        obj = os.path.join(OBJ, n[:-2] + '.obj')
        if force or not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(os.path.join(GEN, n)), newest_header):
            todo.append(n)
    wanted = set(n[:-2] + '.obj' for n in sources)
    for n in os.listdir(OBJ):
        if n.endswith('.obj') and n not in wanted:
            os.remove(os.path.join(OBJ, n))
    print('sources: %d   to compile: %d   jobs: %d' % (len(sources), len(todo), jobs), flush=True)
    t0 = time.time()
    failed, done, cpu = [], 0, 0.0
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for name, code, err, secs in pool.map(compile_one, todo):
            done += 1
            cpu += secs
            if code:
                failed.append(name)
                print('FAILED %s\n%s' % (name, err), flush=True)
            if done % 50 == 0 or done == len(todo):
                print('  %d/%d   %.0fs elapsed   %.0fs compiler time' % (done, len(todo), time.time() - t0, cpu), flush=True)
    print('compiled: %d   failed: %d   elapsed: %.0fs' % (len(todo) - len(failed), len(failed), time.time() - t0), flush=True)
    return 1 if failed else 0


# The lifted game code lives in ssbu_game.dll, linked only when obj\ changes; the exe holds just the
# host runtime, so the edit-link-run loop costs a couple of seconds (see game_shim.c, gamedll.c).
GAME_DLL = os.environ.get('RECOMP_DLL', 'ssbu_game_eh_o1.dll')
HOST_FLAGS = ['-O1', '-g', '-w', '-fno-strict-aliasing', '-I', HERE, '-I', GEN, '-Dfn_table=(*game_fn_table_p)', '-Dfn_table_count=(*game_fn_table_count_p)',
              '-Ddyn_syms=(*game_dyn_syms_p)', '-Ddyn_sym_count=(*game_dyn_sym_count_p)',
              '-I', os.path.join(os.environ.get('SDL3_ROOT', os.path.join(HERE, '..', 'reference', 'upstream', 'SDL3-3.4.18-VC', 'SDL3-3.4.18')), 'include'),
              '-I', os.path.join(os.environ.get('VULKAN_HEADERS', os.path.join(HERE, '..', 'reference', 'upstream', 'Vulkan-Headers')), 'include')]      # Khronos headers for nvn_vk.c (track V)
# RECOMP_LDN_ONLINE=1 (scripts\setup.py --ldn-online on): local wireless over a room server (ldn_online.inc) is compiled
# into the exe; otherwise the exe has none of that code. The two variants keep their host objects apart.
LDN_ONLINE = os.environ.get('RECOMP_LDN_ONLINE') == '1'
if LDN_ONLINE:
    HOST_FLAGS.append('-DSSBU_LDN_ONLINE')
SDL_DIR = os.path.join(os.environ.get('SDL3_ROOT', os.path.join(HERE, '..', 'reference', 'upstream', 'SDL3-3.4.18-VC', 'SDL3-3.4.18')), 'lib', 'x64')      # SDL3 for nnpad_sdl.c; SDL3.dll is copied beside the exe at link (SDL3_ROOT / VULKAN_HEADERS: scripts\setup.py)
LIBS = ['-luser32', '-lgdi32', '-lwinmm', '-lmfplat', '-lmfreadwrite', '-lmfuuid', '-lole32', '-lshlwapi']


def debug_link_flags(out):
    """The exe's debug information (-g in HOST_FLAGS) and linker map, beside the exe: <exe>.pdb lets WinDbg / Visual Studio
    name the host functions of a minidump (hostrt.c), <exe>.map the freeze watchdog's report (nvn_vk_wd.c). /OPT and
    /INCREMENTAL as link.exe has them without /DEBUG, so the code is the same as before. /PDBALTPATH: the exe holds the
    file name of its .pdb, not the full path (which names the Windows user). Every link line of the tree should use this."""
    base = os.path.splitext(out)[0]
    return ['-Wl,/DEBUG', '-Wl,/PDB:' + base + '.pdb', '-Wl,/PDBALTPATH:%_PDB%', '-Wl,/MAP:' + base + '.map', '-Wl,/OPT:REF', '-Wl,/OPT:ICF', '-Wl,/INCREMENTAL:NO']


def build_game_dll():
    dll = os.path.join(HERE, GAME_DLL)
    lib = dll[:-4] + '.lib'
    shim = os.path.join(HERE, os.environ.get('RECOMP_SHIM', 'game_shim_eh.c'))
    objs = sorted(os.path.join(OBJ, n) for n in os.listdir(OBJ) if n.endswith('.obj'))
    newest = max([os.path.getmtime(o) for o in objs] + [os.path.getmtime(shim)])
    if os.path.exists(dll) and os.path.exists(lib) and os.path.getmtime(dll) >= newest:
        return 0
    rsp = os.path.join(HERE, GAME_DLL + '.rsp')
    with open(rsp, 'w') as f:
        f.write('\n'.join('"%s"' % o.replace('\\', '/') for o in objs))
    t0 = time.time()
    r = subprocess.run([CLANG, '-shared', '-O1', '-w', '-I', HERE, '-I', GEN, shim, '@' + rsp, '-o', dll], capture_output=True, text=True)
    print((r.stdout + r.stderr)[-4000:])
    if r.returncode == 0:
        print('linked %s: %.0f MB in %.0fs' % (dll, os.path.getsize(dll) / 1048576.0, time.time() - t0))
    else:
        print('could not link %s (is a game exe still running and holding it open?)' % GAME_DLL)
    return r.returncode


def compile_host(job):
    src, obj = job
    r = subprocess.run([CLANG, '-c'] + HOST_FLAGS + [src, '-o', obj], capture_output=True, text=True)
    return src, r.returncode, (r.stdout + r.stderr)[-3000:]


def cmd_link():
    if build_game_dll():
        return 1
    exe = os.environ.get('RECOMP_EXE', 'ssbu_native.exe')      # parallel work: one exe name per worker
    out = os.path.join(HERE, exe)
    objdir = os.path.join(HERE, 'obj_host', os.path.splitext(exe)[0] + ('_ldn' if LDN_ONLINE else ''))
    os.makedirs(objdir, exist_ok=True)
    t0 = time.time()
    newest_dep = max([os.path.getmtime(os.path.join(HERE, h)) for h in os.listdir(HERE) if h.endswith(('.h', '.inc'))] + [os.path.getmtime(os.path.join(HERE, 'build.py'))])
    # nvn_vk.c includes the other nvn_vk*.c files as one translation unit: an edit of one of them must rebuild nvn_vk.obj
    newest_vk = max(os.path.getmtime(os.path.join(HERE, n)) for n in os.listdir(HERE) if n.startswith('nvn_vk') and n.endswith('.c'))
    objs, jobs = [], []
    for n in HOST_SOURCES + ['gamedll.c']:
        src, obj = os.path.join(HERE, n), os.path.join(objdir, n[:-2] + '.obj')
        objs.append(obj)
        if not os.path.exists(obj) or os.path.getmtime(obj) < max(os.path.getmtime(src), newest_dep, newest_vk if n == 'nvn_vk.c' else 0):
            jobs.append((src, obj))
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(16) as pool:
        for src, code, err in pool.map(compile_host, jobs):
            if code:
                failed += 1
                print('FAILED %s\n%s' % (os.path.basename(src), err))
    if failed:
        return 1
    r = subprocess.run([CLANG] + objs + [os.path.join(HERE, GAME_DLL[:-4] + '.lib'), os.path.join(SDL_DIR, 'SDL3.lib')] + LIBS + debug_link_flags(out) + ['-o', out], capture_output=True, text=True, errors='replace')
    if r.returncode == 0:
        import shutil
        shutil.copy2(os.path.join(SDL_DIR, 'SDL3.dll'), os.path.join(HERE, 'SDL3.dll'))
    print((r.stdout + r.stderr)[-4000:])                 # link.exe reports its errors on stdout
    if r.returncode == 0:
        print('linked %s: %.1f MB in %.1fs (%d host files recompiled)' % (out, os.path.getsize(out) / 1048576.0, time.time() - t0, len(jobs)))
    return r.returncode


if __name__ == '__main__':
    if sys.argv[1] == 'compile':
        args = [a for a in sys.argv[2:] if not a.startswith('--')]
        if not any(a.startswith('--opt=') for a in sys.argv) and '--plain' not in sys.argv:
            # the default: the optimised build (clang 9 -O1, functions over 20,000 statements unoptimised, 180 s per file
            # with automatic fallback; plan\phases\P3_performance.md section 4). --opt=<level> or --plain = this file's own compiler loop.
            import build_opt
            sys.exit(build_opt.cmd_main(int(args[0]) if args else 28))
        sys.exit(cmd_compile(int(args[0]) if args else 28, '--force' in sys.argv))
    if sys.argv[1] == 'nro':                 # run-time modules, one DLL each: python build.py nro <module> ... (build_nro.py)
        # default: the optimised modules, i.e. the folder nnro.c loads by default (build_opt.py: gen_nro_eh_o2, obj_nro_eh_o2,
        # nro_bin_eh_o2). --plain = build_nro.py's own -O0 loop into nro_bin_eh (run those with SSBU_NRO_BIN=nro_bin_eh).
        rest = [a for a in sys.argv[2:] if a != '--plain']
        if '--plain' not in sys.argv:
            import build_opt
            jobs = int(rest.pop(0)) if rest and rest[0].isdigit() else 28
            sys.exit(build_opt.cmd_nro(jobs, rest))
        import build_nro
        sys.exit(build_nro.main(rest))
    sys.exit(cmd_link())
