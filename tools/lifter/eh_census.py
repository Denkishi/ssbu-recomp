"""C++ exception census of main: python eh_census.py [dump]
Counts BL call sites to each exception-related import, parses .eh_frame (found through MOD0's
eh_frame_hdr) and every LSDA: call-site records, landing pads, catch clauses vs clean-ups.
With 'dump' writes eh_tables.bin for the host runtime (see nncxxrt.c):
  u32 count, then per function: u64 start, u64 end, u64 lsda (image-relative; lsda 0 = none)."""
import struct, sys
import recomp
IMG, OFF, BASE = recomp.IMG, recomp.FILE_OFF, recomp.BASE
TEXT_END = recomp.TEXT_SIZE

def u32(o): return struct.unpack_from('<I', IMG, OFF + o)[0]
def s32(o): return struct.unpack_from('<i', IMG, OFF + o)[0]
def u64(o): return struct.unpack_from('<Q', IMG, OFF + o)[0]

env = recomp.Env(); env.imports = set(); env.full = True
# ---- BL call sites per import
names = ['__cxa_throw', '__cxa_rethrow', '__cxa_begin_catch', '__cxa_end_catch', '__cxa_allocate_exception', '__cxa_free_exception',
         '_Unwind_Resume', '__cxa_call_unexpected', '_ZSt9terminatev', '__cxa_bad_typeid', '_ZSt17rethrow_exceptionSt13exception_ptr',
         '_ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv', '_ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv',
         '_ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv', '_ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv',
         '__cxa_pure_virtual', '__cxa_deleted_virtual', '__cxa_guard_abort']
sites = {n: [] for n in names}
stub_cache = {}
for a in range(0, TEXT_END, 4):
    w = u32(a)
    if (w & 0x7c000000) != 0x14000000: continue          # B / BL
    off = w & 0x3ffffff
    if off & 0x2000000: off -= 0x4000000
    t = BASE + a + off * 4
    if t not in stub_cache: stub_cache[t] = env.plt_symbol(t)
    nm = stub_cache[t]
    if nm in sites: sites[nm].append((BASE + a, 'bl' if w >> 31 else 'b'))
print('call sites (BL/B to the PLT stub):')
for n in names: print('  %6d  %s' % (len(sites[n]), n))

# ---- MOD0 -> eh_frame_hdr
mod0 = u32(4)
assert IMG[OFF + mod0:OFF + mod0 + 4] == b'MOD0'
eh_hdr_start = mod0 + s32(mod0 + 0x10); eh_hdr_end = mod0 + s32(mod0 + 0x14)
print('MOD0 at %x: eh_frame_hdr %x..%x' % (mod0, eh_hdr_start, eh_hdr_end))
ver, ptr_enc, cnt_enc, tbl_enc = IMG[OFF + eh_hdr_start:OFF + eh_hdr_start + 4]
assert ver == 1 and ptr_enc == 0x1b and cnt_enc == 3 and tbl_enc == 0x3b, (ver, ptr_enc, cnt_enc, tbl_enc)
eh_frame = eh_hdr_start + 4 + s32(eh_hdr_start + 4)
fde_count = u32(eh_hdr_start + 8)
print('eh_frame at %x, %d FDEs in the search table' % (eh_frame, fde_count))

def uleb(o):
    r = s = 0
    while True:
        b = IMG[OFF + o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80: return r, o
def sleb(o):
    r = s = 0
    while True:
        b = IMG[OFF + o]; o += 1
        r |= (b & 0x7f) << s; s += 7
        if not b & 0x80:
            if b & 0x40: r -= 1 << s
            return r, o
def enc_ptr(o, enc):
    """returns (value as image offset or raw, new offset)"""
    if enc == 0xff: return None, o
    f = enc & 0x0f
    if f == 0x00: v = u64(o); n = 8
    elif f == 0x01: v, n2 = uleb(o); n = n2 - o
    elif f == 0x03: v = u32(o); n = 4
    elif f == 0x04: v = u64(o); n = 8
    elif f == 0x09: v, n2 = sleb(o); n = n2 - o
    elif f == 0x0b: v = s32(o); n = 4
    elif f == 0x0c: v = struct.unpack_from('<q', IMG, OFF + o)[0]; n = 8
    else: raise ValueError('enc %x' % enc)
    if (enc & 0x70) == 0x10: v += o
    if enc & 0x80 and v: v = reloc_value(v)               # indirect: a pointer slot filled by a load-time relocation
    return v, o + n

RELOC = {}
def load_relocs():
    sym, strt = recomp.SYMTAB, recomp.STRTAB
    for tbl, size in ((recomp.RELA, recomp.RELASZ), (recomp.JMPREL, recomp.PLTRELSZ)):
        for i in range(size // 24):
            off, info, addend = struct.unpack_from('<QQq', IMG, OFF + tbl + i * 24)
            t = info & 0xffffffff
            if t == 0x403: RELOC[off] = addend
            else:
                no, _, _, shndx, value, _ = struct.unpack_from('<IBBHQQ', IMG, OFF + sym + (info >> 32) * 24)
                if shndx: RELOC[off] = value + (addend if t == 0x101 else 0)
                else:
                    s = OFF + strt + no
                    RELOC[off] = 'import:' + IMG[s:IMG.index(bytes(1), s)].decode('latin-1')
load_relocs()
def reloc_value(slot):
    return RELOC.get(slot, u64(slot))

cies = {}
def parse_cie(o):
    if o in cies: return cies[o]
    length = u32(o); p = o + 8
    version = IMG[OFF + p]; p += 1
    aug = b''
    while IMG[OFF + p]: aug += bytes([IMG[OFF + p]]); p += 1
    p += 1
    _, p = uleb(p); _, p = sleb(p)
    if version == 1: p += 1
    else: _, p = uleb(p)
    info = {'lsda_enc': 0xff, 'fde_enc': 0, 'personality': None, 'aug': aug}
    if aug.startswith(b'z'):
        _, p = uleb(p)
        for ch in aug[1:]:
            if ch == ord('L'): info['lsda_enc'] = IMG[OFF + p]; p += 1
            elif ch == ord('R'): info['fde_enc'] = IMG[OFF + p]; p += 1
            elif ch == ord('P'):
                e = IMG[OFF + p]; p += 1
                info['personality'], p = enc_ptr(p, e)
    cies[o] = info
    return info

funcs = []
with_lsda = 0
for i in range(fde_count):
    loc = eh_hdr_start + s32(eh_hdr_start + 12 + i * 8)
    fde = eh_hdr_start + s32(eh_hdr_start + 16 + i * 8)
    length = u32(fde)
    cie = fde + 4 - u32(fde + 4)
    ci = parse_cie(cie)
    p = fde + 8
    start, p = enc_ptr(p, ci['fde_enc'])
    rng, p = enc_ptr(p, ci['fde_enc'] & 0x0f)
    lsda = None
    if ci['aug'].startswith(b'z'):
        alen, p = uleb(p)
        if ci['lsda_enc'] != 0xff and alen: lsda, _ = enc_ptr(p, ci['lsda_enc'])
    if lsda: with_lsda += 1
    funcs.append((start, start + rng, lsda or 0))
print('CIEs: %d   personalities: %s' % (len(cies), sorted(set(str(c['personality']) for c in cies.values()))))
print('FDEs with an LSDA: %d of %d' % (with_lsda, len(funcs)))

# ---- LSDAs
n_cs = n_lp = 0
n_cleanup_only = n_catch = n_catch_all = n_spec = 0
catch_types = {}
funcs_with_catch = 0
catch_sites = []
for start, end, lsda in funcs:
    if not lsda: continue
    p = lsda
    lpstart_enc = IMG[OFF + p]; p += 1
    lpstart = start
    if lpstart_enc != 0xff: lpstart, p = enc_ptr(p, lpstart_enc)
    ttype_enc = IMG[OFF + p]; p += 1
    ttype_base = None
    if ttype_enc != 0xff:
        v, p = uleb(p); ttype_base = p + v
    cs_enc = IMG[OFF + p]; p += 1
    cs_len, p = uleb(p)
    cs_end = p + cs_len
    action_tbl = cs_end
    has_catch = False
    while p < cs_end:
        cs_start, p = enc_ptr(p, cs_enc & 0x0f)
        cs_l, p = enc_ptr(p, cs_enc & 0x0f)
        lp, p = enc_ptr(p, cs_enc & 0x0f)
        action, p = uleb(p)
        n_cs += 1
        if not lp: continue
        n_lp += 1
        if action == 0: n_cleanup_only += 1; continue
        a = action_tbl + action - 1
        kinds = set()
        while True:
            filt, a2 = sleb(a)
            nxt, _ = sleb(a2)
            if filt > 0:
                size = {0x00: 8, 0x03: 4, 0x0b: 4, 0x04: 8, 0x0c: 8}[ttype_enc & 0x0f]
                tp, _ = enc_ptr(ttype_base - filt * size, ttype_enc)
                kinds.add(tp or 0)
            elif filt < 0: kinds.add(-1)
            else: kinds.add(-2)                           # clean-up inside an action chain
            if nxt == 0: break
            a = a2 + nxt
        real = [k for k in kinds if not isinstance(k, int) or k >= 0]
        if real:
            n_catch += 1; has_catch = True
            catch_sites.append((start + cs_start, cs_l, lpstart + lp, real))
            for k in real: catch_types[k] = catch_types.get(k, 0) + 1
            if 0 in real: n_catch_all += 1
        elif -1 in kinds: n_spec += 1
        else: n_cleanup_only += 1
    funcs_with_catch += has_catch
print('call-site records: %d, with a landing pad: %d' % (n_cs, n_lp))
print('  clean-up only (destructors, then _Unwind_Resume): %d' % n_cleanup_only)
print('  with catch clauses: %d (in %d functions), of which catch(...): %d' % (n_catch, funcs_with_catch, n_catch_all))
print('  exception-specification filters only: %d' % n_spec)
print('caught types (typeinfo address: call-site records):')
sym_by_addr = {}
for k, v in sorted(catch_types.items(), key=lambda kv: -kv[1]):
    name = '...'
    if isinstance(k, str): print('  %10s  %5d  %s' % ('', v, k)); continue
    if k:
        # typeinfo object: +8 = name pointer (relocated at load: read the RELATIVE addend from the file)
        np = reloc_value(k + 8) if k < len(IMG) - OFF else 0
        if isinstance(np, str): np = 0
        if 0 < np < len(IMG) - OFF:
            e = IMG.index(b'\0', OFF + np); name = IMG[OFF + np:e].decode('latin-1')
        else: name = 'imported / unresolved (%x)' % k
    print('  %10x  %5d  %s' % (k, v, name))
if len(sys.argv) > 1 and sys.argv[1] == 'dump':
    with open('eh_tables.bin', 'wb') as f:
        f.write(struct.pack('<I', len(funcs)))
        for s, e, l in funcs: f.write(struct.pack('<QQQ', s, e, l))
    with open('eh_catch_sites.txt', 'w') as f:
        for s, l, lp, real in catch_sites: f.write('%x %x %x %s\n' % (BASE + s, l, BASE + lp, ','.join(('%x' % r) if isinstance(r, int) else r for r in real)))
    print('wrote eh_tables.bin, eh_catch_sites.txt')
