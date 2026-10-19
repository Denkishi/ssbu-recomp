"""Lift the Skyline plugins of an SD card layout into DLLs for the runtime (runtime/skyline.c).

    python build_plugins.py --sd <folder> [--jobs N] [--only name,...] [--relift] [--flags-local]
                            [--patches <folder>] [--no-auto-patches]

Run in build/work/ro_study (scripts/setup.py --only stage puts it there). For every plugin NRO (plugin_map.find_plugins):
  1 name and stable guest base: plugin_map.py, plugin_map.json in build/work/plugin_bin (bases from 0x7280000000)
  2 lift: recomp_nro.py full, with RECOMP_NRO_MODULE=<name> and the plugin's instruction patches and extra resume
    entries (RECOMP_PATCHES / RECOMP_EXTRA_RESUME = build/work/gen_plugin/<name>.patches.txt / .resume.txt): the ones
    plugin_map.auto_patches finds by the shape of the code (the emulator check, smashline's landing pad), plus
    <patches folder>/<name>.txt and <name>.resume.txt if you give --patches (same formats as recomp_nro.py's lists)
    -> build/work/gen_plugin/<name>
  3 only with --flags-local (the optional flags_local.py pass, when the game's sets are built with it):
    flags_local.py transform -> build/work/gen_plugin_fl/<name>
  4 compile: build_opt.py's compiler, level and flags (RECOMP_CLANG / RECOMP_OPT / RECOMP_CFLAGS, the same switches as
    the game's modules) and its fallback ladder (long functions unoptimised, time limit)
    -> build/work/obj_plugin[_fl]/<name>; nro_shim.c with -DNRO_MODULE=<name>
  5 link: build/work/plugin_bin/<name>.dll
Then plugin_bin/plugins.txt (one line per plugin: name base image_size file_size build_id source) and
plugin_bin/<name>.exports.txt.

--sd (default: SSBU_SD): the folder that stands for the SD card, with atmosphere\\contents\\01006A800016E000\\romfs\\
skyline\\plugins\\*.nro under it. At most --jobs (default 4) clang processes at a time; with RECOMP_HEAVY_LOCK set, the
compiles hold that lock file (other heavy builds that use it wait).
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))          # build/work/ro_study
WORK = os.path.dirname(HERE)                              # build/work
sys.path.insert(0, HERE)
sys.path.insert(0, WORK)
import plugin_map  # noqa: E402

LOCK = os.environ.get('RECOMP_HEAVY_LOCK')      # optional: a lock file shared with other heavy builds of the machine


def take_lock(what):
    while LOCK:
        try:
            fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, ('%s %s\n' % (what, time.strftime('%Y-%m-%d %H:%M:%S'))).encode())
            os.close(fd)
            return
        except FileExistsError:
            try:
                if time.time() - os.path.getmtime(LOCK) > 3 * 3600:
                    os.remove(LOCK)
                    continue
                holder = open(LOCK).read().strip()
            except OSError:
                holder = '?'
            print('waiting for %s (%s)' % (LOCK, holder), flush=True)
            time.sleep(30)


def drop_lock():
    if not LOCK:
        return
    try:
        os.remove(LOCK)
    except OSError:
        pass


def compile_one(job):
    """job = (src, obj, flags). Uses build_opt's compiler settings and ladder: long functions optnone, bounded time."""
    src, obj, flags = job
    import build_opt
    code, out, secs, note = build_opt.compile_file(src, obj, flags)
    return os.path.basename(src), code, out, secs, note


PATCH_DIR = None            # --patches: lists of your own, <name>.txt / <name>.resume.txt
AUTO_PATCHES = True         # --no-auto-patches turns plugin_map.auto_patches off


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    with open(path, 'w', newline='\n') as f:
        f.write(text)


def patch_lists(name, nro, gen):
    """The instruction patches and extra resume entries of one plugin, written beside its generated folder (only when they
    change, so an unchanged plugin is not lifted again). Returns the two paths."""
    patches, resume = plugin_map.auto_patches(nro) if AUTO_PATCHES else ([], [])
    plines = ['%x %08x %08x    # %s' % p for p in patches]
    rlines = ['%x    # smashline interpreter landing pad + 4' % r for r in resume]
    for note in [p[3] for p in patches[::2]] + ['extra resume entry at +%#x' % r for r in resume]:
        print('%s: %s' % (name, note))
    if PATCH_DIR:
        for suffix, lines in (('.txt', plines), ('.resume.txt', rlines)):
            own = os.path.join(PATCH_DIR, name + suffix)
            if os.path.exists(own):
                lines += open(own).read().splitlines()
                print('%s: + %s' % (name, own))
    out = []
    for suffix, lines, what in (('.patches.txt', plines, 'instruction patches'), ('.resume.txt', rlines, 'extra resume entries')):
        path = gen + suffix
        write_if_changed(path, ''.join(l + '\n' for l in ['# build_plugins.py: %s of %s (recomp_nro.py format)' % (what, os.path.basename(nro))] + lines))
        out.append(path)
    return out


def build_one(name, nro, base, jobs, relift, flags_local):
    import build_opt
    gen = os.path.join(WORK, 'gen_plugin', name)
    genfl = os.path.join(WORK, 'gen_plugin_fl', name) if flags_local else gen      # the files that are compiled
    obj = os.path.join(WORK, 'obj_plugin_fl' if flags_local else 'obj_plugin', name)
    bindir = os.path.join(WORK, 'plugin_bin')
    for d in (gen, genfl, obj, bindir):
        os.makedirs(d, exist_ok=True)
    t = {}
    # 1+2. lift
    t0 = time.time()
    env = dict(os.environ, RECOMP_NRO_MODULE=name, RECOMP_NRO=os.path.abspath(nro), RECOMP_NRO_BASE='%#x' % base)
    env['RECOMP_PATCHES'], env['RECOMP_EXTRA_RESUME'] = patch_lists(name, nro, gen)
    stamp = os.path.join(gen, 'table.c')
    deps = [nro, os.path.join(HERE, 'recomp_nro.py'), env['RECOMP_PATCHES'], env['RECOMP_EXTRA_RESUME']]
    if relift or not os.path.exists(stamp) or os.path.getmtime(stamp) < max(os.path.getmtime(p) for p in deps):
        r = subprocess.run([sys.executable, os.path.join(HERE, 'recomp_nro.py'), 'full', nro, gen, str(max(1, min(jobs, 8))), '%#x' % base],
                           capture_output=True, text=True, env=env, cwd=HERE)
        tail = [l for l in r.stdout.splitlines() if l.startswith('functions:')]
        if r.returncode or not tail:
            print('%s: LIFT FAILED\n%s\n%s' % (name, r.stdout[-3000:], r.stderr[-3000:]))
            return None
        print('%s: lifted  %s' % (name, tail[-1]), flush=True)
    if flags_local and (relift or not os.path.exists(os.path.join(genfl, 'table.c')) or os.path.getmtime(os.path.join(genfl, 'table.c')) < os.path.getmtime(stamp)):
        # 3. flags in locals (the transform rewrites only files whose result changed)
        r = subprocess.run([sys.executable, os.path.join(WORK, 'flags_local.py'), gen, genfl], capture_output=True, text=True, cwd=WORK)
        if r.returncode:
            print('%s: flags_local FAILED\n%s\n%s' % (name, r.stdout[-3000:], r.stderr[-3000:]))
            return None
    t['lift'] = time.time() - t0
    # 4. compile what changed
    t0 = time.time()
    sources = sorted(n for n in os.listdir(genfl) if n.endswith('.c'))
    flags = ['-w', '-fno-strict-aliasing', '-I', WORK, '-I', genfl]
    headers = [os.path.join(WORK, h) for h in ('runtime.h', 'neon.h', 'flags_local.h', 'fpexact.h') if os.path.exists(os.path.join(WORK, h))]
    newest_header = max(os.path.getmtime(h) for h in headers)
    todo = []
    for n in sources:
        src, o = os.path.join(genfl, n), os.path.join(obj, n[:-2] + '.obj')
        if not os.path.exists(o) or os.path.getmtime(o) < max(os.path.getmtime(src), newest_header):
            todo.append((src, o, flags))
    shim, shim_obj = os.path.join(WORK, 'nro_shim.c'), os.path.join(obj, '_shim.obj')
    if not os.path.exists(shim_obj) or os.path.getmtime(shim_obj) < max(os.path.getmtime(shim), newest_header):
        todo.append((shim, shim_obj, flags + ['-DNRO_MODULE=' + name]))
    wanted = set(n[:-2] + '.obj' for n in sources) | {'_shim.obj'}
    for n in os.listdir(obj):
        if n.endswith('.obj') and n not in wanted:
            os.remove(os.path.join(obj, n))
    failed = 0
    if todo:
        take_lock('build_plugins %s' % name)
        try:
            with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
                for label, code, out, secs, note in pool.map(compile_one, todo):
                    if code:
                        failed += 1
                        print('FAILED %s %s\n%s' % (name, label, out), flush=True)
                    elif note or secs > 60:
                        print('  %s %s: %.0fs %s' % (name, label, secs, note), flush=True)
        finally:
            drop_lock()
    if failed:
        return None
    t['compile'] = time.time() - t0
    # 5. link
    t0 = time.time()
    dll = os.path.join(bindir, name + '.dll')
    objs = sorted(os.path.join(obj, n) for n in os.listdir(obj) if n.endswith('.obj'))
    if not os.path.exists(dll) or os.path.getmtime(dll) < max(os.path.getmtime(o) for o in objs):
        rsp = os.path.join(obj, 'link.rsp')
        with open(rsp, 'w') as f:
            f.write('\n'.join('"%s"' % o.replace('\\', '/') for o in objs))
        r = subprocess.run([build_opt.CLANG, '-shared', '@' + rsp, '-o', dll], capture_output=True, text=True)
        if r.returncode:
            print((r.stdout + r.stderr)[-4000:])
            print('could not link %s (is a game exe still running and holding it open?)' % dll)
            return None
        for ext in ('.lib', '.exp'):
            p = dll[:-4] + ext
            if os.path.exists(p):
                os.remove(p)
    t['link'] = time.time() - t0
    print('%s: %d C files, %d compiled, %s %.1f MB   lift %.0fs  compile %.0fs  link %.0fs' % (
        name, len(sources), len(todo), os.path.basename(dll), os.path.getsize(dll) / 1048576.0, t['lift'], t['compile'], t['link']), flush=True)
    return dll


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sd', default=os.environ.get('SSBU_SD'), required=not os.environ.get('SSBU_SD'))
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--only', default='')
    ap.add_argument('--relift', action='store_true')
    ap.add_argument('--flags-local', action='store_true', help='apply flags_local.py (only when the game is built with that pass)')
    ap.add_argument('--patches', help='folder with instruction patch lists of your own: <name>.txt, <name>.resume.txt')
    ap.add_argument('--no-auto-patches', action='store_true', help='no patches found by the shape of the code (plugin_map.auto_patches)')
    a = ap.parse_args()
    global PATCH_DIR, AUTO_PATCHES
    PATCH_DIR, AUTO_PATCHES = a.patches, not a.no_auto_patches
    bindir = os.path.join(WORK, 'plugin_bin')
    os.makedirs(bindir, exist_ok=True)
    map_path = os.path.join(bindir, 'plugin_map.json')
    m = plugin_map.load_map(map_path)
    plugins = []
    for path in plugin_map.find_plugins(a.sd):
        name = plugin_map.module_name(path)
        image_size, _, _ = plugin_map.sizes(path)
        note = plugin_map.assign(m, name, image_size, path)
        if note:
            print(note)
        plugins.append((name, path))
    plugin_map.save_map(map_path, m)
    only = set(x for x in a.only.split(',') if x)
    ok = []
    for name, path in plugins:
        if only and name not in only:
            ok.append((name, path))
            continue
        if build_one(name, path, int(m[name]['base'], 16), a.jobs, a.relift, a.flags_local):
            ok.append((name, path))
            with open(os.path.join(bindir, name + '.exports.txt'), 'w') as f:
                for off, kind, size, sym in plugin_map.exports(path):
                    f.write('%#010x %-7s %#8x %s\n' % (off, kind, size, sym))
    with open(os.path.join(bindir, 'plugins.txt'), 'w') as f:
        f.write('# name base image_size file_size build_id source   (build_plugins.py; read by runtime/skyline.c)\n')
        for name, path in ok:
            if os.path.exists(os.path.join(bindir, name + '.dll')):
                f.write(plugin_map.registry_line(name, int(m[name]['base'], 16), path) + '\n')
    print('plugins.txt: %d plugins' % len(ok))
    return 0 if len(ok) == len(plugins) else 1


if __name__ == '__main__':
    sys.exit(main())
