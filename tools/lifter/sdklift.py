"""Lift selected functions of the real SDK module (binaries\\13.0.4\\sdk.elf) to C with the project's lifter.

    python sdklift.py closure <mangled name> ...     size of the call closure of each root (nothing written)
    python sdklift.py gen                            lift every root of sdklift_roots.txt -> sdklift_gen.c

The SDK image is mapped by sdklift_rt.c at SDK_BASE; lifted code runs on it exactly like the game's code
runs on the game image. A call from a lifted function goes to
  * another lifted SDK function (the callee is lifted too), or
  * the HOST handler of the same name, when the callee is an exported SDK function that the game itself
    imports (those have host implementations: nn::os, memcpy, ...): "cut", or
  * a trap, for names listed under `trap` in sdklift_roots.txt (abort paths, service/IPC boundaries).
sdklift_roots.txt: one mangled name per line = root; `cut <name>` forces a host cut; `trap <name>` a trap;
`lift <name>` lifts the SDK's own code even though the game imports the name; `addr <hex> [label]` = root
without a symbol (function pointer target found at run time).
"""
import os, sys, struct, bisect, collections, re, glob

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'ro_study'))
import recomp_nro as R

SDK = os.path.join(os.environ.get('RECOMP_BIN') or os.path.join(HERE, '..', 'binaries', '13.0.4'), 'sdk.elf')   # RECOMP_BIN: scripts/setup.py
SDK_BASE = 0x7500000000
d = open(SDK, 'rb').read()
FILE_OFF = 0x1000
dyn = {}
o = 0xc2a8f8
while True:
    tag, val = struct.unpack_from('<qQ', d, o); o += 16
    if tag == 0: break
    dyn[tag] = val
TEXT_SIZE = 0x540950
NSYM = 0x94d40 // 24
R.IMG, R.BASE, R.FILE_OFF, R.TEXT_SIZE = d, SDK_BASE, FILE_OFF, TEXT_SIZE
R.RELA, R.RELASZ, R.JMPREL, R.PLTRELSZ = dyn[7], dyn[8], dyn[23], dyn[2]
R.SYMTAB, R.STRTAB, R.NSYM, R.DYN_ENTRIES, R.MODULE = dyn[6], dyn[5], NSYM, [], 'sdk'

syms, by_addr = {}, {}
for i in range(NSYM):
    name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', d, FILE_OFF + dyn[6] + i * 24)
    s = FILE_OFF + dyn[5] + name_off
    name = d[s:d.index(b'\0', s)].decode('latin-1')
    if shndx and name:
        syms[name] = (value, size, info & 0xf)
        if (info & 0xf) == 2: by_addr.setdefault(SDK_BASE + value, []).append(name)

# data slots that hold the address of a function after relocation (GOT entries, method tables): slot -> target
CODE_SLOTS = {}
for _base, _size in ((dyn[7], dyn[8]),):
    for i in range(_size // 24):
        off, info, addend = struct.unpack_from('<QQq', d, FILE_OFF + _base + i * 24)
        typ, sym = info & 0xffffffff, info >> 32
        if typ == 0x403:
            if 0x200 <= addend < TEXT_SIZE and addend % 4 == 0: CODE_SLOTS[SDK_BASE + off] = SDK_BASE + addend
        elif typ in (0x401, 0x101):
            _, sinfo, _, shndx, value, _ = struct.unpack_from('<IBBHQQ', d, FILE_OFF + dyn[6] + sym * 24)
            if shndx and (sinfo & 0xf) == 2: CODE_SLOTS[SDK_BASE + off] = SDK_BASE + value + (addend if typ == 0x101 else 0)

GAME_IMPORTS = set(l.strip() for l in open(os.path.join(HERE, 'imports_all.txt')) if l.strip())
THROW_HELPER = re.compile(r'^_ZNKSt3__12[01]__(basic_string|vector_base)_commonILb1EE20__throw_(length_error|out_of_range)Ev$')
NORETURN = re.compile(r'Abort|UnexpectedDefault|OnAssertionFailure|__assert_fail|^abort$|__cxa_throw|__cxa_pure_virtual|_Unwind_Resume|__throw_|terminate|^exit$|^_Exit$')


class Config:
    def __init__(self):
        self.roots, self.cut, self.trap, self.lift = [], set(), set(), set()
        p = os.path.join(HERE, 'sdklift_roots.txt')
        if os.path.exists(p):
            for line in open(p):
                line = line.split('#')[0].strip()
                if not line: continue
                parts = line.split()
                if parts[0] == 'cut': self.cut.add(parts[1])
                elif parts[0] == 'trap': self.trap.add(parts[1])
                elif parts[0] == 'lift': self.lift.add(parts[1])
                elif parts[0] == 'addr': self.roots.append(SDK_BASE + int(parts[1], 16))
                else:
                    if parts[0] not in syms: print('sdklift: %s is not a symbol of sdk.elf' % parts[0]); continue
                    self.roots.append(SDK_BASE + syms[parts[0]][0])


class Env(R.Env):
    def __init__(self, cfg, roots):
        R.Env.__init__(self)
        self.cfg = cfg
        self.imports = set()
        self.host_names = []            # names called through the host
        self.host_index = {}
        self.root_set = set(roots)
        self.noreturn_stmt = False
        self.edges = collections.defaultdict(set)
        self.cur = None

    def host(self, name, why):
        if name not in self.host_index:
            self.host_index[name] = len(self.host_names)
            self.host_names.append(name)
        return 'sdk_host_call(c, %d);' % self.host_index[name]

    def resolve(self, target):
        """Follow PLT stubs. Returns (address, None) or (None, imported name)."""
        for _ in range(4):
            plt = self.plt_entry(target)
            if not plt: return target, None
            sym, shndx, value = plt
            if not shndx: return None, self.plt_symbol(target)
            target = SDK_BASE + value
        return target, None

    def call(self, target):
        self.noreturn_stmt = False
        addr, imp = self.resolve(target)
        if imp is not None:                                  # malloc & co: the game's own
            return self.host(imp, 'import')
        names = by_addr.get(addr, [])
        if 'setjmp' in names or '_setjmp' in names:
            return 'SDK_SETJMP(c);'
        if 'longjmp' in names or '_longjmp' in names:
            self.noreturn_stmt = True
            return '{ sdk_longjmp(c); return; }'
        for n in names:
            # close5 (c5b): libc++'s __throw_length_error / __throw_out_of_range of basic_string / vector are imports of
            # the game with host handlers that construct and throw the real exception (nncxxrt.c throw_string_length ...):
            # a real throw through the port's exception runtime instead of a trap. The call does not return.
            if THROW_HELPER.search(n) and n in GAME_IMPORTS and n not in self.cfg.trap:
                self.noreturn_stmt = True
                return '{ %s return; }' % self.host(n, 'throw')
        for n in names:
            if NORETURN.search(n) or n in self.cfg.trap:
                self.noreturn_stmt = True
                return '{ sdk_trap_call(c, "%s"); return; }' % n
        if addr not in self.root_set:
            for n in names:
                if (n in GAME_IMPORTS or n in self.cfg.cut) and n not in self.cfg.lift:
                    return self.host(n, 'cut')
        self.wanted.setdefault(addr, None)
        self.edges[self.cur].add(addr)
        return 'f_%x(c);' % addr


TAILS = ('return;', 'goto', '{ jt', '{ f_', '{ sdk_host_call', '{ sdk_trap_call', '{ sdk_longjmp')


def scan_code_refs(env, entry, end):
    """Text addresses materialised by ADRP+ADD / ADR inside [entry, end) that are function starts
    (callbacks, method tables filled by code)."""
    page, found = {}, []
    for pc in range(entry, end, 4):
        w = R.word(pc)
        if (w & 0x9f000000) == 0x90000000:
            imm = R.sext((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21)
            page[w & 31] = (pc, (pc & ~0xfff) + (imm << 12))
        elif (w & 0xffc00000) == 0x91000000:
            hit = page.get((w >> 5) & 31)
            if hit and pc - hit[0] <= 32:
                t = hit[1] + ((w >> 10) & 0xfff)
                if SDK_BASE + 0x200 <= t < SDK_BASE + TEXT_SIZE and t % 4 == 0: found.append(t)
        elif (w & 0x9f000000) == 0x10000000:
            imm = R.sext((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21)
            t = pc + imm
            if SDK_BASE + 0x200 <= t < SDK_BASE + TEXT_SIZE and t % 4 == 0 and not entry <= t < end: found.append(t)
        elif (w & 0xffc00000) == 0xf9400000:                 # ldr xt, [xn, #imm]: a function pointer from the GOT
            hit = page.get((w >> 5) & 31)
            if hit and pc - hit[0] <= 32:
                t = CODE_SLOTS.get(hit[1] + ((w >> 10) & 0xfff) * 8)
                if t is not None: found.append(t)
    return found


def mul_by_element(w):
    """MUL Vd.T, Vn.T, Vm.Ts[index] (0 Q 0 01111 size L M Rm 1000 H 0 Rn Rd), 16- and 32-bit elements. Returns C or None."""
    if (w & 0xbf00f400) != 0x0f008000: return None
    q, size, l, m, rm, h, rn, rd = (w >> 30) & 1, (w >> 22) & 3, (w >> 21) & 1, (w >> 20) & 1, (w >> 16) & 15, (w >> 11) & 1, (w >> 5) & 31, w & 31
    if size == 2: idx, rm, fld, n, typ = (h << 1) | l, (m << 4) | rm, 'w', 4 if q else 2, 'uint32_t'
    elif size == 1: idx, fld, n, typ = (h << 2) | (l << 1) | m, 'h', 8 if q else 4, 'uint16_t'
    else: return None
    return ('{ V128 a_ = c->v[%d], r_; %s e_ = c->v[%d].%s[%d]; r_.d[0] = 0; r_.d[1] = 0; for (int i_ = 0; i_ < %d; i_++) r_.%s[i_] = (%s)(a_.%s[i_] * e_); c->v[%d] = r_; }'
            % (rn, typ, rm, fld, idx, n, fld, typ, fld, rd))


def addv(w):
    """ADDV Bd/Hd/Sd, Vn.T (0 Q 0 01110 size 11000 11011 10 Rn Rd): sum of the elements, upper bits of Vd cleared.
    nlib's nnmem_nlib_memchr / memrchr / memchr_not (nn::mem::StandardAllocator) use it. Returns C or None."""
    if (w & 0xbf3ffc00) != 0x0e31b800: return None
    q, size, rn, rd = (w >> 30) & 1, (w >> 22) & 3, (w >> 5) & 31, w & 31
    if size == 0: fld, n, typ = 'b', 16 if q else 8, 'uint8_t'
    elif size == 1: fld, n, typ = 'h', 8 if q else 4, 'uint16_t'
    elif size == 2 and q: fld, n, typ = 'w', 4, 'uint32_t'
    else: return None
    return ('{ V128 a_ = c->v[%d]; %s s_ = 0; for (int i_ = 0; i_ < %d; i_++) s_ = (%s)(s_ + a_.%s[i_]); c->v[%d].d[0] = s_; c->v[%d].d[1] = 0; }'
            % (rn, typ, n, typ, fld, rd, rd))


def ushl_vec(w):
    """USHL / SSHL Vd.T, Vn.T, Vm.T (0 Q U 01110 size 1 Rm 010001 Rn Rd): each element shifted by the signed low
    byte of the matching element of Vm: left when positive, right (logical for USHL, arithmetic for SSHL) when
    negative, no rounding, no saturation; a shift of the element size or more gives 0 (or the sign for SSHL right).
    NVN's kind attributes [sdk 0x4204b8] use it. Returns C or None."""
    if (w & 0x9f20fc00) != 0x0e204400: return None
    q, u, size, rm, rn, rd = (w >> 30) & 1, (w >> 29) & 1, (w >> 22) & 3, (w >> 16) & 31, (w >> 5) & 31, w & 31
    fld, bits, ut, st = [('b', 8, 'uint8_t', 'int8_t'), ('h', 16, 'uint16_t', 'int16_t'), ('w', 32, 'uint32_t', 'int32_t'), ('d', 64, 'uint64_t', 'int64_t')][size]
    if size == 3 and not q: return None
    n = (128 if q else 64) // bits
    if u:
        body = 'r_.%s[i_] = s_ >= %d || s_ <= -%d ? 0 : s_ >= 0 ? (%s)(x_ << s_) : (%s)(x_ >> -s_);' % (fld, bits, bits, ut, ut)
        decl = '%s x_ = a_.%s[i_];' % (ut, fld)
    else:
        body = ('r_.%s[i_] = (%s)(s_ >= %d ? 0 : s_ >= 0 ? (%s)((%s)x_ << s_) : s_ <= -%d ? (x_ < 0 ? -1 : 0) : (%s)(x_ >> -s_));'
                % (fld, ut, bits, st, ut, bits, st))
        decl = '%s x_ = (%s)a_.%s[i_];' % (st, st, fld)
    return ('{ V128 a_ = c->v[%d], b_ = c->v[%d], r_; r_.d[0] = 0; r_.d[1] = 0; for (int i_ = 0; i_ < %d; i_++) { int s_ = (int8_t)b_.%s[i_]; %s %s } c->v[%d] = r_; }'
            % (rn, rm, n, fld, decl, body, rd))


def lift_function(env, entry):
    end = env.extent(entry)
    env.cur = entry
    fn = R.Fn(entry, end, env)
    lines, bad = [], []
    pc = entry
    last_noreturn = False
    while pc < end:
        w = R.word(pc)
        env.noreturn_stmt = False
        try:
            stmt = fn.lift(w, pc)
        except R.Unimpl as e:
            stmt = mul_by_element(w)                         # the shared lifter has no MUL (vector, by element): musl's __floatscan / printf_core use it
            if stmt is None: stmt = addv(w)
            if stmt is None: stmt = ushl_vec(w)
            if stmt is None:
                stmt = 'guest_unimpl(0x%xULL, 0x%08xU, "%s");' % (pc, w, e)
                bad.append((pc, w, str(e)))
        last_noreturn = env.noreturn_stmt
        lines.append((pc, stmt))
        pc += 4
    out = ['static void f_%x(Ctx *c) {' % entry, '  c->fn = 0x%xULL;' % entry]
    if fn.has_br: out.append('  uint64_t jt = 0;')
    for pc, stmt in lines: out.append('L_%x: %s' % (pc, stmt))
    falls = not lines[-1][1].startswith(TAILS) and not last_noreturn and R.word(end - 4) != 0 and end < SDK_BASE + TEXT_SIZE
    # an unconditional branch / ret as the last real instruction never falls through
    lastw = R.word(end - 4)
    if (lastw & 0xfc000000) == 0x14000000 or lastw == 0xd65f03c0 or (lastw & 0xfffffc1f) == 0xd61f0000: falls = False
    if falls:
        env.wanted.setdefault(end, None)
        out.append('  f_%x(c); return;' % end)
    else:
        out.append('  return;')
    if fn.has_br:
        out.append('L_dispatch: switch (jt) {')
        out.extend('  case 0x%xULL: goto L_%x;' % (pc, pc) for pc, _ in lines)
        out.append('  default: call_indirect(c, jt); return; }')
    out.append('}')
    for t in scan_code_refs(env, entry, end):
        if t in env.starts_set:
            env.wanted.setdefault(t, None)
    has_svc = any('guest_svc' in s for _, s in lines)
    return '\n'.join(out), len(lines), bad, has_svc


def closure(cfg, roots, limit=200000):
    env = Env(cfg, roots)
    env.compute_starts()
    env.starts_set = set(env.starts)
    for r in roots:
        env.wanted.setdefault(r, None)
        if r not in env.starts_set:
            bisect.insort(env.starts, r); env.starts_set.add(r)
    bodies, done, total, allbad, svc = [], [], 0, [], []
    seen = set()
    while True:
        todo = [a for a in env.wanted if a not in seen]
        if not todo or total > limit: break
        for a in todo:
            body, n, bad, has_svc = lift_function(env, a)
            bodies.append(body); done.append(a); seen.add(a); total += n; allbad += bad
            if has_svc: svc.append(a)
    return env, bodies, done, total, allbad, svc


def label(a):
    return by_addr.get(a, ['sub_%x' % (a - SDK_BASE)])[0]


def main():
    cfg = Config()
    if sys.argv[1] == 'closure':
        for name in sys.argv[2:]:
            matches = [n for n in syms if name == n] or [n for n in syms if name in n and syms[n][2] == 2]
            for n in matches:
                env, bodies, done, total, bad, svc = closure(cfg, [SDK_BASE + syms[n][0]], 30000)
                print('%-70s fns %4d insns %6d bad %d svc %d host: %s' % (n, len(done), total, len(bad), len(svc), ' '.join(env.host_names[:12])))
                if '-v' in sys.argv:
                    for a in done: print('      %x %s' % (a - SDK_BASE, label(a)))
        return
    if sys.argv[1] == 'gen':
        env, bodies, done, total, bad, svc = closure(cfg, cfg.roots, 10 ** 9)
        out = os.path.join(HERE, 'sdklift_gen.c')
        with open(out, 'w') as f:
            f.write('/* generated by sdklift.py from sdk.elf -- do not edit */\n#include "runtime.h"\n#include "sdklift.h"\n\n')
            f.write('static void sdk_call_indirect(Ctx *c, uint64_t a);\n#define call_indirect sdk_call_indirect\n')
            for a in done: f.write('static void f_%x(Ctx *c);\n' % a)
            f.write('\n' + '\n\n'.join(bodies) + '\n\n#undef call_indirect\n')
            f.write('const char *const sdk_host_names[] = {\n')
            for n in env.host_names: f.write('  "%s",\n' % n)
            f.write('  0};\nconst int sdk_host_name_count = %d;\n' % len(env.host_names))
            f.write('const SdkFn sdk_fns[] = {\n')
            for a in sorted(done): f.write('  {0x%xULL, f_%x, "%s"},\n' % (a, a, label(a)))
            f.write('};\nconst int sdk_fn_count = %d;\n' % len(done))
            f.write('static void sdk_call_indirect(Ctx *c, uint64_t a) { sdk_dispatch(c, a); }\n')
        print('functions lifted: %d   instructions: %d   not translated: %d   with svc: %d   host names: %d' % (len(done), total, len(bad), len(svc), len(env.host_names)))
        for a in svc: print('  svc in %x %s' % (a - SDK_BASE, label(a)))
        for pc, w, why in bad[:20]: print('  untranslated %x %08x %s' % (pc - SDK_BASE, w, why))
        with open(os.path.join(HERE, 'sdklift_report.txt'), 'w') as f:
            f.write('lifted %d functions, %d instructions\nhost calls:\n' % (len(done), total))
            for n in env.host_names: f.write('  %s\n' % n)
            f.write('functions:\n')
            for a in sorted(done): f.write('  %x %s\n' % (a - SDK_BASE, label(a)))


if __name__ == '__main__':
    main()
