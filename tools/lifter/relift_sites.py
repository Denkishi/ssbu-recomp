"""Re-lift of the game DLL for a SITE LIST: run-time code changes of Skyline plugins (runtime/skyline.c) compiled in.

    python relift_sites.py build <site file> [--tag=sky] [--jobs=N] [--lift-only] [--force] [--base-gen=gen_eh_o2]
                                             [--base-obj=obj_eh_o1] [--flags-local] [--min-free-gb=G] [--no-prune]
                                             [--module=<name>]
    python relift_sites.py check <site file>      parse, check against the binary and classify; no lift

Base set: by default the one scripts/setup.py builds (main: gen_eh_o2 -> obj_eh_o1; --module: gen_nro_eh_o2\\<m> ->
obj_nro_eh_o2\\<m>). --flags-local: a base set made with flags_local.py's transform (gen_fl -> obj_fl23, gen_nro_fl23 ->
obj_nro_fl23 by default); the re-lifted files get the same transform. Only needed with that optional pass.

Output (nothing of the base set is written): gen_<tag>\\ (the re-lifted bucket files, funcs.h, table.c, sky_sites.c),
obj_<tag>\\ (their objects, _relift_report.txt) and ssbu_game_<tag>.dll + .lib (+ .map). An exe selects the DLL by its
import library:  RECOMP_DLL=ssbu_game_sky.dll RECOMP_EXE=ssbu_sky.exe python build.py link
(the default shim and the re-lifted DLL's shim are the same file; build.py would only relink the DLL if an object were newer).

SITE LIST: text, one site per line, `#` starts a comment, addresses are full guest addresses in hex.
  patch    <addr> <orig_word> <new_word>   the instruction word is replaced before lifting. orig is checked against the
                                           binary; several writes to one address are a chain (each orig = the previous
                                           new). A write outside .text needs no re-lift and is only reported.
  inline   <addr>   `sky_site_inline(c, addr);` before the instruction at addr (all registers are in c; the flags are
                    written back before and reloaded after when flags_local.py is used). At a function START (one lifted copy only,
                    reached through its entry) no callout is compiled: the runtime uses the hook table; reported, and
                    the address is treated as a `noinline` site.
  midhook  <addr>   a function hook written at a non-entry address: `if (sky_site_midhook(c, addr)) return;` before the
                    instruction, and a new lifted function f_<addr> starting there (in fn_table): the runtime's "orig".
                    f_<addr> enters behind its own callout; a jump back to addr inside it meets the callout as on hardware.
  dynimm   <addr>   the immediate of the instruction is read from guest memory each time it runs: add/sub immediate
                    (cmp, cmn, adds, subs, add, sub: imm12, lsl 12 and add/sub from the word; sf, S, Rn, Rd fixed) and
                    movz / movn (imm16, hw, z/n from the word; sf, Rd fixed); any other change of the word traps.
  jtable   <addr>   a `br` dispatching through a jump table that is patched at run time: checked, nothing to change
                    (the lifter loads the entry from guest memory and dispatches over every instruction of the
                    function, call_indirect for a target outside it).
  noinline <addr>   a function entry the hook table hooks: the bucket holding it is compiled with
                    `__attribute__((noinline))` on f_<addr> so no caller in that translation unit inlines it. If no
                    other site touches that bucket and hook_inline_check.py finds every caller in it still calling the
                    entry in the base objects, the base object is kept (--no-prune: compile it anyway).

STEPS. Every bucket file (48 KB of guest code) holding an affected lifted copy is lifted twice with recomp_eh's function
(+ flags_local's transform with --flags-local): WITHOUT the sites it must reproduce the base file byte for byte, WITH them it goes
to gen_<tag>. The changed lines of each file are counted (multiset of lines) and compared with what the sites predict.
Compile: the base set's compiler and flags (last header line of <base obj>\\_opt_report.txt) and the same functions
unoptimised (<base obj>\\c_XXXX.optnone.h) so a file differs from the base build only by the sites. Memory: at most
--jobs compiles (default 2), a new one only with --min-free-gb available (default 5), the lock file RECOMP_HEAVY_LOCK (if
set: other heavy builds that use it wait) held during the batch; a compile is killed (and retried alone) when free memory
drops under 1 GB.
Link: every object of the base set, the re-compiled ones replaced, table.obj from gen_<tag>\\table.c (new entries),
sky_sites.c (exports sky_site_table / sky_site_count: what this DLL has compiled in), the shim game_shim_eh.c.
"""
import bisect
import collections
import ctypes
import json
import os
import re
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
KINDS = {'patch': 1, 'inline': 2, 'midhook': 3, 'dynimm': 4, 'jtable': 5, 'noinline': 6}
LOCK = os.environ.get('RECOMP_HEAVY_LOCK')      # optional: a lock file shared with other heavy builds of the machine


def log(*a):
    print(*a, flush=True)


# ---------------------------------------------------------------------------------------------------- site list
class Sites:
    def __init__(self):
        self.patch = {}          # addr -> final word
        self.patch_chain = {}    # addr -> [words written]
        self.inline, self.midhook, self.dynimm, self.jtable, self.noinline = set(), set(), set(), set(), set()
        self.ignored = []        # (line, reason)


def parse_sites(path, recomp, what='main'):
    """the sites of one module: lines whose address is outside its .text are counted and skipped (a list recorded at run
    time names main, the script modules and the plugins); `hook <addr>` (an entry hook the runtime installs through the
    hook table) is a `noinline` site; `data` lines (writes outside code) are skipped"""
    s = Sites()
    s.other = collections.Counter()
    lo, hi = recomp.BASE, recomp.BASE + recomp.TEXT_SIZE
    with open(path) as f:
        for n, line in enumerate(f, 1):
            t = line.split('#', 1)[0].split()
            if not t:
                continue
            kind = t[0].lower()
            if kind == 'data':
                s.other['data'] += 1
                continue
            if kind == 'hook':
                kind = 'noinline'
            if kind not in KINDS:
                raise ValueError('%s:%d: unknown site kind %r' % (path, n, t[0]))
            want = 4 if kind == 'patch' else 2
            if len(t) != want:
                raise ValueError('%s:%d: %s takes %d field(s)' % (path, n, kind, want - 1))
            addr = int(t[1], 16)
            if not lo <= addr < hi:
                s.other[kind] += 1
                continue
            if addr & 3:
                raise ValueError('%s:%d: %x is not an instruction address of %s' % (path, n, addr, what))
            if kind == 'patch':
                old, new = int(t[2], 16), int(t[3], 16)
                have = s.patch.get(addr, recomp.word(addr))
                if have != old:
                    raise ValueError('%s:%d: patch %x expects %08x, the %s has %08x' % (path, n, addr, old, 'previous patch' if addr in s.patch else 'binary', have))
                s.patch[addr] = new
                s.patch_chain.setdefault(addr, []).append(new)
            else:
                getattr(s, kind).add(addr)
    return s


# ---------------------------------------------------------------------------------------------------- lifting
def dynimm_lines(w, a, recomp):
    """(check line, statement) for the run-time decoded instruction word w at a, or raise ValueError."""
    bits, SET, R, T = recomp.bits, recomp.SET, recomp.R, recomp.T
    sf, rd, rn = bits(w, 31, 31), bits(w, 4, 0), bits(w, 9, 5)
    word = '__atomic_load_n((const uint32_t *)(uintptr_t)0x%xULL, __ATOMIC_RELAXED)' % a
    if bits(w, 28, 23) == 0x22:                                 # add / adds / sub / subs (immediate); cmp / cmn = Rd 31 with S
        mask = 0xbf8003ff                                       # sf, S, class, Rn, Rd fixed; op, sh, imm12 from the word
        s = bits(w, 29, 29)
        n = 64 if sf else 32
        src = R(rn, sf, sp=True)
        imm = '((uint64_t)((iw_ >> 10) & 0xfff) << (((iw_ >> 22) & 1) ? 12 : 0))'
        if s:
            add = SET(rd, sf, 'adds%d(c, %s, (%s)im_, 0)' % (n, src, T(sf)))
            sub = SET(rd, sf, 'adds%d(c, %s, ~(%s)im_, 1)' % (n, src, T(sf)))
        else:
            add = SET(rd, sf, '%s + im_' % src, sp=True)
            sub = SET(rd, sf, '%s - im_' % src, sp=True)
        stmt = '{ uint32_t iw_ = %s; uint64_t im_ = %s; if ((iw_ >> 30) & 1) { %s } else { %s } }' % (word, imm, sub, add)
    elif bits(w, 28, 23) == 0x25 and bits(w, 29, 29) == 0:     # movn (opc 00) / movz (opc 10); movk (11) is not taken
        mask = 0xbf80001f                                       # sf, opc<0> (must stay 0), class, Rd fixed; opc<1>, hw, imm16 from the word
        stmt = ('{ uint32_t iw_ = %s; uint64_t v_ = (uint64_t)((iw_ >> 5) & 0xffff) << (((iw_ >> 21) & 3) * 16); '
                'if (!((iw_ >> 30) & 1)) v_ = ~v_; %s }' % (word, SET(rd, sf, 'v_')))
    else:
        raise ValueError('dynimm: %x holds %08x, neither add/sub immediate nor movz/movn' % (a, w))
    check = 'if ((%s & 0x%08xU) != 0x%08xU) guest_trap("dynimm: the instruction word was rewritten beyond its immediate", 0x%xULL);' % (word, mask, w & mask, a)
    return check, stmt


def module_info(name):
    """(path of the .nro, guest base, name, text size) of a game module (ro_study/nro_map.json)"""
    with open(os.path.join(HERE, 'ro_study', 'nro_map.json')) as f:
        for e in json.load(f):
            if e['name'] in (name, 'lua2cpp_' + name):
                return os.path.join(HERE, 'ro_study', 'nro', e['file']), e['base'], e['name'], e['text_size']
    raise SystemExit('unknown module %s (ro_study/nro_map.json)' % name)


class Relifter:
    """module=None: main (recomp.py + recomp_eh.py); module=<name>: a script module (ro_study/recomp_nro.py, which reads
    the module from the environment at import: RECOMP_NRO / RECOMP_NRO_BASE / RECOMP_NRO_MODULE)"""
    def __init__(self, base_gen, module=None, use_flags_local=False):
        for v in ('RECOMP_PATCHES', 'RECOMP_EXTRA_RESUME', 'RECOMP_SITES'):
            os.environ.pop(v, None)
        self.transform = lambda text: text             # the base set is the lifter's output as it is
        if use_flags_local:                            # a base set made with flags_local.py (optional pass): the same transform
            import flags_local
            flags_local.check_helpers()
            self.transform = flags_local.transform
        self.module = module
        if module:
            nro, base, name, _ = module_info(module)
            os.environ['RECOMP_NRO'], os.environ['RECOMP_NRO_BASE'], os.environ['RECOMP_NRO_MODULE'] = os.path.abspath(nro), base, name
            sys.path.insert(0, os.path.join(HERE, 'ro_study'))
            import recomp_nro as recomp
            self.module = recomp.MODULE
            self._lift = lambda env, e: recomp.lift_function(env, e)[0]
            self.generator = 'recomp.py'
            self.table_sym = 'fn_table_' + recomp.MODULE
        else:
            import recomp
            import recomp_eh
            self._lift = lambda env, e: recomp_eh.lift_function(env, e)[0]
            self.generator = 'recomp_eh.py'
            self.table_sym = 'fn_table'
        self.recomp = recomp
        self.base_gen = base_gen
        env = recomp.Env()
        env.imports = set()
        env.full = True
        if module:
            env.compute_starts()                       # includes the starts of the exception tables
        else:
            _, _, eh_starts = recomp_eh.eh_info()
            env.starts = sorted(set(env.compute_starts()) | set(eh_starts))
        self.env = env
        self.starts_set = set(env.starts)
        self.funcs_h = open(os.path.join(base_gen, 'funcs.h')).read()
        self.all_funcs = sorted(int(m, 16) for m in re.findall(r'^void f_([0-9a-f]+)\(Ctx \*c\);', self.funcs_h, re.M))
        self.func_set = set(self.all_funcs)
        self._sorted = None

    def bucket_of(self, a):
        return (a - self.recomp.BASE) // self.recomp.BUCKET

    def entries_holding(self, a):
        """the lifted functions (entries) whose code covers address a"""
        s = self.env.starts[bisect.bisect_right(self.env.starts, a) - 1]
        if self._sorted is None or len(self._sorted) != len(self.func_set):
            self._sorted = sorted(self.func_set)
        fs = self._sorted
        return fs[bisect.bisect_left(fs, s):bisect.bisect_right(fs, a)]

    def lift_entry(self, e, plan):
        """lifted text of entry e; plan = None (plain) or {addr: (pre lines, statement or None, midhook)}."""
        text = self._lift(self.env, e)
        if not plan:
            return text
        end = self.env.extent(e)
        mine = {a for a in plan if e <= a < end}
        if not mine:
            return text
        out = []
        for line in text.split('\n'):
            if line.startswith('L_') and not line.startswith('L_dispatch'):
                pc = int(line[2:line.index(':')], 16)
                if pc in mine:
                    pre, stmt, mid = plan[pc]
                    if not pre:                                    # a plain patch: the lifter has already lifted the new word
                        out.append(line)
                        continue
                    stmt = stmt or line[line.index(': ') + 2:]
                    if e == pc and mid and not out[-1].startswith('  goto L_'):
                        out.append('  goto L_%x_m;' % pc)          # the entry of f_<midhook addr> runs behind its own callout
                    out.append('L_%x: %s' % (pc, pre[0]))
                    out.extend('  ' + p for p in pre[1:])
                    out.append(('L_%x_m: ' % pc if mid else '  ') + stmt)
                    continue
            out.append(line)
        return '\n'.join(out)

    def bucket_text(self, b, entries, plan, noinline):
        texts = [self.lift_entry(e, plan) for e in entries]
        head = '/* generated by %s -- do not edit */\n#include "runtime.h"\n#include "funcs.h"\n' % self.generator
        if noinline:
            head += ''.join('void f_%x(Ctx *c) __attribute__((noinline));      /* relift_sites.py: hooked through the hook table */\n' % a
                            for a in sorted(noinline))
        return head + '\n' + '\n\n'.join(texts) + '\n'


# ---------------------------------------------------------------------------------------------------- jump tables
def check_jtable(rl, a):
    """Report on a `br` dispatching through a .rodata table; returns (ok, text)."""
    recomp = rl.recomp
    bits, sext, word = recomp.bits, recomp.sext, recomp.word
    w = word(a)
    if (w & 0xfffffc1f) != 0xd61f0000:
        return False, 'not a br (%08x)' % w
    rn = bits(w, 9, 5)
    notes = []
    for e in rl.entries_holding(a):
        if 'L_%x: { jt = c->x[%d]; goto L_dispatch; }' % (a, rn) not in rl.lift_entry(e, None):
            return False, 'f_%x does not lift the br as a dispatch on jt' % e
    owner = rl.env.starts[bisect.bisect_right(rl.env.starts, a) - 1]
    end = rl.env.extent(owner)
    # add xT, xT, xBase ; ldrsw xT, [xBase, xI, lsl #2] ; add xBase, xBase, #lo ; adrp xBase, page ; cmp wI, #N ; b.hi
    base = idx = count = None
    regs = {}
    for k in range(1, 24):
        pc = a - 4 * k
        v = word(pc)
        if (v & 0xffe0fc00) == 0xb8a07800 and base is None:           # ldrsw xt, [xn, xm, lsl #2]
            regs['tab'], idx = bits(v, 9, 5), bits(v, 20, 16)
        elif (v & 0xffc00000) == 0x91000000 and 'tab' in regs and bits(v, 4, 0) == regs['tab'] and 'lo' not in regs:
            regs['lo'] = bits(v, 21, 10)
        elif (v & 0x9f000000) == 0x90000000 and 'lo' in regs and bits(v, 4, 0) == regs['tab']:
            base = (pc & ~0xfff) + (sext((bits(v, 23, 5) << 2) | bits(v, 30, 29), 21) << 12) + regs['lo']
        elif idx is not None and (v & 0x7f80001f) == 0x7100001f and bits(v, 9, 5) == idx and count is None:
            count = bits(v, 21, 10) + 1                               # cmp wI, #N (then b.hi): N + 1 entries
        if base is not None and count is not None:
            break
    if base is None or count is None:
        return True, 'dispatch is dynamic in every lifted copy; table pattern not recognised (no bounds report)'
    out = []
    for i in range(count):
        off = base - recomp.BASE + i * 4
        t = base + struct.unpack_from('<i', recomp.IMG, recomp.FILE_OFF + off)[0]
        if not owner <= t < end:
            out.append(t)
    rodata = 0x39c8000 <= base - recomp.BASE < 0x4f15f60
    return True, ('table %x (%s) %d entries, read with rd32 at run time; dispatch switch of f_%x covers %x..%x; '
                  '%d entries of the binary outside it (would go to call_indirect)%s'
                  % (base, '.rodata' if rodata else 'NOT .rodata', count, owner, owner, end, len(out),
                     (': ' + ', '.join('%x' % t for t in out[:8])) if out else ''))


# ---------------------------------------------------------------------------------------------------- memory, lock
class MEMSTAT(ctypes.Structure):
    _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong), ('ullTotalPhys', ctypes.c_ulonglong),
                ('ullAvailPhys', ctypes.c_ulonglong), ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong), ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]


def free_gb():
    m = MEMSTAT()
    m.dwLength = ctypes.sizeof(m)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.ullAvailPhys / 2.0 ** 30


class PMC(ctypes.Structure):
    _fields_ = [('cb', ctypes.c_ulong), ('PageFaultCount', ctypes.c_ulong), ('PeakWorkingSetSize', ctypes.c_size_t),
                ('WorkingSetSize', ctypes.c_size_t), ('QuotaPeakPagedPoolUsage', ctypes.c_size_t), ('QuotaPagedPoolUsage', ctypes.c_size_t),
                ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t), ('QuotaNonPagedPoolUsage', ctypes.c_size_t),
                ('PagefileUsage', ctypes.c_size_t), ('PeakPagefileUsage', ctypes.c_size_t)]


def proc_mem(p):
    """(private bytes, peak private bytes) of a Popen'd process (clang runs cc1 in its own process: integrated cc1)"""
    c = PMC()
    c.cb = ctypes.sizeof(c)
    h = ctypes.c_void_p(int(p._handle))
    if not ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(c), c.cb):
        return 0, 0
    return c.PagefileUsage, c.PeakPagefileUsage


def take_lock(purpose):
    while LOCK:
        try:
            fd = os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, ('%s\nstarted %s pid %d\n' % (purpose, time.strftime('%Y-%m-%d %H:%M:%S'), os.getpid())).encode())
            os.close(fd)
            return
        except FileExistsError:
            try:
                age = time.time() - os.path.getmtime(LOCK)
                if age > 3 * 3600:
                    log('  heavy.lock is %.1f h old: stale, removed' % (age / 3600))
                    os.remove(LOCK)
                    continue
                who = open(LOCK).read().strip().replace('\n', ' | ')
            except OSError:
                continue
            log('  waiting for %s (%s)' % (LOCK, who))
            time.sleep(30)


def drop_lock():
    if not LOCK:
        return
    try:
        os.remove(LOCK)
    except OSError:
        pass


# ---------------------------------------------------------------------------------------------------- compile
def base_settings(base_obj, module=None):
    """compiler, -O level, extra flags and the per-file optnone limit / whole-file -O0 of the base set's report
    (a module's report is obj_nro_<suffix>/_opt_report.txt, its lines `<module>/c_XXXX.c`)"""
    import build_opt                              # no report (no fallback was needed): build_opt.py's settings (RECOMP_CLANG / _OPT / _CFLAGS)
    clang, opt, extra = build_opt.CLANG, build_opt.OPT, list(build_opt.EXTRA)
    per_file = {}
    rep = os.path.join(os.path.dirname(base_obj) if module else base_obj, '_opt_report.txt')
    prefix = re.escape(module + '/') if module else ''
    if os.path.exists(rep):
        for line in open(rep):
            m = re.match(r'^# \S+ \S+\s+(.+?clang\.exe) (-O\d) (.*?)\s+optnone over (\d+) statements', line)
            if m:
                clang, opt, extra = m.group(1), m.group(2), m.group(3).split()
                continue
            m = re.match(r'^%s(c_[0-9a-f]+\.c)\s+\d+s\s*(.*)$' % prefix, line)
            if m:
                note = m.group(2)
                lim = re.search(r'over (\d+) statements', note)
                per_file[m.group(1)] = ('O0' if 'WHOLE FILE' in note else int(lim.group(1)) if lim else None)
    return clang, opt, extra, per_file


def optnone_names(path):
    if not os.path.exists(path):
        return []
    return re.findall(r'^void (\w+)\((.*?)\) __attribute__\(\(optnone, noinline\)\);', open(path).read(), re.M)


class Compiler:
    def __init__(self, base_obj, gen, obj, jobs, min_free, module=None):
        import build_opt
        self.build_opt = build_opt
        self.base_obj, self.gen, self.obj = base_obj, gen, obj
        self.clang, self.opt, self.extra, self.per_file = base_settings(base_obj, module)
        self.jobs, self.min_free = jobs, min_free
        self.lock = threading.Lock()
        self.running = {}
        self.results = {}

    def command(self, name, new_funcs):
        src, obj = os.path.join(self.gen, name), os.path.join(self.obj, name[:-2] + '.obj')
        how = self.per_file.get(name)
        if how == 'O0':
            return [self.clang, '-c', '-O0'] + self.extra + ['-w', '-fno-strict-aliasing', '-I', HERE, '-I', self.gen, src, '-o', obj], 'whole file -O0 (as the base set)'
        names = optnone_names(os.path.join(self.base_obj, name[:-2] + '.optnone.h'))
        limit = how or self.build_opt.OPTNONE_LINES
        have = set(n for n, _ in names)
        for n, fname, params in self.build_opt.function_sizes(src):
            if fname not in have and n > limit and int(fname[2:], 16) in new_funcs:
                names.append((fname, params))
        hdr = obj[:-4] + '.optnone.h'
        cmd = [self.clang, '-c', self.opt] + self.extra + ['-w', '-fno-strict-aliasing', '-I', HERE, '-I', self.gen]
        if names:
            with open(hdr, 'w') as f:
                f.write('/* relift_sites.py: the functions the base set compiled unoptimised (+ new ones over %d statements) */\n#include "runtime.h"\n' % limit)
                f.writelines('void %s(%s) __attribute__((optnone, noinline));\n' % nm for nm in names)
            cmd += ['-include', hdr]
        elif os.path.exists(hdr):
            os.remove(hdr)
        return cmd + [src, '-o', obj], ('%d function(s) unoptimised' % len(names) if names else 'fully optimised')

    def run_one(self, name, new_funcs, timeout=3600):
        cmd, note = self.command(name, new_funcs)
        t = time.time()
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with self.lock:
            self.running[name] = p
        peak = 0
        out = b''
        try:
            while p.poll() is None:
                _, pk = proc_mem(p)
                peak = max(peak, pk)
                if time.time() - t > timeout:
                    p.kill()
                    note += ' TIMEOUT'
                    break
                time.sleep(0.5)
            out = p.stdout.read()
            _, pk = proc_mem(p)
            peak = max(peak, pk)
        finally:
            with self.lock:
                self.running.pop(name, None)
        return p.returncode, out.decode('latin-1')[-1500:], time.time() - t, peak, note

    def watchdog(self, stop):
        while not stop.is_set():
            if free_gb() < 1.0:
                with self.lock:
                    victims = sorted(self.running.items(), key=lambda kv: -proc_mem(kv[1])[0])
                if victims and len(victims) > 1:
                    name, p = victims[0]
                    log('  memory under 1 GB: killing the compile of %s (retried alone later)' % name)
                    self.killed.add(name)
                    p.kill()
            stop.wait(2)

    def compile_all(self, names, new_funcs):
        self.killed = set()
        stop = threading.Event()
        threading.Thread(target=self.watchdog, args=(stop,), daemon=True).start()
        todo = list(names)
        failed = []
        workers = []

        def worker(name):
            code, out, secs, peak, note = self.run_one(name, new_funcs)
            self.results[name] = (code, secs, peak, note)
            if name in self.killed:
                return
            log('  compiled %s: exit %s  %.0fs  peak %.1f GB  %s %s' % (name, code, secs, peak / 2.0 ** 30, note, out.strip()[-400:] if code else ''))
            if code:
                failed.append(name)

        while todo or workers:
            workers = [w for w in workers if w.is_alive()]
            if todo and len(workers) < self.jobs and (not workers or free_gb() >= self.min_free):
                name = todo.pop(0)
                w = threading.Thread(target=worker, args=(name,))
                w.start()
                workers.append(w)
                time.sleep(3)
                continue
            time.sleep(1)
        for name in sorted(self.killed):                       # alone
            log('  retrying %s alone' % name)
            self.killed.discard(name)
            worker(name)
        stop.set()
        return failed


# ---------------------------------------------------------------------------------------------------- PE / map checks
def pe_fn_table_check(dll, mapfile, wanted, table_sym='fn_table'):
    """read fn_table out of the linked DLL (address from the link map) and check that each wanted guest address is
    there and points at f_<addr>. Returns {addr: ok?}."""
    sym = {}
    for line in open(mapfile, errors='replace'):
        t = line.split()
        if len(t) >= 3 and re.match(r'^[0-9a-fA-F]{16}$', t[2] if len(t) > 2 else ''):
            sym[t[1]] = int(t[2], 16)
    data = open(dll, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt = pe + 24
    image_base = struct.unpack_from('<Q', data, opt + 24)[0]
    secs = []
    so = opt + struct.unpack_from('<H', data, pe + 20)[0]
    for i in range(nsec):
        _, vsize, va, rsize, raw = struct.unpack_from('<8sIIII', data, so + i * 40)
        secs.append((va, max(vsize, rsize), raw))

    def at(rva, n):
        for va, size, raw in secs:
            if va <= rva < va + size:
                return data[raw + rva - va: raw + rva - va + n]
        raise ValueError('rva %x not in the image' % rva)
    tab = sym.get(table_sym)
    cnt = sym.get('fn_table_count' + table_sym[len('fn_table'):])
    n = struct.unpack('<i', at(cnt - image_base, 4))[0]
    res = {}
    lo, hi = 0, n - 1
    entries = {}
    for a in wanted:
        lo, hi = 0, n - 1
        found = None
        while lo <= hi:
            mid = (lo + hi) // 2
            ga, fp = struct.unpack('<QQ', at(tab - image_base + mid * 16, 16))
            if ga == a:
                found = fp
                break
            if ga < a:
                lo = mid + 1
            else:
                hi = mid - 1
        res[a] = found is not None and found == sym.get('f_%x' % a)
        entries[a] = found
    return n, res


# ---------------------------------------------------------------------------------------------------- main
ORDERING = [(re.compile(r'\blda(8|16|32|64)\('), r'rd\1('), (re.compile(r'\bstl(8|16|32|64)\('), r'wr\1('),
            (re.compile(r'__atomic_(thread|signal)_fence\(__ATOMIC_\w+\);'), ';')]


def ordering_drift(raw, base_eh, name, have_fl, rl):
    """number of lines by which the current lifter's plain output differs from the base set's (before flags_local) when
    every difference is the ordered-access lifting (ldar / stlr as lda / stl, dmb / isb as fences, which used to be plain
    accesses and `;`), and the base set's flags_local file is the transform of its lifter output; else None"""
    path = os.path.join(base_eh, name)
    if not os.path.exists(path):
        return None
    with open(path, encoding='latin-1') as f:
        old = f.read()
    if rl.transform(old) != have_fl:
        return None
    a, b = old.split('\n'), raw.split('\n')
    if len(a) != len(b):
        return None
    n = 0
    for x, y in zip(a, b):
        if x == y:
            continue
        for pat, rep in ORDERING:
            y = pat.sub(rep, y)
        if x != y:
            return None
        n += 1
    return n


def write_if_changed(path, text):
    """keep the time stamp of an unchanged file (the compile step is incremental)"""
    if os.path.exists(path):
        with open(path, encoding='latin-1') as f:
            if f.read() == text:
                return False
    with open(path, 'w', encoding='latin-1', newline='\n') as f:
        f.write(text)
    return True


def plan_sites(rl, sites):
    """classify; returns (plan {addr: (pre, stmt, midhook)}, notes, refused, site table rows)"""
    recomp = rl.recomp
    plan, notes, refused, table = {}, [], [], []
    for a in sorted(sites.midhook):
        if a in rl.starts_set:
            refused.append(('midhook', a, 'a function start: hook it through the hook table (entry hook)'))
            continue
        if a in sites.inline or a in sites.dynimm:
            refused.append(('midhook', a, 'also an inline / dynimm site'))
            continue
        if a in rl.func_set:
            refused.append(('midhook', a, 'already a lifted entry (a branch target lifted on its own): its callers and "orig" would be the same function'))
            continue
        plan[a] = (['if (sky_site_midhook(c, 0x%xULL)) return;' % a], None, True)
        table.append((a, 3, 0))
    for a in sorted(sites.inline):
        if a in rl.starts_set:
            notes.append('inline %x: a function start (only f_%x holds it): no callout, the runtime hooks the entry (noinline added)' % (a, a))
            sites.noinline.add(a)
            continue
        if a in sites.midhook:
            continue
        pre = ['sky_site_inline(c, 0x%xULL);' % a]
        plan[a] = (pre, None, False)
        table.append((a, 2, 0))
    for a in sorted(sites.dynimm):
        if a in sites.midhook:
            continue
        w = struct.unpack_from('<I', recomp.IMG, recomp.FILE_OFF + a - recomp.BASE)[0]
        try:
            check, stmt = dynimm_lines(w, a, recomp)
        except ValueError as e:
            refused.append(('dynimm', a, str(e)))
            continue
        if a in sites.patch:
            notes.append('patch %x: the address is a dynimm site, the word is decoded at run time (patch to %08x not compiled in)' % (a, sites.patch[a]))
            del sites.patch[a]
        pre, _, mid = plan.get(a, ([], None, False))
        plan[a] = (pre + [check], stmt, mid)
        table.append((a, 4, w))
    for a, w in sorted(sites.patch.items()):
        plan.setdefault(a, ([], None, False))
        table.append((a, 1, w))
    for a in sorted(sites.jtable):
        ok, text = check_jtable(rl, a)
        (notes if ok else refused).append(('jtable %x: %s' % (a, text)) if ok else ('jtable', a, text))
        if ok:
            table.append((a, 5, 0))
    for a in sorted(sites.noinline):
        if a not in rl.func_set:
            refused.append(('noinline', a, 'not the entry of a lifted function'))
            continue
        table.append((a, 6, 0))
    for line, why in sites.ignored:
        notes.append('ignored "%s": %s' % (line, why))
    for a, chain in sites.patch_chain.items():
        if len(chain) > 1:
            notes.append('patch %x written %d times, compiled with the last word %08x' % (a, len(chain), chain[-1]))
    return plan, notes, refused, sorted(table)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('build', 'check'):
        print(__doc__)
        return 2
    opts = dict(a[2:].split('=', 1) if '=' in a else (a[2:], '1') for a in sys.argv[3:] if a.startswith('--'))
    site_file = os.path.abspath(sys.argv[2])
    tag = opts.get('tag', 'sky')
    module = opts.get('module')
    fl = 'flags-local' in opts                    # base set: setup.py's (gen_eh_o2 -> obj_eh_o1), or flags_local's (gen_fl -> obj_fl23)
    if module:                                    # a script module: gen_nro_eh_o2\<m> / obj_nro_eh_o2\<m> -> nro_bin_<tag>\<m>.dll
        mname = module_info(module)[2]
        base_gen = os.path.join(HERE, opts.get('base-gen', os.path.join('gen_nro_fl23' if fl else 'gen_nro_eh_o2', mname)))
        base_eh = os.path.join(HERE, opts.get('base-eh', os.path.join('gen_nro_eh_o2', mname)))
        base_obj = os.path.join(HERE, opts.get('base-obj', os.path.join('obj_nro_fl23' if fl else 'obj_nro_eh_o2', mname)))
        gen, obj = os.path.join(HERE, 'gen_nro_' + tag, mname), os.path.join(HERE, 'obj_nro_' + tag, mname)
        dll = os.path.join(HERE, 'nro_bin_' + tag, mname + '.dll')
    else:
        base_gen = os.path.join(HERE, opts.get('base-gen', 'gen_fl' if fl else 'gen_eh_o2'))
        base_eh = os.path.join(HERE, opts.get('base-eh', 'gen_eh_o2'))
        base_obj = os.path.join(HERE, opts.get('base-obj', 'obj_fl23' if fl else 'obj_eh_o1'))
        gen, obj = os.path.join(HERE, 'gen_' + tag), os.path.join(HERE, 'obj_' + tag)
        dll = os.path.join(HERE, 'ssbu_game_%s.dll' % tag)
    t0 = time.time()
    rl = Relifter(base_gen, module, fl)
    recomp = rl.recomp
    sites = parse_sites(site_file, recomp, rl.module or 'main')
    log('site list %s for %s: %d patches, %d inline, %d midhook, %d dynimm, %d jtable, %d noinline; function entries %d (%.0fs)'
        % (site_file, rl.module or 'main', len(sites.patch), len(sites.inline), len(sites.midhook), len(sites.dynimm), len(sites.jtable),
           len(sites.noinline), len(rl.env.starts), time.time() - t0))
    if sites.other:
        log('  lines for other modules / data, skipped: ' + ', '.join('%s %d' % kv for kv in sorted(sites.other.items())))
    plan, notes, refused, table = plan_sites(rl, sites)
    for n in notes:
        log('  note: ' + n)
    for kind, a, why in refused:
        log('  REFUSED %s %x: %s' % (kind, a, why))
    patches = dict(sites.patch)

    # new entries: the mid-function hooks, then whatever a patched branch or a new entry reaches that is no entry yet
    new = [a for a in sorted(sites.midhook) if a in plan and a not in rl.func_set]
    for a in new:
        rl.func_set.add(a)
    noinline_by_bucket = collections.defaultdict(set)
    for a in sites.noinline:
        if a in rl.func_set:
            noinline_by_bucket[rl.bucket_of(a)].add(a)
    # a noinline entry whose file no other site re-lifts: if hook_inline_check.py finds every caller in that file still
    # calling it in the base objects, the base object is kept (the property holds there); else the file is compiled again
    code_buckets = set(rl.bucket_of(e) for a in plan for e in rl.entries_holding(a)) | set(rl.bucket_of(a) for a in new)
    alone = sorted(a for b, s in noinline_by_bucket.items() if b not in code_buckets for a in s)
    if alone and 'no-prune' not in opts:
        import hook_inline_check
        res = hook_inline_check.check(alone, base_gen, base_obj, base=recomp.BASE)
        # a file is kept only if ALL its noinline entries pass; a file compiled again gets the attribute on all of them
        # (its new code could inline one that the base object does not)
        keep_b = set(rl.bucket_of(a) for a in alone) - set(rl.bucket_of(a) for a in alone if res[a]['verdict'] != 'OK')
        kept = [a for a in alone if rl.bucket_of(a) in keep_b]
        for b in keep_b:
            del noinline_by_bucket[b]
        log('  noinline: %d entr%s in files no other site touches; %d verified not inlined in %s (%d object(s) kept), %d in %d file(s) compiled again for: %s'
            % (len(alone), 'y' if len(alone) == 1 else 'ies', len(kept), os.path.basename(base_obj), len(keep_b), len(alone) - len(kept),
               len(set(rl.bucket_of(a) for a in alone) - keep_b), ' '.join('%x(%s)' % (a, res[a]['verdict']) for a in alone if res[a]['verdict'] != 'OK') or '-'))
    pairs = collections.defaultdict(list)          # bucket -> [(entry, addr)]
    while True:
        pairs.clear()
        for a in plan:
            for e in rl.entries_holding(a):
                pairs[rl.bucket_of(e)].append((e, a))
        buckets = sorted(set(pairs) | set(noinline_by_bucket) | set(rl.bucket_of(a) for a in new))
        # lift with the sites to collect targets that are no entry yet
        recomp.PATCHES = patches
        rl.env.wanted = collections.OrderedDict()
        for b in buckets:
            for e in sorted(x for x in rl.func_set if rl.bucket_of(x) == b):
                rl.lift_entry(e, plan)
        recomp.PATCHES = {}
        extra = [w for w in rl.env.wanted if w not in rl.func_set and recomp.BASE <= w < recomp.BASE + recomp.TEXT_SIZE]
        if not extra:
            break
        log('  new entries reached by patched code / new entries: %s' % ', '.join('%x' % w for w in extra))
        new += extra
        rl.func_set.update(extra)
    new = sorted(set(new))
    by_bucket = collections.defaultdict(list)
    for e in sorted(rl.func_set):
        by_bucket[rl.bucket_of(e)].append(e)
    log('%d bucket file(s) to re-lift: %s; new entries: %s (%.0fs)' % (len(buckets), ' '.join('c_%04x' % b for b in buckets),
                                                                   ' '.join('%x' % a for a in new) or 'none', time.time() - t0))
    if sys.argv[1] == 'check':
        return 1 if refused else 0

    os.makedirs(gen, exist_ok=True)
    os.makedirs(obj, exist_ok=True)
    report = ['# relift_sites.py %s  %s  site list %s' % (time.strftime('%Y-%m-%d %H:%M'), tag, site_file)]
    names, bad = [], 0
    base_entries = set(rl.all_funcs)
    for b in buckets:
        name = 'c_%04x.c' % b
        plain_entries = [e for e in by_bucket[b] if e in base_entries]
        raw = rl.bucket_text(b, plain_entries, None, None)
        plain = rl.transform(raw)
        with open(os.path.join(base_gen, name), encoding='latin-1') as f:
            have = f.read()
        if plain != have:
            n_drift = ordering_drift(raw, base_eh, name, have, rl) if 'allow-ordering-drift' in opts else None
            if n_drift is None:
                log('  %s: the re-lift WITHOUT sites differs from %s: NOT the same lifter / function list, stop%s' % (
                    name, base_gen, '' if 'allow-ordering-drift' in opts else ' (--allow-ordering-drift: accept a base set lifted before the ordered-access lifting)'))
                bad += 1
                continue
            line = ('  %s: the base set predates the ordered-access lifting (the separate lifter ordering change): %d line(s) differ only by '
                    'lda/stl/fences; this file is compiled from the current lifter' % (name, n_drift))
            log(line)
            report.append(line)
            raw = None
        if raw is not None and os.path.exists(os.path.join(base_eh, name)):     # and before the flags transform, the lifter's own output
            with open(os.path.join(base_eh, name), encoding='latin-1') as f:
                if f.read() != raw:
                    log('  %s: the re-lift WITHOUT sites differs from %s (the lifter output), stop' % (name, base_eh))
                    bad += 1
                    continue
        recomp.PATCHES = patches
        text = rl.transform(rl.bucket_text(b, by_bucket[b], plan, noinline_by_bucket.get(b)))
        recomp.PATCHES = {}
        a_lines, b_lines = collections.Counter(plain.split('\n')), collections.Counter(text.split('\n'))
        removed, added = sum((a_lines - b_lines).values()), sum((b_lines - a_lines).values())
        # expected: per (lifted copy, site address) the line `L_<a>: <stmt>` goes, 1 + callout/check lines come;
        # + `goto L_<a>_m;` in f_<midhook>; + every line of a new function and its blank separator; + noinline declarations
        exp_removed = len(pairs[b])
        exp_added = sum(1 + len(plan[a][0]) for _, a in pairs[b])
        exp_added += sum(1 for e, a in pairs[b] if e == a and plan[a][2])
        exp_added += len(noinline_by_bucket.get(b, ()))
        newb = [e for e in by_bucket[b] if e not in base_entries]
        for e in newb:                                               # a new function: all its lines (+ a blank separator), not per site
            recomp.PATCHES = patches
            exp_added += len(rl.transform(rl.lift_entry(e, plan)).split('\n')) + 1
            recomp.PATCHES = {}
            exp_added -= sum(1 + len(plan[a][0]) + (1 if e == a and plan[a][2] else 0) for ee, a in pairs[b] if ee == e)
            exp_removed -= sum(1 for ee, a in pairs[b] if ee == e)
        # a patch can change the shape of a lifted copy: a new `br` adds `uint64_t jt` and the L_dispatch switch (a line
        # per instruction + 2), a new / removed terminator at the end changes the fall-through line
        for e in sorted(set(e for e, _ in pairs[b]) - set(newb)):
            p0 = rl.lift_entry(e, None)
            recomp.PATCHES = patches
            p1 = rl.lift_entry(e, None)
            recomp.PATCHES = {}
            n_ins = sum(1 for l in p0.split('\n') if l.startswith('L_') and not l.startswith('L_dispatch'))
            j0, j1 = '  uint64_t jt = 0;' in p0, '  uint64_t jt = 0;' in p1
            if j1 and not j0:
                exp_added += n_ins + 3
            elif j0 and not j1:
                exp_removed += n_ins + 3
            def tail(x):                                             # the line behind the last instruction
                ls = x.split('\n')
                end = next(i for i, l in enumerate(ls) if l == '}' or l.startswith('L_dispatch:'))
                return ls[end - 1]
            if tail(p0) != tail(p1):
                exp_added += 1
                exp_removed += 1
        ok = removed == exp_removed and added == exp_added
        desc = collections.Counter()
        for a in sorted(set(a for _, a in pairs[b])):
            if a in sites.midhook and a in plan and plan[a][2]:
                desc['midhook'] += 1
            if a in sites.inline and a in plan and not plan[a][2] and plan[a][0] and plan[a][0][0].startswith('sky_site_inline'):
                desc['inline'] += 1
            if a in sites.dynimm and a in plan:
                desc['dynimm'] += 1
            if a in patches:
                desc['patch'] += 1
        line = ('  %s: identical without sites; with sites %d lines out, %d in (expected %d / %d) %s; %d lifted copies of %d site addresses [%s]%s%s'
                % (name, removed, added, exp_removed, exp_added, 'ok' if ok else '<-- CHECK', len(pairs[b]), len(set(a for _, a in pairs[b])),
                   ' '.join('%s %d' % kv for kv in sorted(desc.items())),
                   ('; noinline ' + ' '.join('f_%x' % a for a in sorted(noinline_by_bucket[b]))) if b in noinline_by_bucket else '',
                   ('; new ' + ' '.join('f_%x' % a for a in newb)) if newb else ''))
        log(line)
        report.append(line)
        bad += not ok
        path = os.path.join(gen, name)
        if not os.path.exists(path) or open(path, encoding='latin-1').read() != text:
            with open(path, 'w', encoding='latin-1', newline='\n') as f:
                f.write(text)
        names.append(name)
    if bad:
        log('%d file(s) failed the checks: stop' % bad)
        return 1
    # funcs.h / table.c with the new entries; the resume table is unchanged (a pad belongs to the outermost lifted copy)
    decl = ''.join('void f_%x(Ctx *c);\n' % a for a in new)
    for a in new:
        if 'void u_%x(Ctx *c, uint64_t rp) {' % a in open(os.path.join(gen, 'c_%04x.c' % rl.bucket_of(a)), encoding='latin-1').read():
            decl += 'void u_%x(Ctx *c, uint64_t rp);\n' % a
    funcs_h = rl.funcs_h + '/* relift_sites.py */\nvoid sky_site_inline(Ctx *c, uint64_t addr);\nint sky_site_midhook(Ctx *c, uint64_t addr);\n' + decl
    write_if_changed(os.path.join(gen, 'funcs.h'), funcs_h)
    table_c = open(os.path.join(base_gen, 'table.c')).read()
    ts = rl.table_sym                                                    # fn_table (main) / fn_table_<module>
    start = table_c.index('const FnEntry %s[] = {\n' % ts) + len('const FnEntry %s[] = {\n' % ts)
    cname = 'fn_table_count' + ts[len('fn_table'):]
    stop = table_c.index('};\nconst int %s = ' % cname, start)
    rows = table_c[start:stop].split('\n')[:-1]
    allf = sorted(rl.func_set)
    assert len(rows) == len(rl.all_funcs)
    rows = ['  { 0x%xULL, f_%x },' % (a, a) for a in allf]
    m = re.search(r'const int %s = (\d+);' % cname, table_c)
    table_c = table_c[:start] + '\n'.join(rows) + '\n' + table_c[stop:].replace(m.group(0), 'const int %s = %d;' % (cname, len(allf)), 1)
    write_if_changed(os.path.join(gen, 'table.c'), table_c)
    hs = ('/* generated by relift_sites.py from %s -- the sites compiled into this DLL (the vanilla DLL has no such export)\n'
          ' * kind: 1 patch (word = the instruction word compiled in), 2 inline callout, 3 midhook callout + entry,\n'
          ' * 4 dynimm (word = the word of the binary; its immediate is read at run time), 5 jtable (dynamic dispatch checked),\n'
          ' * 6 noinline (hook-table entry no caller of its file inlines) */\n#include <stdint.h>\n'
          'typedef struct { uint64_t addr; uint32_t kind; uint32_t word; } SkySite;\n'
          '__declspec(dllexport) const SkySite sky_site_table[] = {\n' % os.path.basename(site_file))
    hs += ''.join('  { 0x%xULL, %d, 0x%08xU },\n' % r for r in table) or '  { 0, 0, 0 },\n'
    hs += '};\n__declspec(dllexport) const int sky_site_count = %d;\n' % len(table)
    write_if_changed(os.path.join(gen, 'sky_sites.c'), hs)
    keep = set(names)                                                   # files of an earlier site list that this one does not touch
    stale = [os.path.join(gen, n) for n in os.listdir(gen) if re.match(r'^c_[0-9a-f]+\.c$', n) and n not in keep]
    stale += [os.path.join(obj, n) for n in os.listdir(obj) if re.match(r'^c_[0-9a-f]+\.(obj|optnone\.h)$', n) and n.split('.')[0] + '.c' not in keep]
    for n in stale:
        os.remove(n)
    if stale:
        log('  removed %d file(s) of an earlier site list' % len(stale))
    t_lift = time.time() - t0
    log('lift done: %d files (%.0fs)' % (len(names), t_lift))
    if 'lift-only' in opts:
        return 0

    # ---- compile
    comp = Compiler(base_obj, gen, obj, int(opts.get('jobs', '2')), float(opts.get('min-free-gb', '5')), rl.module)
    todo = []
    for name in names + ['table.c']:
        s, o = os.path.join(gen, name), os.path.join(obj, name[:-2] + '.obj')
        if 'force' in opts or not os.path.exists(o) or os.path.getmtime(o) < os.path.getmtime(s):
            todo.append(name)
    log('compile: %d of %d files (%s %s %s), %d at a time' % (len(todo), len(names) + 1, comp.clang, comp.opt, ' '.join(comp.extra), comp.jobs))
    t1 = time.time()
    failed = []
    if todo:
        take_lock('relift_sites.py: compiling %d files of gen_%s (%s, up to %d at a time)' % (len(todo), tag, comp.opt, comp.jobs))
        try:
            failed = comp.compile_all(todo, set(new))
        finally:
            drop_lock()
    t_comp = time.time() - t1
    for name in todo:
        code, secs, peak, note = comp.results.get(name, (None, 0, 0, ''))
        report.append('  compiled %-12s exit %s %6.0fs  peak %5.2f GB  %s' % (name, code, secs, peak / 2.0 ** 30, note))
    if failed:
        log('compile failed: %s' % ' '.join(failed))
        return 1

    # ---- link
    t2 = time.time()
    mine = set(n[:-2] + '.obj' for n in names) | {'table.obj'}       # only this site list's files (not those of an earlier list)
    base = sorted(n for n in os.listdir(base_obj) if n.endswith('.obj'))
    objs = [os.path.join(obj if n in mine else base_obj, n) for n in base]
    mapfile = dll[:-4] + '.map'
    if rl.module:
        # a module DLL: its own shim (nro_shim.c with the site callouts, -DNRO_MODULE) and the site table as objects
        os.makedirs(os.path.dirname(dll), exist_ok=True)
        flags = ['-c', comp.opt] + comp.extra + ['-w', '-fno-strict-aliasing', '-I', HERE, '-I', gen]
        for src, o, extra in ((os.path.join(HERE, 'nro_shim.c'), os.path.join(obj, '_shim.obj'), ['-DNRO_MODULE=' + rl.module]),
                              (os.path.join(gen, 'sky_sites.c'), os.path.join(obj, '_sky_sites.obj'), [])):
            r = subprocess.run([comp.clang] + flags + extra + [src, '-o', o], capture_output=True, text=True)
            if r.returncode:
                log('could not compile %s:\n%s' % (src, (r.stdout + r.stderr)[-2000:]))
                return 1
        objs = [os.path.join(obj, '_shim.obj') if n == '_shim.obj' else o for n, o in zip(base, objs)] + [os.path.join(obj, '_sky_sites.obj')]
        mine |= {'_shim.obj'}
        cmd_head, cmd_tail = [comp.clang, '-shared'], []
    else:
        shim = os.path.join(HERE, os.environ.get('RECOMP_SHIM', 'game_shim_eh.c'))
        cmd_head, cmd_tail = [comp.clang, '-shared', '-O1', '-w', '-I', HERE, '-I', gen, shim, os.path.join(gen, 'sky_sites.c')], []
    rsp = dll + '.rsp'
    with open(rsp, 'w') as f:
        f.write('\n'.join('"%s"' % o.replace('\\', '/') for o in objs))
    cmd = cmd_head + ['@' + rsp, '-o', dll, '-Wl,/MAP:' + mapfile] + cmd_tail
    r = subprocess.run(cmd, capture_output=True, text=True)
    t_link = time.time() - t2
    log((r.stdout + r.stderr)[-3000:])
    log('link %s: exit %d, %d objects (%d re-compiled), %.0f MB, %.0fs' % (os.path.basename(dll), r.returncode, len(objs), len(mine & set(base)),
                                                                     os.path.getsize(dll) / 2.0 ** 20 if os.path.exists(dll) else 0, t_link))
    if r.returncode:
        return r.returncode
    if rl.module:
        for ext in ('.lib', '.exp'):                                    # nothing links against a module DLL (build_nro.py)
            if os.path.exists(dll[:-4] + ext):
                os.remove(dll[:-4] + ext)
    n, res = pe_fn_table_check(dll, mapfile, new, rl.table_sym)
    line = 'fn_table of %s: %d entries (base %d); new entries %s' % (os.path.basename(dll), n, len(rl.all_funcs),
                                                                  ', '.join('%x %s' % (a, 'present' if ok else 'MISSING') for a, ok in sorted(res.items())) or 'none')
    log(line)
    report.append(line)
    report.append('time: lift %.0fs, compile %.0fs, link %.0fs' % (t_lift, t_comp, t_link))
    with open(os.path.join(obj, '_relift_report.txt'), 'a') as f:
        f.write('\n'.join(report) + '\n')
    log('total %.0fs' % (time.time() - t0))
    return 0 if all(res.values()) and n == len(rl.func_set) else 1


if __name__ == '__main__':
    sys.exit(main())
