"""Second pass of the exception census: classify catch landing pads and throw sites.
Needs eh_catch_sites.txt from `python eh_census.py dump`."""
import struct, sys, collections
import recomp
IMG, OFF, BASE = recomp.IMG, recomp.FILE_OFF, recomp.BASE
env = recomp.Env(); env.imports = set(); env.full = True
def word(a): return struct.unpack_from('<I', IMG, OFF + a - BASE)[0]
def bl_target(a):
    w = word(a)
    if (w & 0x7c000000) != 0x14000000: return None
    off = w & 0x3ffffff
    if off & 0x2000000: off -= 0x4000000
    return a + off * 4

def first_calls(a, limit=24):
    """names / addresses of the first BL/B targets from a, following nothing"""
    out = []
    for i in range(limit):
        t = bl_target(a + i * 4)
        if t is None:
            if word(a + i * 4) == 0xd65f03c0: break
            continue
        out.append(env.plt_symbol(t) or t)
        if not (word(a + i * 4) >> 31): break                 # unconditional B ends the block
    return out

term_helpers = set()
def is_terminate_helper(t):
    if not isinstance(t, int): return False
    if t in term_helpers: return True
    c = first_calls(t, 6)
    if c[:2] == ['__cxa_begin_catch', '_ZSt9terminatev']:
        term_helpers.add(t); return True
    return False

pads = collections.OrderedDict()
for line in open('eh_catch_sites.txt'):
    s, l, lp, types = line.split()
    pads.setdefault(int(lp, 16), set()).update(types.split(','))
kinds = collections.Counter()
real = []
for lp, types in pads.items():
    calls = first_calls(lp, 40)
    if calls and is_terminate_helper(calls[0]): kinds['terminate (noexcept boundary: __clang_call_terminate)'] += 1
    elif '__cxa_begin_catch' in calls: kinds['real catch handler (__cxa_begin_catch)'] += 1; real.append((lp, types, calls))
    elif calls and calls[0] == '__cxa_call_unexpected': kinds['exception specification (__cxa_call_unexpected)'] += 1
    else: kinds['other'] += 1; real.append((lp, types, calls))
print('distinct landing pads reached through a catch clause: %d' % len(pads))
for k, v in kinds.most_common(): print('  %5d  %s' % (v, k))
print('terminate helpers: %s' % ', '.join('%x' % t for t in sorted(term_helpers)))
print('\nreal catch handlers / unclassified pads:')
for lp, types, calls in real:
    print('  pad %x  types %s  calls %s' % (lp, ','.join(sorted(types)), [c if isinstance(c, str) else '%x' % c for c in calls[:6]]))

# ---- what is thrown: for each __cxa_throw call site find the typeinfo loaded into x1 (adrp/add or adrp/ldr before the call)
relocs = {}
for tbl, size in ((recomp.RELA, recomp.RELASZ), (recomp.JMPREL, recomp.PLTRELSZ)):
    for i in range(size // 24):
        off, info, addend = struct.unpack_from('<QQq', IMG, OFF + tbl + i * 24)
        if (info & 0xffffffff) == 0x403: relocs[off] = addend
        else:
            no, _, _, shndx, value, _ = struct.unpack_from('<IBBHQQ', IMG, OFF + recomp.SYMTAB + (info >> 32) * 24)
            s = OFF + recomp.STRTAB + no
            relocs[off] = value if shndx else IMG[s:IMG.index(bytes(1), s)].decode('latin-1')
def sext(v, n): return v - (1 << n) if v >> (n - 1) else v
def typeinfo_name(ti):
    if isinstance(ti, str): return ti
    n = relocs.get(ti + 8)
    if isinstance(n, int) and 0 < n < len(IMG):
        return IMG[OFF + n:IMG.index(bytes(1), OFF + n)].decode('latin-1')
    return '?%x' % ti
thrown = collections.Counter()
throw_sites = []
for a in range(BASE, BASE + recomp.TEXT_SIZE, 4):
    t = bl_target(a)
    if t is None or env.plt_symbol(t) != '__cxa_throw': continue
    reg = {}
    ti = None
    for b in range(a - 4 * 24, a, 4):                         # tiny forward register tracker over the preceding block
        w = word(b)
        rd = w & 31
        if (w & 0x9f000000) == 0x90000000:                    # adrp
            imm = sext((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21) << 12
            reg[rd] = ('page', (b & ~0xfff) + imm)
        elif (w & 0xffc00000) == 0x91000000 and ((w >> 5) & 31) in reg and reg[(w >> 5) & 31][0] == 'page':   # add imm
            reg[rd] = ('addr', reg[(w >> 5) & 31][1] + ((w >> 10) & 0xfff))
        elif (w & 0xffc00000) == 0xf9400000 and ((w >> 5) & 31) in reg and reg[(w >> 5) & 31][0] == 'page':   # ldr x, [page, off]
            slot = reg[(w >> 5) & 31][1] + ((w >> 10) & 0xfff) * 8
            reg[rd] = ('addr', relocs.get(slot - BASE, 0) if not isinstance(relocs.get(slot - BASE, 0), int) else BASE + relocs.get(slot - BASE, 0))
        elif (w & 0xffe0ffe0) == 0xaa0003e0 and ((w >> 16) & 31) in reg:  # mov x, x
            reg[rd] = reg[(w >> 16) & 31]
        elif (w & 0x7c000000) == 0x14000000: reg = {k: v for k, v in reg.items() if k >= 19}
    if 1 in reg and reg[1][0] == 'addr':
        v = reg[1][1]
        ti = typeinfo_name(v - BASE if isinstance(v, int) else v)
    thrown[ti or '(not resolved statically)'] += 1
    throw_sites.append((a, ti))
print('\n__cxa_throw call sites by thrown type (%d sites):' % len(throw_sites))
for k, v in thrown.most_common(): print('  %5d  %s' % (v, k))
