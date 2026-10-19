"""Differential test of the lifter against the Unicorn AArch64 emulator.

For every instruction shape found in the game binary, a few real instruction
words are executed twice from the same random machine state: once by Unicorn
(reference) and once by the C code recomp.py generates. Final registers, flags
and memory must be identical.

    python difftest.py sample     collect sample instructions from the binary -> difftest_samples.json
    python difftest.py gen        generate difftest_gen.c from the samples
    python difftest.py check      run difftest.exe and Unicorn, compare
    python difftest.py sample-nro <nro> [<nro> ...]
                                  also sample the shapes of NRO modules (e.g. Skyline plugins) -> difftest_samples_nro.json;
                                  `gen` then appends their shapes that the game does not have

`gen` appends to the sampled shapes the instruction words of extra_words() (every valid arrangement of the instructions
the game's code does not use but Rust-compiled modules do) and short exclusive sequences (LDXP + STXP, LDXR + STXR ...),
each from a random state.

difftest.exe is difftest_gen.c + difftest_host.c compiled with the flags of the build under test, e.g.
    C:\\llvm-8.0.0\\bin\\clang.exe -O0 -w -fno-strict-aliasing -I . difftest_gen.c difftest_host.c -o difftest.exe
(DIFFTEST_EXE=<path> selects another exe for `check`; DIFFTEST_NO_SPECIAL=1 makes `gen` write only the old list,
DIFFTEST_NO_EXTRA=1 the list without those additions; DIFFTEST_LIFTER=recomp_nro lifts with tools/nro/recomp_nro.py,
whose instruction translation is a copy of recomp.py's. The tests of a list keep their index, so old indices stay valid.)
"""
import collections
import json
import os
import re
import struct
import subprocess
import sys
import zlib

import recomp

LIFT = recomp                                              # the lifter under test (Fn, Env, Unimpl, bits)
if os.environ.get('DIFFTEST_LIFTER') == 'recomp_nro':
    _here = os.path.dirname(os.path.abspath(__file__))
    sys.path[:0] = [os.path.join(_here, 'ro_study'), os.path.join(_here, '..', 'nro')]   # build\work layout, repo layout
    import recomp_nro as LIFT

REGION, REGION_SIZE = 0x7200000000, 0x400000
CODE, DATA = REGION + 0x200000, REGION + 0x300000
WIN_LO, WIN_HI = DATA - 0x1000, DATA + 0x12000
M64 = (1 << 64) - 1
PER_SHAPE = 3
HOST_EXE = os.environ.get('DIFFTEST_EXE') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'difftest.exe')


class Rng:
    def __init__(self, seed):
        self.s = (seed * 0x9E3779B97F4A7C15 + 0x1234567) & M64

    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & M64
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        return z ^ (z >> 31)


def pattern():
    r = Rng(0xABCDEF)
    return b''.join(struct.pack('<Q', r.next()) for _ in range(REGION_SIZE // 8))


def initial_state(idx, mem, rn, rm):
    r = Rng(idx)
    x = [r.next() for _ in range(31)] + [DATA + 0x800]
    v = []
    for _ in range(32):
        lanes = [(r.next() % 200001 - 100000) / 64.0 for _ in range(4)]
        v.append(struct.pack('<4f', *lanes))
    flags = r.next() & 0xf
    if mem:
        base = DATA + 0x100 * (r.next() % 8)
        index = r.next() % 0x40
        if rn != 31:
            x[rn] = base
        if 0 <= rm < 31 and rm != rn:
            x[rm] = index
    return x, v, flags


# ---- floating-point special operands (added for the FMAX/FMIN and FCVT-half fixes) ----------------------------------
# Random states never contain a NaN, a signed zero, a subnormal or a half-precision rounding tie, so the instructions
# below are also run from states built out of these tables: operand registers get every pair (two-operand instructions)
# or every value (one-operand ones). The first tests of the list are the old random-state tests, in the old order.
SPECIAL = {32: [0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x40200000, 0xc0200000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fc12345, 0x7f800001, 0xffa00000, 0x00000001, 0x007fffff, 0x80000400, 0x00800000, 0x7f7fffff, 0x477fe000, 0x477ff000, 0x477fefff, 0x33800000, 0x33000000, 0x33000001, 0x38800000, 0x387fffff, 0x3f801000, 0x3f803000, 0x3f801001],
           64: [0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000, 0x4004000000000000, 0xc004000000000000, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000000, 0x7ff8000000012345, 0x7ff0000000000001, 0xfff4000000000000, 0x0000000000000001, 0x000fffffffffffff, 0x8000000000000400, 0x0010000000000000, 0x7fefffffffffffff, 0x40effc0000000000, 0x40effe0000000000, 0x40effdffffffffff, 0x3e70000000000000, 0x3e60000000000000, 0x3e60000000000001, 0x3f10000000000000, 0x3f0fffffffffffff, 0x3ff0020000000000, 0x3ff0060000000000, 0x3ff0020000001000],
           16: [0x0000, 0x8000, 0x3c00, 0xbc00, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7e01, 0x7c01, 0xfd55, 0x0001, 0x03ff, 0x8200, 0x0400, 0x7bff, 0x3555, 0xc123]}
FP_SPECIAL_MNEMONICS = ('fmax', 'fmin', 'fmaxnm', 'fminnm', 'fcvt')
if os.environ.get('DIFFTEST_FP_ALL'):                    # close5: NaN operand selection of every arithmetic instruction (measures the row 'NaN propagation')
    FP_SPECIAL_MNEMONICS += ('fadd', 'fsub', 'fmul', 'fdiv', 'fnmul', 'fmadd', 'fmsub', 'fnmadd', 'fnmsub', 'fsqrt', 'fabd', 'faddp', 'fmulx', 'fmla', 'fmls',
                             'frinta', 'frinti', 'frintm', 'frintn', 'frintp', 'frintx', 'frintz', 'fmaxp', 'fminp', 'fmaxnmp', 'fminnmp', 'frecpe', 'frsqrte', 'fneg', 'fabs')
# Encodings the game does not contain but the lifter translates (other directions / widths of the same instructions).
FP_EXTRA_WORDS = [0x1e63c020, 0x1ee24020, 0x1ee2c020, 0x1e624820, 0x1e627820, 0x1e625820, 0x1e626820,
                  0x4e22c420, 0x4ea2c420, 0x4e62f420, 0x4ee2f420, 0x4e62c420, 0x4ee2c420, 0x0e22f420, 0x0ea2f420]


def fp_mode(w):
    """Width of the source operands of a floating-point instruction word: 16, 32 or 64."""
    if recomp.bits(w, 28, 24) == 0x1e:                     # scalar
        return {0: 32, 1: 64, 3: 16}.get(recomp.bits(w, 23, 22), 0)
    return 64 if recomp.bits(w, 22, 22) else 32            # vector three-same


def special_v(t):
    """The 32 vector registers of special test t: lane l of register r holds table[(base + 7 * l) % n]."""
    mode, k = t['fpmode'], t['k']
    table = SPECIAL[mode]
    n = len(table)
    ia, ib = k % n, (k // n) % n
    v = []
    for r in range(32):
        b = ib if r == t['frm'] else ia if r == t['rn'] else (k + r) % n
        lanes = [table[(b + 7 * l) % n] for l in range(128 // mode)]
        v.append(b''.join(x.to_bytes(mode // 8, 'little') for x in lanes))
    return v


def build_special(env_fn, old):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    words = [(t['key'], t['word']) for t in old if t['key'].split()[0] in FP_SPECIAL_MNEMONICS]
    shapes = json.load(open('difftest_samples.json'))
    for key, ws in sorted(shapes.items()):                 # words the old lifter could not translate (fcvt h, s)
        if key.split()[0] in FP_SPECIAL_MNEMONICS:
            words += [(key, w) for w in ws if (key, w) not in words]
    for w in FP_EXTRA_WORDS:
        ins = list(md.disasm(struct.pack('<I', w), CODE))
        words.append(('(not in the game) %s %s' % (ins[0].mnemonic, ins[0].op_str), w))
    tests = []
    for key, w in words:
        try:
            stmt = env_fn.lift(w, CODE)
        except LIFT.Unimpl:                                # counted as different: the host does nothing
            stmt = 'guest_unimpl(0, 0, "untranslated");'
        mode = fp_mode(w)
        if mode not in SPECIAL: continue                   # close5: not a floating-point operand width (DIFFTEST_FP_ALL)
        unary = key.split()[0] == 'fcvt' or 'fcvt' in key
        n = len(SPECIAL[mode])
        for k in range(n if unary else n * n):
            tests.append({'key': key + ' [special]', 'word': w, 'stmt': stmt, 'mem': 0, 'rn': recomp.bits(w, 9, 5), 'rm': -1,
                          'fpmode': mode, 'k': k, 'frm': -1 if unary else recomp.bits(w, 20, 16)})
    return tests


def cmd_sample():
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    text = recomp.IMG[recomp.FILE_OFF:recomp.FILE_OFF + recomp.TEXT_SIZE]
    seen = set()
    shapes = collections.defaultdict(list)
    for (w,) in struct.iter_unpack('<I', text):
        if w in seen:
            continue
        seen.add(w)
        ins = list(md.disasm(struct.pack('<I', w), CODE))
        if not ins:
            continue
        shape = re.sub(r'#-?(0x)?[0-9a-f.e+-]+', '#', ins[0].op_str)
        shape = re.sub(r'\b([vqdshbwx])\d+\b', r'\1', shape)
        key = ins[0].mnemonic + ' ' + shape
        if len(shapes[key]) < PER_SHAPE:
            shapes[key].append(w)
    json.dump(shapes, open('difftest_samples.json', 'w'))
    print('shapes:', len(shapes), ' sample words:', sum(len(v) for v in shapes.values()))


def build_tests():
    shapes = json.load(open('difftest_samples.json'))
    env = LIFT.Env()
    env.imports = set()
    fn = LIFT.Fn(CODE, CODE + 4, env)
    tests = []
    for key, words in sorted(shapes.items()):
        for w in words:
            op0 = recomp.bits(w, 28, 25)
            if op0 in (10, 11):
                continue                                  # branches / system: not straight-line
            mem = op0 in (4, 6, 12, 14)
            if mem and recomp.bits(w, 29, 24) == 8 and not recomp.bits(w, 26, 26):
                continue                                  # exclusives: single-thread model differs by design
            try:
                stmt = fn.lift(w, CODE)
            except LIFT.Unimpl:
                continue
            if any(t in stmt for t in ('goto', 'return', 'call_indirect', 'guest_', 'f_7')):
                continue
            rn = recomp.bits(w, 9, 5)
            rm = -1
            if mem and recomp.bits(w, 29, 27) == 7 and recomp.bits(w, 24, 24) == 0 and recomp.bits(w, 21, 21) and recomp.bits(w, 11, 10) == 2:
                rm = recomp.bits(w, 20, 16)
            tests.append({'key': key, 'word': w, 'stmt': stmt, 'mem': int(mem), 'rn': rn, 'rm': rm, 'fpmode': 0, 'k': 0, 'frm': -1})
    if os.environ.get('DIFFTEST_NO_SPECIAL'):              # the old list only (3,120 tests before the FP fixes)
        return tests
    tests += build_special(fn, tests)
    if os.environ.get('DIFFTEST_NO_EXTRA'):
        return tests
    return tests + build_extra(fn, shapes)


# ---- instructions the game's code does not use (Rust-compiled modules do): every valid arrangement written out here,
# plus sequences around the exclusive monitor. Appended after the sampled list, so the old indices stay valid.
def _shape_key(md, w, prefix=''):
    ins = list(md.disasm(struct.pack('<I', w), CODE))
    if not ins:
        return None
    shape = re.sub(r'#-?(0x)?[0-9a-f.e+-]+', '#', ins[0].op_str)
    shape = re.sub(r'\b([vqdshbwx])\d+\b', r'\1', shape)
    return prefix + ins[0].mnemonic + ' ' + shape


def cmd_sample_nro(paths):
    """Shapes of NRO modules (Skyline plugins): `gen` adds those the game does not have (key '(plugin) ...')."""
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    shapes = collections.defaultdict(list)
    seen = set()
    for p in paths:
        d = open(p, 'rb').read()
        assert d[0x10:0x14] == b'NRO0', p
        text_off, text_size = struct.unpack_from('<II', d, 0x20)
        for (w,) in struct.iter_unpack('<I', d[text_off + 0x80:text_off + text_size]):   # past the NRO header
            if w in seen:
                continue
            seen.add(w)
            key = _shape_key(md, w)
            if key and len(shapes[key]) < PER_SHAPE:
                shapes[key].append(w)
    json.dump(shapes, open('difftest_samples_nro.json', 'w'))
    print('NRO shapes:', len(shapes), ' sample words:', sum(len(v) for v in shapes.values()))


def _regs(i, n, avoid=()):
    """n distinct register numbers 0..30 (a deterministic spread), none in avoid."""
    out, r = [], (i * 7 + 3) % 31
    while len(out) < n:
        if r not in out and r not in avoid:
            out.append(r)
        r = (r + 11) % 31
    return out


def extra_words():
    """(word, fp special-operand width or 0) for every valid arrangement of the added instructions."""
    out = []
    v3 = lambda q, u, size, rm, opc, rn, rd: (q << 30) | (u << 29) | (0x0e << 24) | (size << 22) | (1 << 21) | (rm << 16) | (opc << 11) | (1 << 10) | (rn << 5) | rd
    s3 = lambda u, size, rm, opc, rn, rd: (1 << 30) | (u << 29) | (0x1e << 24) | (size << 22) | (1 << 21) | (rm << 16) | (opc << 11) | (1 << 10) | (rn << 5) | rd
    v2 = lambda q, u, size, opc, rn, rd: (q << 30) | (u << 29) | (0x0e << 24) | (size << 22) | (0x10 << 17) | (opc << 12) | (2 << 10) | (rn << 5) | rd
    s2 = lambda u, size, opc, rn, rd: (1 << 30) | (u << 29) | (0x1e << 24) | (size << 22) | (0x10 << 17) | (opc << 12) | (2 << 10) | (rn << 5) | rd
    vx = lambda q, u, size, opc, rn, rd: (q << 30) | (u << 29) | (0x0e << 24) | (size << 22) | (0x18 << 17) | (opc << 12) | (2 << 10) | (rn << 5) | rd
    sp = lambda u, size, opc, rn, rd: (1 << 30) | (u << 29) | (0x1e << 24) | (size << 22) | (0x18 << 17) | (opc << 12) | (2 << 10) | (rn << 5) | rd
    vsh = lambda q, u, immhb, opc, rn, rd: (q << 30) | (u << 29) | (0x1e << 23) | (immhb << 16) | (opc << 11) | (1 << 10) | (rn << 5) | rd
    ssh = lambda u, immhb, opc, rn, rd: (1 << 30) | (u << 29) | (0x3e << 23) | (immhb << 16) | (opc << 11) | (1 << 10) | (rn << 5) | rd
    arr = [(q, size) for size in range(4) for q in (0, 1) if not (size == 3 and q == 0)]     # 8b 16b 4h 8h 2s 4s 2d
    k = 0
    for u, opc in [(0, 0x08), (1, 0x08), (0, 0x0c), (1, 0x0c), (0, 0x0d), (1, 0x0d), (0, 0x17), (0, 0x14), (1, 0x14), (0, 0x15), (1, 0x15)]:
        for q, size in arr:
            if size == 3 and opc in (0x0c, 0x0d, 0x14, 0x15):
                continue
            for j in range(2):
                rd, rn, rm = _regs(k, 3); k += 1
                if j:
                    rd = rn                               # destination = source
                out.append((v3(q, u, size, rm, opc, rn, rd), 0))
        if opc == 0x08:
            rd, rn, rm = _regs(k, 3); k += 1
            out.append((s3(u, 3, rm, opc, rn, rd), 0))   # sshl / ushl d
    for u, opc in [(0, 0x08), (1, 0x08), (1, 0x09), (0, 0x0a)]:          # cmgt / cmge / cmle / cmlt #0
        for q, size in arr:
            for j in range(2):
                rd, rn = _regs(k, 2); k += 1
                out.append((v2(q, u, size, opc, rn, rn if j else rd), 0))
        rd, rn = _regs(k, 2); k += 1
        out.append((s2(u, 3, opc, rn, rd), 0))
    for u in (0, 1):                                                      # [su]addlp, [su]adalp
        for opc in (0x02, 0x06):
            for q, size in arr:
                if size == 3:
                    continue
                for j in range(2):
                    rd, rn = _regs(k, 2); k += 1
                    out.append((v2(q, u, size, opc, rn, rn if j else rd), 0))
    for opc in (0x16, 0x17):                                              # fcvtn(2) / fcvtl(2)
        for sz in (0, 1):
            for q in (0, 1):
                for j in range(2):
                    rd, rn = _regs(k, 2); k += 1
                    mode = (16 if sz == 0 else 32) if opc == 0x17 else (32 if sz == 0 else 64)
                    out.append((v2(q, 0, sz, opc, rn, rn if j else rd), mode))
    for u, opc in [(0, 0x1b), (0, 0x03), (1, 0x03), (0, 0x0a), (1, 0x0a), (0, 0x1a), (1, 0x1a)]:   # across lanes
        for q, size in arr:
            if size == 3 or (size == 2 and not q):
                continue
            for j in range(2):
                rd, rn = _regs(k, 2); k += 1
                out.append((vx(q, u, size, opc, rn, rn if j else rd), 0))
    for opc in (0x0c, 0x0f):                                              # fmaxnmv / fminnmv / fmaxv / fminv
        for o1 in (0, 1):
            for j in range(2):
                rd, rn = _regs(k, 2); k += 1
                out.append((vx(1, 1, o1 << 1, opc, rn, rn if j else rd), 32))
    for j in range(3):                                                    # addp d, v.2d
        rd, rn = _regs(k, 2); k += 1
        out.append((sp(0, 3, 0x1b, rn, rn if j == 2 else rd), 0))
    for u, opc in [(0, 0x02), (1, 0x02), (0, 0x04), (1, 0x04), (0, 0x06), (1, 0x06), (1, 0x08), (1, 0x0a), (0, 0x10), (0, 0x11)]:
        narrow = opc in (0x10, 0x11)
        for es in range(3 if narrow else 4):
            ebits = 8 << es
            for q in (0, 1):
                if es == 3 and not q:
                    continue
                left = opc == 0x0a
                amounts = sorted({0, 1, ebits // 2, ebits - 1}) if left else sorted({1, 2, ebits // 2, ebits - 1, ebits})
                for amt in amounts:
                    immhb = (ebits + amt) if left else (2 * ebits - amt)
                    rd, rn = _regs(k, 2); k += 1
                    out.append((vsh(q, u, immhb, opc, rn, rd), 0))
        if not narrow:                                                    # scalar d forms
            for amt in ((0, 1, 33, 63) if opc == 0x0a else (1, 2, 31, 63, 64)):
                immhb = (64 + amt) if opc == 0x0a else (128 - amt)
                rd, rn = _regs(k, 2); k += 1
                out.append((ssh(u, immhb, opc, rn, rd), 0))
    for opc in range(0x10, 0x18):                                         # crc32 / crc32c b h w x
        sf = 1 if (opc & 3) == 3 else 0
        for j in range(3):
            rd, rn, rm = _regs(k, 3); k += 1
            if j == 2:
                rd = rn
            out.append(((sf << 31) | (0xd6 << 21) | (rm << 16) | (opc << 10) | (rn << 5) | rd, 0))
    return out


def exclusive_sequences():
    """Short sequences around the exclusive monitor: (words, base register). The base is set to an aligned address of
    the data window by the test state (mem = 1)."""
    seqs = []
    ldxp = lambda sz, o0, rt, rt2, rn: (1 << 31) | (sz << 30) | (0x08 << 24) | (1 << 22) | (1 << 21) | (31 << 16) | (o0 << 15) | (rt2 << 10) | (rn << 5) | rt
    stxp = lambda sz, o0, rs, rt, rt2, rn: (1 << 31) | (sz << 30) | (0x08 << 24) | (1 << 21) | (rs << 16) | (o0 << 15) | (rt2 << 10) | (rn << 5) | rt
    ldxr = lambda size, o0, rt, rn: (size << 30) | (0x08 << 24) | (1 << 22) | (31 << 16) | (o0 << 15) | (31 << 10) | (rn << 5) | rt
    stxr = lambda size, o0, rs, rt, rn: (size << 30) | (0x08 << 24) | (rs << 16) | (o0 << 15) | (31 << 10) | (rn << 5) | rt
    ldar = lambda size, rt, rn: (size << 30) | (0x08 << 24) | (1 << 23) | (1 << 22) | (31 << 16) | (1 << 15) | (31 << 10) | (rn << 5) | rt
    stlr = lambda size, rt, rn: (size << 30) | (0x08 << 24) | (1 << 23) | (31 << 16) | (1 << 15) | (31 << 10) | (rn << 5) | rt
    add16 = lambda rn: 0x91004000 | (rn << 5) | rn                    # add xN, xN, #16
    k = 0
    for sz in (0, 1):
        for o0l in (0, 1):
            for o0s in (0, 1):
                rn, rt, rt2, rs, a, b = _regs(k, 6); k += 1
                seqs.append(([ldxp(sz, o0l, rt, rt2, rn)], rn))                                        # the load alone
                seqs.append(([ldxp(sz, o0l, rt, rt2, rn), stxp(sz, o0s, rs, a, b, rn)], rn))           # succeeds
                seqs.append(([ldxp(sz, o0l, rt, rt2, rn), add16(rn), stxp(sz, o0s, rs, a, b, rn)], rn))   # other address: fails
                st = (0xf9000000 if sz else 0xb9000000) | (rn << 5) | a                                # str x/w a, [rn]: value changed
                seqs.append(([ldxp(sz, o0l, rt, rt2, rn), st, stxp(sz, o0s, rs, rt, b, rn)], rn))      # fails (value compare)
        seqs.append(([ldxp(sz, 0, 1, 2, 31), stxp(sz, 1, 3, 4, 5, 31)], 31))                          # base sp
    for size in range(4):
        rn, rt, rs, a = _regs(k, 4); k += 1
        for o0 in (0, 1):
            seqs.append(([ldxr(size, o0, rt, rn), stxr(size, o0, rs, a, rn)], rn))
        seqs.append(([ldar(size, rt, rn)], rn))
        seqs.append(([stlr(size, a, rn)], rn))
    return seqs


def build_extra(fn, main_shapes):
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    words = []                                                            # (key, word, fp mode)
    if os.path.exists('difftest_samples_nro.json'):                       # sample-nro: shapes of modules the game does not have
        main_words = set(w for ws in main_shapes.values() for w in ws)
        for key, ws in sorted(json.load(open('difftest_samples_nro.json')).items()):
            if key not in main_shapes:
                words += [('(plugin) ' + key, w, 0) for w in ws if w not in main_words]
    words += [(_shape_key(md, w, '(added) ') or '(added) %08x' % w, w, mode) for w, mode in extra_words()]
    tests = []
    for key, w, mode in words:
        op0 = LIFT.bits(w, 28, 25)
        if op0 in (10, 11):
            continue
        mem = op0 in (4, 6, 12, 14)
        if mem and LIFT.bits(w, 29, 24) == 8 and not LIFT.bits(w, 26, 26):
            continue                                                      # exclusives: as sequences below
        try:
            stmt = fn.lift(w, CODE)
        except LIFT.Unimpl:
            if key.startswith('(added)'):
                raise SystemExit('extra_words: %08x (%s) is not translated' % (w, key))
            continue                                                       # a sampled module shape the lifter does not translate
        if any(t in stmt for t in ('goto', 'return', 'call_indirect', 'guest_', 'f_7')):
            continue
        rn, rm = LIFT.bits(w, 9, 5), -1
        if mem and LIFT.bits(w, 29, 27) == 7 and LIFT.bits(w, 24, 24) == 0 and LIFT.bits(w, 21, 21) and LIFT.bits(w, 11, 10) == 2:
            rm = LIFT.bits(w, 20, 16)
        tests.append({'key': key, 'word': w, 'stmt': stmt, 'mem': int(mem), 'rn': rn, 'rm': rm, 'fpmode': 0, 'k': 0, 'frm': -1})
        if mode:                                                          # one-operand floating point: every special value
            for kk in range(len(SPECIAL[mode])):
                tests.append({'key': key + ' [special]', 'word': w, 'stmt': stmt, 'mem': 0, 'rn': rn, 'rm': -1,
                              'fpmode': mode, 'k': kk, 'frm': -1})
    for ws, rn in exclusive_sequences():
        stmts = [fn.lift(w, CODE + 4 * i) for i, w in enumerate(ws)]
        key = '(sequence) ' + ' ; '.join(_shape_key(md, w) for w in ws)
        tests.append({'key': key, 'word': ws[0], 'words': ws, 'stmt': ' '.join(stmts), 'mem': 1, 'rn': rn, 'rm': -1,
                      'fpmode': 0, 'k': 0, 'frm': -1})
    return tests


def cmd_gen():
    tests = build_tests()
    with open('difftest_gen.c', 'w') as f:
        f.write('#include "runtime.h"\n\n')
        for i, t in enumerate(tests):
            f.write('static void t_%d(Ctx *c) { %s }\n' % (i, t['stmt']))
        f.write('\ntypedef struct { void (*fn)(Ctx *); uint32_t word; int mem, rn, rm, fpmode, k, frm; } Test;\n')
        f.write('const Test tests[] = {\n')
        for i, t in enumerate(tests):
            f.write('  { t_%d, 0x%08xU, %d, %d, %d, %d, %d, %d },\n' % (i, t['word'], t['mem'], t['rn'], t['rm'], t['fpmode'], t['k'], t['frm']))
        f.write('};\nconst int test_count = %d;\n' % len(tests))
    json.dump(tests, open('difftest_tests.json', 'w'))
    print('tests generated: %d (random state: %d, floating-point special operands: %d; module shapes, added instructions and sequences: %d)'
          % (len(tests), sum(1 for t in tests if not t['fpmode']), sum(1 for t in tests if t['fpmode']),
             sum(1 for t in tests if t['key'].startswith(('(plugin)', '(added)', '(sequence)')))))


def cmd_check():
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UcError
    from unicorn import arm64_const as A
    tests = json.load(open('difftest_tests.json'))
    out = subprocess.run([HOST_EXE], capture_output=True, text=True).stdout.split('\n')
    host = {}
    for line in out:
        p = line.split()
        if len(p) == 3:
            host[int(p[0])] = (int(p[1], 16), int(p[2], 16))
    pat = pattern()
    window = pat[WIN_LO - REGION:WIN_HI - REGION]
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(REGION, REGION_SIZE)
    uc.mem_write(REGION, pat)
    uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
    xr = [getattr(A, 'UC_ARM64_REG_X%d' % i) for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
    vr = [getattr(A, 'UC_ARM64_REG_V%d' % i) for i in range(32)]
    bad = collections.defaultdict(list)
    ok = errors = ok_special = 0
    ok_cat, n_cat = collections.Counter(), collections.Counter()
    for i, t in enumerate(tests):
        cat = t['key'].split(')')[0] + ')' if t['key'].startswith('(') and not t['key'].startswith('(not') else 'game'
        n_cat[cat] += 1
        x, v, flags = initial_state(i, t['mem'], t['rn'], t['rm'])
        if t.get('fpmode'):
            v = special_v(t)
        for r, val in zip(xr, x[:31]):
            uc.reg_write(r, val)
        uc.reg_write(A.UC_ARM64_REG_SP, x[31])
        for r, val in zip(vr, v):
            uc.reg_write(r, int.from_bytes(val, 'little'))
        uc.reg_write(A.UC_ARM64_REG_NZCV, flags << 28)
        uc.mem_write(WIN_LO, window)
        ws = t.get('words') or [t['word']]                 # a sequence (exclusives) or one instruction
        uc.mem_write(CODE, b''.join(struct.pack('<I', w) for w in ws))
        try:
            uc.emu_start(CODE, CODE + 4 * len(ws), count=len(ws))
        except UcError as e:
            errors += 1
            bad['(reference emulator fault: %s) %s' % (e, t['key'])].append(i)
            continue
        regs = b''.join(struct.pack('<Q', uc.reg_read(r)) for r in xr) + struct.pack('<Q', uc.reg_read(A.UC_ARM64_REG_SP))
        regs += b''.join(uc.reg_read(r).to_bytes(16, 'little') for r in vr)
        regs += struct.pack('<I', (uc.reg_read(A.UC_ARM64_REG_NZCV) >> 28) & 0xf)
        ref = (zlib.crc32(regs), zlib.crc32(bytes(uc.mem_read(WIN_LO, WIN_HI - WIN_LO))))
        if host.get(i) == ref:
            ok += 1
            ok_special += 1 if t.get('fpmode') else 0
            ok_cat[cat] += 1
        else:
            bad[t['key']].append(i)
    n_special = sum(1 for t in tests if t.get('fpmode'))
    print('tests: %d   identical: %d   different: %d   (reference faults: %d)' % (len(tests), ok, len(tests) - ok, errors))
    print('  random-state tests: %d identical of %d   floating-point special-operand tests: %d identical of %d'
          % (ok - ok_special, len(tests) - n_special, ok_special, n_special))
    for cat in ('(plugin)', '(added)', '(sequence)'):
        if n_cat[cat]:
            print('  %-10s tests: %d identical of %d' % (cat, ok_cat[cat], n_cat[cat]))
    for key, idxs in sorted(bad.items(), key=lambda kv: -len(kv[1]))[:60]:
        t = tests[idxs[0]]
        print('  %-52s %d   e.g. #%d %08x' % (key[:52], len(idxs), idxs[0], t['word']))
    json.dump({k: v for k, v in bad.items()}, open('difftest_bad.json', 'w'))


def cmd_show(idx):
    """Print reference vs host state for one test (host state via `difftest.exe <idx>`)."""
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn import arm64_const as A
    tests = json.load(open('difftest_tests.json'))
    t = tests[idx]
    print('#%d %08x %s\n  %s' % (idx, t['word'], t['key'], t['stmt']))
    pat = pattern()
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(REGION, REGION_SIZE)
    uc.mem_write(REGION, pat)
    uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
    xr = [getattr(A, 'UC_ARM64_REG_X%d' % i) for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
    vr = [getattr(A, 'UC_ARM64_REG_V%d' % i) for i in range(32)]
    x, v, flags = initial_state(idx, t['mem'], t['rn'], t['rm'])
    if t.get('fpmode'):
        v = special_v(t)
    for r, val in zip(xr, x[:31]):
        uc.reg_write(r, val)
    uc.reg_write(A.UC_ARM64_REG_SP, x[31])
    for r, val in zip(vr, v):
        uc.reg_write(r, int.from_bytes(val, 'little'))
    uc.reg_write(A.UC_ARM64_REG_NZCV, flags << 28)
    ws = t.get('words') or [t['word']]
    uc.mem_write(CODE, b''.join(struct.pack('<I', w) for w in ws))
    uc.emu_start(CODE, CODE + 4 * len(ws), count=len(ws))
    ref = {}
    for i, r in enumerate(xr):
        ref['x%d' % i] = uc.reg_read(r)
    ref['sp'] = uc.reg_read(A.UC_ARM64_REG_SP)
    for i, r in enumerate(vr):
        ref['v%d' % i] = uc.reg_read(r)
    ref['nzcv'] = (uc.reg_read(A.UC_ARM64_REG_NZCV) >> 28) & 0xf
    host = {}
    for line in subprocess.run([HOST_EXE, str(idx)], capture_output=True, text=True).stdout.split('\n'):
        p = line.split()
        if len(p) == 2:
            host[p[0]] = int(p[1], 16)
    init = {('x%d' % i): x[i] for i in range(31)}
    init['sp'] = x[31]
    init.update({('v%d' % i): int.from_bytes(v[i], 'little') for i in range(32)})
    init['nzcv'] = flags
    for k in ref:
        if ref[k] != host.get(k):
            print('  %-5s init %x\n        ref  %x\n        host %x' % (k, init[k], ref[k], host.get(k, -1)))


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'sample':
        cmd_sample()
    elif cmd == 'gen':
        cmd_gen()
    elif cmd == 'check':
        cmd_check()
    elif cmd == 'sample-nro':
        cmd_sample_nro(sys.argv[2:])
    else:
        cmd_show(int(sys.argv[2]))
