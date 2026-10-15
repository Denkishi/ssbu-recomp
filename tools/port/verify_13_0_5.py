"""Check every game address the runtime names against a 13.0.5 main.elf (docs/VERSIONS.md).

    python tools/port/verify_13_0_5.py <13.0.5 main.elf> <functions.csv>

The sources name each game address by its 13.0.4 value and pass it through GA() (runtime/hostrt.c), which gives the
address in the build that runs. This script finds those addresses in runtime\\ and fighter_sdk\\ and checks what GA()
answers on 13.0.5:
  * code (below 0x39c8000): the address must be in hostrt.c's table g_code_13_0_5 (ga() stops the program on any other),
    and the table's delta must be the one at which the same-size functions of the decompilation list line up in 13.0.5's
    own unwind table (.eh_frame_hdr), on both sides of the address or for the address's own function
  * read-only data, data and bss: one page (0x1000) later; 13.0.5 code must form the new address (ADRP + ADD/LDR/STR)
    more often than the old one
It also checks that the layout rule of ga() holds (segments one page later, same sizes) and that no 0x71xxxxxxxx literal
in code bypasses GA() unless it is listed below with its reason.
<functions.csv> is data/functions.csv of the matching decompilation (sbergeron42/ssbu-decomp): 13.0.4 function addresses
and sizes (names and addresses only, no code). Nothing of the game is stored here; both inputs come from outside.
Exit code 1 when anything fails."""
import bisect
import collections
import csv
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASE = 0x7100000000
RO4 = 0x39c8000          # 13.0.4: start of the read-only data segment; everything below is code
TEXT4 = 0x39c7e90        # 13.0.4: size of the code
# literals that are not passed through GA(), and why
RAW = {0x0: 'the image base',
       0x1000: 'a test value in the first page of code (cxxrt_selftest.h), the same in every build',
       0x4000000: 'a test value outside any function (cxxrt_selftest.h): read-only data in both builds',
       0x147f330: 'g_nm_path_fn[] (newmodule_host.c): GA() where the array is used',
       0x17df5f0: 'g_nm_path_fn[]', 0x17dfe50: 'g_nm_path_fn[]', 0x353e330: 'g_nm_path_fn[]'}


class Elf:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        phoff, = struct.unpack_from('<Q', self.b, 0x20)
        phnum, = struct.unpack_from('<H', self.b, 0x38)
        self.segs = []
        for i in range(phnum):
            t, _, off, va, _, fsz, msz, _ = struct.unpack_from('<IIQQQQQQ', self.b, phoff + i * 56)
            if t == 1:
                self.segs.append((va, off, fsz, msz))

    def rd(self, va, n):
        for v, o, s, _ in self.segs:
            if v <= va < v + s:
                return self.b[o + va - v:o + va - v + n]
        raise KeyError(hex(va))

    def u32(self, va):
        return struct.unpack('<I', self.rd(va, 4))[0]

    def s32(self, va):
        return struct.unpack('<i', self.rd(va, 4))[0]


def unwind_functions(e):
    """{start: size} from .eh_frame_hdr (found through MOD0)"""
    m = e.u32(4)
    assert e.rd(m, 4) == b'MOD0'
    eh = m + e.s32(m + 16)
    assert tuple(e.rd(eh, 4)[1:]) == (0x1b, 0x03, 0x3b), 'unexpected .eh_frame_hdr encoding'
    out = {}
    for i in range(e.u32(eh + 8)):
        loc = eh + e.s32(eh + 12 + i * 8)
        fde = eh + e.s32(eh + 16 + i * 8)
        out[loc] = e.u32(fde + 12)
    return out


def adrp_targets(e, text_size):
    """how often code forms each address with ADRP + ADD / LDR / STR (unsigned offset) on the same register"""
    words = struct.unpack('<%dI' % (text_size // 4), e.rd(0, text_size // 4 * 4))
    refs = collections.Counter()
    for i, x in enumerate(words):
        if (x & 0x9f000000) != 0x90000000:
            continue
        rd = x & 31
        imm = (((x >> 5) & 0x7ffff) << 2) | ((x >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        page = ((i * 4) & ~0xfff) + (imm << 12)
        for y in words[i + 1:i + 6]:
            if (y >> 5) & 31 != rd:
                continue
            if (y & 0xff800000) == 0x91000000:
                refs[page + ((y >> 10) & 0xfff)] += 1
            elif (y & 0x3b000000) == 0x39000000:
                refs[page + (((y >> 10) & 0xfff) << ((y >> 30) & 3))] += 1
            break
    return refs


def code_table():
    """g_code_13_0_5 of runtime/hostrt.c: {13.0.4 offset: 13.0.5 offset}"""
    s = open(os.path.join(ROOT, 'runtime', 'hostrt.c'), encoding='utf-8').read()
    block = s[s.index('g_code_13_0_5[][2] = {'):]
    block = block[:block.index('};')]
    pairs = [(int(a, 16), int(b, 16)) for a, b in re.findall(r'\{0x([0-9a-fA-F]+), 0x([0-9a-fA-F]+)\}', block)]
    assert [a for a, _ in pairs] == sorted(a for a, _ in pairs), 'g_code_13_0_5 is not sorted (ga() searches it by halves)'
    return dict(pairs)


def source_addresses():
    """{13.0.4 offset: [where, ...]} of every 0x71xxxxxxxx literal in code (comments left out), and the literals not in GA()"""
    wrapped, raw = collections.defaultdict(list), collections.defaultdict(list)
    for top in ('runtime', 'fighter_sdk'):
        for d, dirs, files in os.walk(os.path.join(ROOT, top)):
            for f in files:
                if not f.endswith(('.c', '.h', '.inc')):
                    continue
                p = os.path.join(d, f)
                s = open(p, encoding='utf-8', errors='replace').read()
                s = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), s, flags=re.S)
                s = re.sub(r'//[^\n]*', '', s)
                rel = os.path.relpath(p, ROOT).replace('\\', '/')
                for n, line in enumerate(s.split('\n'), 1):
                    for m in re.finditer(r'(GA\(\s*)?(?:0x71([0-9a-fA-F]{8})(?:ull|ULL)|GUEST_BASE \+ 0x([0-9a-fA-F]{4,})(?:ull|ULL)?)\b', line):
                        (wrapped if m.group(1) else raw)[int(m.group(2) or m.group(3), 16)].append('%s:%d' % (rel, n))
    return wrapped, raw


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    e = Elf(sys.argv[1])
    bad = 0

    def report(ok, text):
        nonlocal bad
        bad += not ok
        print('%s %s' % ('ok  ' if ok else 'FAIL', text))

    text, ro, data = e.segs[0], e.segs[1], e.segs[2]
    report(text[2] == 0x39c8440, '13.0.5 code size %#x (hostrt.c load_layout knows 0x39c8440)' % text[2])
    report(ro[0] == RO4 + 0x1000 and data[0] == 0x4f16000 + 0x1000,
           'read-only data at %#x, data at %#x: 13.0.4 + 0x1000 (the rule of ga() for addresses above the code)' % (ro[0], data[0]))
    report(ro[2] == 0x154df60 and data[2] == 0x392570, 'sizes as in 13.0.4: read-only data %#x (0x154df60), data in the file %#x (0x392570)' % (ro[2], data[2]))

    fde = unwind_functions(e)
    funcs = sorted((int(r['Address'], 16) - BASE, int(r['Size'])) for r in csv.DictReader(open(sys.argv[2])))
    funcs = [f for f in funcs if f[0] < RO4]
    starts = [f[0] for f in funcs]

    def delta_near(a, k=25):
        i = bisect.bisect_right(starts, a)
        res = []
        for side in (funcs[max(0, i - k):i], funcs[i:i + k]):
            best = max(range(-0x800, 0x801, 4), key=lambda d: (sum(fde.get(s + d) == z for s, z in side), -abs(d)))
            res.append((best, sum(fde.get(s + best) == z for s, z in side)))
        own = funcs[i - 1] if i and a < funcs[i - 1][0] + funcs[i - 1][1] else None
        return res, own

    table = code_table()
    wrapped, raw = source_addresses()
    for a, where in sorted(raw.items()):
        report(a in RAW, '0x71%08x not passed through GA() (%s): %s' % (a, ', '.join(where[:3]), RAW.get(a, 'NOT EXPLAINED')))
    names = set(wrapped) | {a for a in raw if a in table}
    for a in sorted(set(table) - names):
        report(False, 'table entry 0x71%08x is not used by any source' % a)
    refs = adrp_targets(e, text[2])
    for old in sorted(names):
        where = ', '.join((wrapped.get(old) or raw.get(old))[:2])
        if old < RO4:
            if old not in table:
                report(False, '0x71%08x (%s): code address missing from g_code_13_0_5' % (old, where))
                continue
            new = table[old]
            d = new - old
            ((db, nb), (da, na)), own = delta_near(old)
            own_ok = own is not None and fde.get(own[0] + d) == own[1]
            ok = old < TEXT4 and (own_ok or (db == d and da == d) or (d in (db, da) and max(nb, na) >= 15))
            report(ok, '0x71%08x -> 0x71%08x  %+#7x  neighbours %+#x (%d/25) / %+#x (%d/25)%s  [%s]'
                   % (old, new, d, db, nb, da, na, ', own function same size' if own_ok else '', where))
        else:
            new = old + 0x1000
            report(refs[new] > refs[old], '0x71%08x -> 0x71%08x  +0x1000  code references: new %d, old %d  [%s]' % (old, new, refs[new], refs[old], where))
    print('%d checks failed' % bad if bad else 'all checks passed')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
