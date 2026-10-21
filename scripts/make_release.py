#!/usr/bin/env python3
"""Build the prebuilt-runtime package of a GitHub Release: the parts of the port that are OUR code only, so a player's PC
needs neither the MSVC Build Tools nor the Windows SDK (scripts/setup.py --runtime <package>; scripts/install.py fetches it).

    python scripts/make_release.py [--out dist] [--build build_release] [--llvm ..] [--sdl3 ..] [--vulkan-headers ..]
                                   [--glslang ..] [--suyu ..] [--opus <opus.dll>] [--against <a build\\work of a dump>]

Needs no dump and reads nothing of the game: the exe is built from the runtime's own sources (tools/lifter/build.py host),
which since game_data.h hold nothing derived from the dump; the game DLL it loads is built on the player's PC.

The package, dist/ssbu-runtime-<commit>.zip, holds:
    ssbu_native.exe      the host runtime (runtime/*.c); it loads the game DLL by name at start (gamedll.c), no import library
    SDL3.dll             SDL 3 (zlib licence), as in the official VC development package
    ssbu_glslang.dll     runtime/vk_glslang_shim.c + the glslang / SPIRV-Tools release libraries
    opus.dll             only with --opus: libopus (BSD licence), for the music
    LICENSE-*.txt        the licences of the above
    manifest.json        SHA-256 and size of every file, the source commit, the tree digest setup.py compares with its own
                         tree (the package is used only by the tree it was built from), the game-data interface version

Before the zip is written the package is audited (audit_package): only the files above; the exe's linker map lists no object
but the runtime's own sources; the exe imports no game DLL (gamedll.c loads it by name at start); no file holds a path or
name of this PC or a lifted-function name. --against <build\\work> also checks that none of that build's dump-derived
tables (sdkdata.c byte blobs, nro_registry.c build ids) occurs in any file. The result is printed with the zip's SHA-256,
which goes into scripts/install.py (RUNTIME_RELEASE) once the zip is uploaded.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('ssbu_setup', os.path.join(REPO, 'scripts', 'setup.py'))
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)

EXE = 'ssbu_native.exe'
PACKAGE_FILES = [EXE, 'SDL3.dll', 'ssbu_glslang.dll']      # (R4: no rollback exe any more: rollback runs on the normal build)
OPTIONAL_FILES = ['opus.dll']
# what may be in the zip besides PACKAGE_FILES / OPTIONAL_FILES
PACKAGE_EXTRA = re.compile(r'^(manifest\.json|README\.txt|LICENSE-[A-Za-z0-9_.-]+\.txt)$')


def say(s=''):
    print(s, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def git(*args):
    r = subprocess.run(['git', '-C', REPO] + list(args), capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ''


# ---------------------------------------------------------------------------------------------- audit
def personal_patterns():
    """A path or name of the PC the package is built on: any user-profile path, and this PC's user, profile, computer,
    repository path and git identity, read from the environment (nothing personal is written in this file)."""
    pats = [rb'(?i)[a-z]:[\\/]+users[\\/]+(?!public[\\/]|default[\\/])[a-z0-9 ._~-]+[\\/]', rb'(?i)/c/users/[a-z]']
    words = [os.environ.get(v, '') for v in ('USERNAME', 'COMPUTERNAME')]
    words += [os.path.basename(os.environ.get('USERPROFILE', '')), os.path.basename(os.path.expanduser('~')), git('config', 'user.name'),
              git('config', 'user.email')]
    for w in words:                                   # each word of a name, and the whole name
        for part in [w] + w.split():
            if len(part) >= 4 and part.lower() not in ('users', 'user', 'admin', 'administrator', 'workgroup'):
                pats.append(b'(?i)' + re.escape(part.encode('utf-8', 'replace')))
    for whole in (REPO, os.path.dirname(REPO), os.environ.get('USERPROFILE', '')):
        if whole:
            pats.append(b'(?i)' + re.escape(whole.encode('utf-8', 'replace')).replace(b'\\\\\\\\', b'[\\\\/]'))
    if os.environ.get('USERPROFILE'):                 # the 8.3 name of the profile folder (XXXXXX~1), as temporary paths show it
        import ctypes
        buf = ctypes.create_unicode_buffer(1024)
        if ctypes.windll.kernel32.GetShortPathNameW(os.environ['USERPROFILE'], buf, 1024):
            short = os.path.basename(buf.value)
            if '~' in short:
                pats.append(b'(?i)' + re.escape(short.encode()))
    return sorted(set(pats))
# names of lifted functions: the runtime's sources name a few (self-tests, hooks); a name the sources do not hold is lifted code
LIFTED_NAME = rb'f_7[0-9a-f]{9}'
BANNERS = rb'generated by (?:recomp|sdklift|gen_sdkdata|gen_eh_tests|build_nro|gen_imports)'
MAP_SPECIAL = {'<absolute>', '<common>', '<linker-defined>'}
DUMP_OBJECTS = {'sdkdata.obj', 'nro_registry.obj', 'sdklift_gen.obj', 'game_data.obj'}


def strings_of(data, n=6):
    return re.findall(rb'[\x20-\x7e]{%d,}' % n, data)


def audit_package(folder, files, against=None, llvm=None):
    """Return a list of problems (empty = the package holds nothing of the game and nothing of this PC)."""
    bad = []
    for name in os.listdir(folder):
        if name not in files and not PACKAGE_EXTRA.match(name):
            bad.append('file not allowed in the package: %s' % name)
    for name in files:
        if not (name in PACKAGE_FILES or name in OPTIONAL_FILES or PACKAGE_EXTRA.match(name)):
            bad.append('file not allowed in the package: %s' % name)
    personal = personal_patterns()
    for name in sorted(os.listdir(folder)):
        data = open(os.path.join(folder, name), 'rb').read()
        for rx in personal:
            m = re.search(rx, data)
            if m:
                bad.append('%s: a path or name of this PC (%r at %#x)' % (name, m.group(0)[:60], m.start()))
        m = re.search(BANNERS, data)
        if m:
            bad.append('%s: text of a generated file (%r at %#x)' % (name, m.group(0), m.start()))
        unknown = sorted(n for n in set(re.findall(LIFTED_NAME, data)) if n.decode() not in source_names())
        if unknown:
            bad.append('%s: %d name(s) of lifted functions the runtime sources do not hold (%s)' % (name, len(unknown), b' '.join(unknown[:5]).decode()))
    return bad


_names = None


def source_names():
    """Every f_7xxxxxxxxx the tree's own sources write (comments, self-tests, hook tables)."""
    global _names
    if _names is None:
        _names = set()
        for top in ('runtime', 'fighter_sdk', 'mods'):
            for root, _, files in os.walk(os.path.join(REPO, top)):
                for f in files:
                    if f.endswith(('.c', '.h', '.inc', '.cpp', '.hpp')) and f not in setup.GENERATED_NAMES:
                        _names |= set(re.findall(r'f_7[0-9a-f]{9}', open(os.path.join(root, f), encoding='latin-1').read()))
    return _names


def audit_exe_build(work, exe, llvm):
    """The exe's own build: its linker map names only runtime objects, and it imports no game DLL (gamedll.c loads it by name)."""
    bad = []
    mp = os.path.join(work, os.path.splitext(exe)[0] + '.map')
    if not os.path.exists(mp):
        return ['no linker map %s: cannot list the objects of the exe' % mp], {}
    sys.path.insert(0, work)
    import build                                                          # the staged tools/lifter/build.py
    allowed = {n[:-2] + '.obj' for n in build.HOST_SOURCES + ['gamedll.c', 'menu_stub.c']}
    # the settings menu (C++): menu.cpp and Dear ImGui's own four files, objects named <name>_cxx.obj by build.py
    allowed |= {os.path.splitext(os.path.basename(s))[0] + '_cxx.obj' for s in getattr(build, 'CXX_SOURCES', [])}
    # "Publics by Value" lines of link.exe's map end with the object: "boot.obj" for an object of the link line,
    # "libcmt:xyz.obj" / "kernel32:KERNEL32.dll" for a member of a library
    owners = set()
    for line in open(mp, encoding='latin-1'):
        parts = line.split()
        if len(parts) >= 4 and re.match(r'^[0-9a-f]{4}:[0-9a-f]{8}$', parts[0]) and re.match(r'^[0-9a-f]{16}$', parts[2]):
            owners.add(parts[-1])
    direct = {o.lower() for o in owners if ':' not in o and o not in MAP_SPECIAL and o.lower() != os.path.splitext(exe)[0].lower() + '.exp'}
    libs = sorted({o.split(':')[0].lower() for o in owners if ':' in o})
    for o in sorted(direct - allowed):
        bad.append('the exe contains %s, which is not one of the runtime sources (build.py HOST_SOURCES)%s'
                   % (o, ': it is built from the dump' if o in DUMP_OBJECTS else ''))
    if len(direct & allowed) < len(allowed) * 0.9:
        bad.append('the map names %d of the %d runtime objects: not the map of this exe?' % (len(direct & allowed), len(allowed)))
    say('exe objects: %d runtime sources; libraries: %s' % (len(direct & allowed), ' '.join(libs)))
    out = subprocess.run([os.path.join(llvm, 'bin', 'llvm-objdump.exe'), '-p', os.path.join(work, exe)], capture_output=True, text=True).stdout
    imports, cur = {}, None
    for line in out.splitlines():
        m = re.match(r'\s+DLL Name: (\S+)', line)
        if m:
            cur = m.group(1).lower()
            imports[cur] = []
        elif cur and re.match(r'\s+\d+\s+\S+', line):
            imports[cur].append(line.split()[-1])
    game = [d for d in imports if d.startswith('ssbu_game')]
    if game:                                     # gamedll.c loads the game DLL by name at start (ssbu_game_sky.dll with plugins): no import
        bad.append('the exe should import no game DLL (gamedll.c loads it by name), it imports %s' % ' '.join(game))
    return bad, imports


def audit_against(folder, work):
    """None of a dump build's tables (sdkdata.c blobs of 16 bytes or more, nro_registry.c build ids) occurs in the package."""
    needles = []
    p = os.path.join(work, 'sdkdata.c')
    if os.path.exists(p):
        for m in re.finditer(r'static const unsigned char d\d+\[\] = \{([0-9,]+)\};', open(p).read()):
            b = bytes(int(x) for x in m.group(1).split(','))
            if len(b) >= 24 and len(set(b)) >= 6:      # not the unit vectors / ones any program holds (16-byte {1,0,0,0} seen)
                needles.append(('sdkdata.c blob', b))
    p = os.path.join(work, 'nro_registry.c')
    if os.path.exists(p):
        for m in re.finditer(r'\{"?\s*"(\w+)",[^{]*\{([0-9a-fx,]+)\}', open(p).read()):
            needles.append(('build id of ' + m.group(1), bytes(int(x, 16) for x in m.group(2).split(','))))
    bad = []
    for name in sorted(os.listdir(folder)):
        data = open(os.path.join(folder, name), 'rb').read()
        for what, b in needles:
            if b in data:
                bad.append('%s contains the %s of the dump build in %s' % (name, what, work))
    return bad, len(needles)


# ---------------------------------------------------------------------------------------------- build
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(REPO, 'dist'))
    ap.add_argument('--build', default=os.path.join(REPO, 'build_release'), help='scratch folder (default <repo>/build_release)')
    ap.add_argument('--llvm'), ap.add_argument('--sdl3'), ap.add_argument('--vulkan-headers'), ap.add_argument('--glslang'), ap.add_argument('--suyu')
    ap.add_argument('--opus', help='opus.dll to ship beside the exe (optional; BSD licence)')
    ap.add_argument('--imgui', help='Dear ImGui v1.91.9b folder (imgui.h, imgui.cpp): the settings menu (F1) is built into the exe '
                                    '(MIT licence, shipped as LICENSE-imgui.txt); default IMGUI_ROOT or <repo>/deps/imgui; without it no menu')
    ap.add_argument('--against', help='a build\\work folder made from a dump: check that none of its tables is in the package')
    ap.add_argument('--allow-dirty', action='store_true', help='package a tree with uncommitted changes (testing only)')
    a = ap.parse_args()
    t0 = time.time()
    commit = git('rev-parse', 'HEAD')
    dirty = bool(git('status', '--porcelain', '--untracked-files=no'))
    if dirty and not a.allow_dirty:
        sys.exit('the tree has uncommitted changes: a release is built from a commit (--allow-dirty to test)')

    # tools: the same places setup.py looks
    deps = os.path.join(REPO, 'deps')
    find = setup.find_first
    llvm = find([a.llvm, os.environ.get('LLVM_ROOT'), r'C:\llvm-9.0.0', os.path.join(deps, 'llvm')], lambda d: os.path.exists(os.path.join(d, 'bin', 'clang.exe')))
    tools = {}
    for key, opt, envv, sub, probe in (('sdl3', a.sdl3, 'SDL3_ROOT', 'SDL3', os.path.join('lib', 'x64', 'SDL3.lib')),
                                       ('vulkan', a.vulkan_headers, 'VULKAN_HEADERS', 'Vulkan-Headers', os.path.join('include', 'vulkan', 'vulkan.h')),
                                       ('glslang', a.glslang, 'GLSLANG_ROOT', 'glslang', os.path.join('lib', 'glslang.lib')),
                                       ('suyu', a.suyu, 'SUYU_ROOT', 'suyu', os.path.join('src', 'core', 'hle', 'service', 'mii', 'types', 'raw_data.cpp'))):
        tools[key] = find([opt, os.environ.get(envv), os.path.join(deps, sub)], lambda d, p=probe: os.path.exists(os.path.join(d, p)))
    imgui = find([a.imgui, os.environ.get('IMGUI_ROOT'), os.path.join(deps, 'imgui')],
                 lambda d: os.path.exists(os.path.join(d, 'imgui.h')) and os.path.exists(os.path.join(d, 'imgui.cpp')))
    if not imgui:
        say('WARNING: Dear ImGui not found (--imgui): the package exe has no settings menu (F1)')
    missing = [k for k, v in tools.items() if not v] + ([] if llvm else ['llvm'])
    if missing:
        sys.exit('not found: %s (same options as scripts/setup.py)' % ', '.join(missing))
    clang = os.path.join(llvm, 'bin', 'clang.exe')
    clang_ver = subprocess.run([clang, '--version'], capture_output=True, text=True).stdout.splitlines()[0]

    # 1. stage the sources exactly as setup.py does (one folder), nothing of the game
    build = os.path.abspath(a.build)
    work = os.path.join(build, 'work')
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    cx = type('Cx', (), {})()
    cx.work = work
    setup.step_stage(cx)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('RECOMP_', 'SSBU_'))}
    env.update(PYTHONIOENCODING='utf-8', RECOMP_CLANG=clang, SDL3_ROOT=tools['sdl3'], VULKAN_HEADERS=tools['vulkan'],
               GLSLANG_ROOT=tools['glslang'], SUYU_ROOT=tools['suyu'], RECOMP_EXE=EXE)
    env['IMGUI_ROOT'] = imgui or os.path.join(build, 'no-imgui')     # build.py would otherwise look beside the study tree
    os.environ['IMGUI_ROOT'] = env['IMGUI_ROOT']                     # the audit imports the staged build.py in this process
    # the movie fallback (mf_ffmpeg.c) is compiled with FFmpeg's headers; a package exe without it would leave players whose
    # Windows cannot play WebM on a black screen, so they are required (python scripts/fetch_media.py puts them in deps\ffmpeg)
    ffmpeg = find([os.environ.get('FFMPEG_ROOT'), os.path.join(deps, 'ffmpeg')],
                  lambda d: os.path.exists(os.path.join(d, 'include', 'libavcodec', 'avcodec.h')))
    if not ffmpeg:
        sys.exit('not found: FFmpeg headers (deps\\ffmpeg\\include): run python scripts/fetch_media.py, or set FFMPEG_ROOT')
    env['FFMPEG_ROOT'] = os.environ['FFMPEG_ROOT'] = ffmpeg

    def run(argv, what):
        r = subprocess.run(argv, cwd=work, env=env, capture_output=True, text=True, errors='replace')
        if r.returncode:
            sys.exit('%s failed (exit %d):\n%s' % (what, r.returncode, (r.stdout + r.stderr)[-3000:]))
        return r.stdout

    # 2. the host's own generated headers: none reads the dump
    run([sys.executable, 'gen_quad_tests.py'], 'gen_quad_tests.py')          # soft-float vectors, computed in Python
    run([sys.executable, 'gen_sdktest.py'], 'gen_sdktest.py')                # suyu's public Mii tables, zlib, Pillow
    with open(os.path.join(REPO, 'tools', 'lifter', 'sdklift_roots.txt'), encoding='utf-8', errors='replace') as f:
        with open(os.path.join(work, 'sdklift_install.h'), 'w', newline='\n') as g:
            g.write(setup.install_header(f.read()))                          # names of the tree's own list
    for name, var in (('fake_whitelist.h', 'audit_whitelist'), ('fake_openlist.h', 'audit_openlist')):
        with open(os.path.join(work, name), 'w', newline='\n') as f:
            f.write(setup.STUB_LIST % var)
    for gen_name in ('sdkdata.c', 'nro_registry.c', 'sdklift_gen.c', 'eh_tests.h', 'imports_all.txt'):
        if os.path.exists(os.path.join(work, gen_name)):
            sys.exit('%s is in the staged tree: it is derived from a dump and must not be (remove it from the repository)' % gen_name)

    # 3. the exe (build.py host: no game DLL needed), ssbu_glslang.dll
    say('building %s from the runtime sources ...' % EXE)
    out = run([sys.executable, 'build.py', 'host'], 'build.py host')
    say('  ' + (out.strip().splitlines() or [''])[-1])
    run([os.environ.get('COMSPEC', 'cmd.exe'), '/c', os.path.join(work, 'build_glslang.bat')], 'build_glslang.bat')
    for f in PACKAGE_FILES:
        if not os.path.exists(os.path.join(work, f)):
            sys.exit('%s was not produced' % f)

    # 4. the package folder
    pkg = os.path.join(build, 'package')
    if os.path.isdir(pkg):
        shutil.rmtree(pkg)
    os.makedirs(pkg)
    files = list(PACKAGE_FILES)
    for f in PACKAGE_FILES:
        shutil.copy2(os.path.join(work, f), os.path.join(pkg, f))
    if a.opus:
        shutil.copy2(a.opus, os.path.join(pkg, 'opus.dll'))
        files.append('opus.dll')
        lic = os.path.join(os.path.dirname(os.path.abspath(a.opus)), 'LICENSE-opus.txt')     # BSD: its notice goes with it
        if os.path.isfile(lic):
            shutil.copy2(lic, os.path.join(pkg, 'LICENSE-opus.txt'))
            files.append('LICENSE-opus.txt')
    licences = [(os.path.join(REPO, 'LICENSE'), 'LICENSE-ssbu-native.txt'),
                (next((os.path.join(tools['sdl3'], n) for n in ('LICENSE.txt', 'COPYING.txt') if os.path.exists(os.path.join(tools['sdl3'], n))), None), 'LICENSE-SDL3.txt'),
                (next((os.path.join(tools['glslang'], n) for n in ('LICENSE.txt', 'LICENSE') if os.path.exists(os.path.join(tools['glslang'], n))), None), 'LICENSE-glslang.txt')]
    if imgui and os.path.exists(os.path.join(imgui, 'LICENSE.txt')):
        licences.append((os.path.join(imgui, 'LICENSE.txt'), 'LICENSE-imgui.txt'))
    for src, dst in licences:
        if src:
            shutil.copyfile(src, os.path.join(pkg, dst))
        elif dst == 'LICENSE-glslang.txt':                 # the glslang release zip holds no licence file
            with open(os.path.join(pkg, dst), 'w', newline='\r\n') as f:
                f.write('ssbu_glslang.dll links the glslang and SPIRV-Tools libraries of the official glslang release\n'
                        '(https://github.com/KhronosGroup/glslang/releases). Their licences: glslang, mostly BSD-3-Clause and\n'
                        'other permissive licences, https://github.com/KhronosGroup/glslang/blob/main/LICENSE.txt ; SPIRV-Tools,\n'
                        'Apache-2.0, https://github.com/KhronosGroup/SPIRV-Tools/blob/main/LICENSE\n')
    with open(os.path.join(pkg, 'README.txt'), 'w', newline='\r\n') as f:
        f.write('Prebuilt runtime of ssbu-native (our own code only; nothing of the game is in it).\n'
                'scripts/install.py downloads and checks it; scripts/setup.py --runtime <this folder or zip> uses it, and builds\n'
                'the game DLLs from your own dump with LLVM alone. manifest.json lists the files and their SHA-256.\n')
    digest = setup.tree_digest()
    manifest = {
        'format': 1,
        'name': 'ssbu-native runtime',
        'source_commit': commit, 'source_dirty': dirty,
        'tree_digest': digest,                              # setup.py --runtime compares it with the digest of its own tree
        # since 2026-10-20 also file by file: setup.py then names what differs and accepts files that were only added
        'tree_files': setup.tree_files(),
        'game_data_version': setup.game_data_version(),
        'game_dll': 'ssbu_game_eh_o1.dll',
        'game_builds': sorted(setup.GAME_BUILDS.values()),
        'built': time.strftime('%Y-%m-%d %H:%M'),
        'toolchain': {'clang': clang_ver, 'linker': 'MSVC link.exe (through clang)'},
        'settings_menu': bool(imgui),          # Dear ImGui built in (F1)
        'files': {n: {'sha256': sha256(os.path.join(pkg, n)), 'size': os.path.getsize(os.path.join(pkg, n))}
                  for n in sorted(os.listdir(pkg))},
    }
    with open(os.path.join(pkg, 'manifest.json'), 'w', newline='\n') as f:
        json.dump(manifest, f, indent=1)

    # 5. audit
    bad = audit_package(pkg, files + [n for n in os.listdir(pkg) if PACKAGE_EXTRA.match(n)])
    more, imports = audit_exe_build(work, EXE, llvm)
    bad += more
    if a.against:
        more, n = audit_against(pkg, a.against)
        bad += more
        say('audit against %s: %d dump-derived tables looked for' % (a.against, n))
    say('exe imports: ' + ', '.join('%s (%d)' % (d, len(v)) for d, v in sorted(imports.items())))
    if bad:
        for b in bad:
            say('AUDIT: ' + b)
        sys.exit('the package failed the audit: nothing written to %s' % a.out)
    say('audit: %d files, nothing of the game, nothing of this PC' % len(os.listdir(pkg)))

    # 6. the zip
    os.makedirs(a.out, exist_ok=True)
    zpath = os.path.join(a.out, 'ssbu-runtime-%s.zip' % (commit[:10] or 'local'))
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for n in sorted(os.listdir(pkg)):
            z.write(os.path.join(pkg, n), n)
    say('')
    say('package: %s  (%.1f MB, %.0fs)' % (zpath, os.path.getsize(zpath) / 1048576.0, time.time() - t0))
    say('sha256:  %s' % sha256(zpath))
    say('tree:    %s  commit %s%s' % (digest, commit[:10], ' (dirty)' if dirty else ''))
    say('After uploading it to a GitHub Release, put its URL and SHA-256 into scripts/install.py (RUNTIME_RELEASE).')
    return 0


if __name__ == '__main__':
    sys.exit(main())
