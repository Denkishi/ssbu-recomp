#!/usr/bin/env python3
"""NRO module inspector.

  python nro_info.py <file.nro>      details of one module
  python nro_info.py all [dir]       table for every module in dir (default ./nro) + totals;
                                     also writes nro_summary.json and imports_resolution.txt

Import resolution order used here: main.elf exports, then other NRO exports, then sdk.elf exports.
"""
import os, struct, sys, json, collections
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get('RECOMP_BIN') or os.path.normpath(os.path.join(HERE, '..', '..', 'binaries', '13.0.4'))   # RECOMP_BIN: scripts/setup.py

DT = {1: 'NEEDED', 2: 'PLTRELSZ', 3: 'PLTGOT', 4: 'HASH', 5: 'STRTAB', 6: 'SYMTAB', 7: 'RELA', 8: 'RELASZ',
      9: 'RELAENT', 10: 'STRSZ', 11: 'SYMENT', 12: 'INIT', 13: 'FINI', 14: 'SONAME', 20: 'PLTREL', 23: 'JMPREL',
      25: 'INIT_ARRAY', 26: 'FINI_ARRAY', 27: 'INIT_ARRAYSZ', 28: 'FINI_ARRAYSZ', 0x6ffffef5: 'GNU_HASH',
      0x6ffffff9: 'RELACOUNT', 30: 'FLAGS', 0x6ffffffb: 'FLAGS_1'}
RTYPE = {0x101: 'ABS64', 0x401: 'GLOB_DAT', 0x402: 'JUMP_SLOT', 0x403: 'RELATIVE', 0x404: 'TLS_DTPMOD',
         0x405: 'TLS_DTPREL', 0x406: 'TLS_TPREL', 0x407: 'TLSDESC'}


def cstr(d, o):
    return d[o:d.index(b'\0', o)].decode('latin-1')


def parse_dynamic(d, dyn_off):
    dyn = collections.OrderedDict(); needed = []
    o = dyn_off
    while True:
        tag, val = struct.unpack_from('<qQ', d, o); o += 16
        if tag == 0: break
        if tag == 1: needed.append(val)
        else: dyn[DT.get(tag, hex(tag))] = val
    return dyn, needed


def read_symbols(d, symtab, strtab, count):
    syms = []
    for i in range(count):
        name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', d, symtab + i * 24)
        syms.append((cstr(d, strtab + name_off), info & 0xf, info >> 4, shndx, value, size))
    return syms


class Nro:
    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0]
        d = self.d = open(path, 'rb').read()
        assert d[0x10:0x14] == b'NRO0', path
        self.mod0_off, = struct.unpack_from('<I', d, 4)
        self.file_size, = struct.unpack_from('<I', d, 0x18)
        (self.text_off, self.text_size, self.ro_off, self.ro_size, self.data_off, self.data_size,
         self.bss_size) = struct.unpack_from('<7I', d, 0x20)
        self.build_id = d[0x40:0x60].hex()
        self.dso_handle_off, = struct.unpack_from('<I', d, 0x60)
        m = self.mod0_off
        assert d[m:m + 4] == b'MOD0'
        dyn, bss0, bss1, eh0, eh1, modobj = struct.unpack_from('<6i', d, m + 4)
        self.dyn_off, self.bss_start, self.bss_end = m + dyn, m + bss0, m + bss1
        self.eh_hdr, self.eh_hdr_end, self.module_object = m + eh0, m + eh1, m + modobj
        self.dyn, self.needed = parse_dynamic(d, self.dyn_off)
        g = self.dyn.get
        self.symtab, self.strtab = g('SYMTAB'), g('STRTAB')
        # symbol count: from DT_HASH nchain if present, else up to strtab
        if g('HASH') is not None:
            self.nsym = struct.unpack_from('<I', d, g('HASH') + 4)[0]
        else:
            self.nsym = (self.strtab - self.symtab) // 24
        self.syms = read_symbols(d, self.symtab, self.strtab, self.nsym)
        self.soname = cstr(d, self.strtab + g('SONAME')) if g('SONAME') is not None else None
        self.needed = [cstr(d, self.strtab + n) for n in self.needed]
        self.image_size = self.bss_end if self.bss_end > self.file_size else self.file_size
        self.image_size = max(self.image_size, self.data_off + self.data_size + self.bss_size)

    def relocs(self):
        d = self.d; out = []
        for tab, sz in (('RELA', 'RELASZ'), ('JMPREL', 'PLTRELSZ')):
            if self.dyn.get(tab) is None: continue
            a = np.frombuffer(d, dtype='<u8', count=self.dyn[sz] // 8, offset=self.dyn[tab]).reshape(-1, 3)
            out.append((tab, a))
        return out

    def imports(self):
        return [s for s in self.syms[1:] if s[3] == 0]

    def exports(self):
        return [s for s in self.syms[1:] if s[3] != 0]

    def text_words(self):
        return np.frombuffer(self.d, dtype='<u4', count=self.text_size // 4, offset=self.text_off)

    def function_starts(self):
        """Same criteria as recomp.py compute_starts: BL targets, code pointers in RELATIVE relocations,
        ADRP+ADD / ADR code addresses are NOT included here (approximation), exported FUNC symbols."""
        w = self.text_words()
        idx = np.nonzero((w & 0xfc000000) == 0x94000000)[0]
        imm = (w[idx] & 0x3ffffff).astype(np.int64)
        imm = np.where(imm & 0x2000000, imm - 0x4000000, imm)
        tg = idx.astype(np.int64) * 4 + imm * 4 + self.text_off
        starts = set(int(t) for t in np.unique(tg) if self.text_off <= t < self.text_off + self.text_size)
        n_bl = len(starts)
        ptrs = set()
        for tab, a in self.relocs():
            rel = a[(a[:, 1] == 0x403)]
            add = rel[:, 2]
            ok = add[(add >= self.text_off) & (add < self.text_off + self.text_size) & (add % 4 == 0)]
            ptrs.update(int(x) for x in ok)
        exp = set(s[4] for s in self.exports() if s[1] == 2 and self.text_off <= s[4] < self.text_off + self.text_size)
        return starts | ptrs | exp, n_bl, len(ptrs), len(exp)

    def plt_range(self):
        """PLT stubs: adrp x16 / ldr x17,[x16,#] / add x16 / br x17 groups."""
        w = self.text_words()
        m = ((w[:-3] & 0x9f00001f) == 0x90000010) & ((w[1:-2] & 0xffc003ff) == 0xf9400211) & (w[3:] == 0xd61f0220)
        idx = np.nonzero(m)[0]
        return (int(idx[0]) * 4, int(idx[-1]) * 4 + 16, len(idx)) if len(idx) else (0, 0, 0)

    def reloc_stats(self):
        st = collections.Counter()
        for tab, a in self.relocs():
            t, c = np.unique(a[:, 1] & 0xffffffff, return_counts=True)
            for tt, cc in zip(t, c): st[RTYPE.get(int(tt), hex(int(tt)))] += int(cc)
        return st


def elf_dynsyms(path):
    """Dynamic symbols of an ELF (as produced by nso2elf): list like Nro.syms."""
    d = open(path, 'rb').read()
    phoff, = struct.unpack_from('<Q', d, 0x20); phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    segs = []; dyn_off = None
    for i in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
        if p_type == 1: segs.append((p_vaddr, p_offset, p_filesz))
        if p_type == 2: dyn_off = p_offset
    def v2o(v):
        for va, off, sz in segs:
            if va <= v < va + sz: return off + v - va
        raise ValueError(hex(v))
    dyn, _ = parse_dynamic(d, dyn_off)
    symtab, strtab = v2o(dyn['SYMTAB']), v2o(dyn['STRTAB'])
    if 'HASH' in dyn: n = struct.unpack_from('<I', d, v2o(dyn['HASH']) + 4)[0]
    else: n = (strtab - symtab) // 24
    return read_symbols(d, symtab, strtab, n)


def show(n):
    print('module        ', n.name, ' soname', n.soname, ' needed', n.needed)
    print('file size      %#x   image size (with bss) %#x' % (len(n.d), n.image_size))
    print('text           %#x +%#x' % (n.text_off, n.text_size))
    print('rodata         %#x +%#x' % (n.ro_off, n.ro_size))
    print('data           %#x +%#x' % (n.data_off, n.data_size))
    print('bss            %#x..%#x (header bss size %#x)' % (n.bss_start, n.bss_end, n.bss_size))
    print('MOD0 at %#x  dynamic %#x  eh_frame_hdr %#x..%#x  module object %#x  dso handle %#x' % (
        n.mod0_off, n.dyn_off, n.eh_hdr, n.eh_hdr_end, n.module_object, n.dso_handle_off))
    print('build id      ', n.build_id)
    print('dynamic:')
    for k, v in n.dyn.items(): print('   %-13s %#x' % (k, v))
    st, nbl, nptr, nexp = n.function_starts()
    print('functions (approx): %d  = union of %d BL targets, %d code pointers in relocations, %d exported FUNC' % (len(st), nbl, nptr, nexp))
    print('plt stubs      %#x..%#x (%d stubs)' % n.plt_range())
    print('relocations   ', dict(n.reloc_stats()))
    imp, exp = n.imports(), n.exports()
    print('dynamic symbols %d: imports %d (FUNC %d, OBJECT %d, NOTYPE %d), exports %d (FUNC %d, OBJECT %d)' % (
        n.nsym, len(imp), sum(s[1] == 2 for s in imp), sum(s[1] == 1 for s in imp), sum(s[1] == 0 for s in imp),
        len(exp), sum(s[1] == 2 for s in exp), sum(s[1] == 1 for s in exp)))
    for key in ('INIT', 'FINI'):
        if key in n.dyn: print('   %s at %#x' % (key, n.dyn[key]))
    if 'INIT_ARRAY' in n.dyn:
        print('   init_array %#x, %d entries' % (n.dyn['INIT_ARRAY'], n.dyn.get('INIT_ARRAYSZ', 0) // 8))
    print('exports:')
    for s in exp[:40]: print('   %#9x %s %s' % (s[4], 'FUNC' if s[1] == 2 else 'OBJ ' if s[1] == 1 else 'T%d  ' % s[1], s[0]))
    if len(exp) > 40: print('   ... %d more' % (len(exp) - 40))
    print('imports (first 40):')
    for s in imp[:40]: print('   %s %s' % ('FUNC' if s[1] == 2 else 'OBJ ' if s[1] == 1 else 'T%d  ' % s[1], s[0]))


def all_modules(dirpath):
    mods = [Nro(os.path.join(dirpath, f)) for f in sorted(os.listdir(dirpath)) if f.endswith('.nro')]
    main_syms = elf_dynsyms(os.path.join(BIN, 'main.elf'))
    main_exp = {s[0]: s for s in main_syms if s[3] != 0}
    main_imp = {s[0] for s in main_syms if s[3] == 0 and s[0]}
    sdk_exp = {s[0] for s in elf_dynsyms(os.path.join(BIN, 'sdk.elf')) if s[3] != 0}
    nro_exp = collections.defaultdict(list)
    for m in mods:
        for s in m.exports(): nro_exp[s[0]].append(m.name)
    rows = []; tot = collections.Counter()
    all_imports = collections.Counter(); provider = collections.Counter(); unresolved = collections.Counter()
    from_nro = collections.Counter(); main_used = set(); sdk_used = set()
    summary = []
    print('%-22s %9s %9s %8s %7s %7s %6s | %5s %5s %5s %5s %5s | %5s %s' % (
        'module', 'text', 'rodata', 'data', 'bss', 'funcs', 'plt', 'imp', 'main', 'nro', 'sdk', 'unres', 'exp', 'needed'))
    for m in mods:
        st, nbl, nptr, nexp = m.function_starts()
        imp, exp = m.imports(), m.exports()
        c = collections.Counter()
        for s in imp:
            nm = s[0]; all_imports[nm] += 1
            if nm in main_exp: c['main'] += 1; main_used.add(nm)
            elif any(o != m.name for o in nro_exp.get(nm, ())):
                c['nro'] += 1
                for o in nro_exp[nm]:
                    if o != m.name: from_nro[o] += 1
            elif nm in sdk_exp: c['sdk'] += 1; sdk_used.add(nm)
            else: c['unres'] += 1; unresolved[nm] += 1
        rs = m.reloc_stats()
        print('%-22s %9d %9d %8d %7d %7d %6d | %5d %5d %5d %5d %5d | %5d %s' % (
            m.name, m.text_size, m.ro_size, m.data_size, m.bss_end - m.bss_start, len(st), m.plt_range()[2],
            len(imp), c['main'], c['nro'], c['sdk'], c['unres'], len(exp), ','.join(m.needed)))
        tot.update(text=m.text_size, ro=m.ro_size, data=m.data_size, bss=m.bss_end - m.bss_start, funcs=len(st),
                   imp=len(imp), exp=len(exp), main=c['main'], nro=c['nro'], sdk=c['sdk'], unres=c['unres'],
                   image=m.image_size, file=len(m.d), init_array=m.dyn.get('INIT_ARRAYSZ', 0) // 8)
        for k, v in rs.items(): tot['rel_' + k] += v
        summary.append(dict(name=m.name, text=m.text_size, ro=m.ro_size, data=m.data_size, bss=m.bss_end - m.bss_start,
                            image_size=m.image_size, funcs=len(st), imports=len(imp), exports=len(exp),
                            imp_main=c['main'], imp_nro=c['nro'], imp_sdk=c['sdk'], imp_unresolved=c['unres'],
                            needed=m.needed, soname=m.soname, relocs=dict(rs), build_id=m.build_id,
                            init_array=m.dyn.get('INIT_ARRAYSZ', 0) // 8, has_init='INIT' in m.dyn,
                            export_names=[s[0] for s in exp]))
    print()
    print('modules: %d' % len(mods))
    print('total text %d bytes (%.1f MB), rodata %d, data %d, bss %d, file %d, image %d' % (
        tot['text'], tot['text'] / 1e6, tot['ro'], tot['data'], tot['bss'], tot['file'], tot['image']))
    print('main text 60,587,664 bytes: all NRO text together = %.2fx main' % (tot['text'] / 60587664))
    print('total functions (approx) %d   (main: 112,882)' % tot['funcs'])
    print('import slots: %d total; resolved by main exports %d, by another NRO %d, by sdk %d, unresolved %d' % (
        tot['imp'], tot['main'], tot['nro'], tot['sdk'], tot['unres']))
    print('distinct imported names: %d; distinct names taken from main: %d (main exports %d symbols); from sdk: %d' % (
        len(all_imports), len(main_used), len(main_exp), len(sdk_used)))
    print('distinct unresolved names: %d' % len(unresolved))
    print('exports: %d total, %d distinct names' % (tot['exp'], len(nro_exp)))
    print('modules that provide symbols to other NROs (import slots served):', dict(from_nro.most_common(10)))
    print('relocations:', {k[4:]: v for k, v in tot.items() if k.startswith('rel_')})
    print('init_array entries total: %d' % tot['init_array'])
    # which main imports (symbols main itself leaves undefined) are provided by NROs
    main_from_nro = sorted(nm for nm in main_imp if nm in nro_exp)
    print('symbols that MAIN imports and an NRO exports: %d' % len(main_from_nro))
    prov = collections.Counter()
    for nm in main_from_nro:
        for o in nro_exp[nm]: prov[o] += 1
    print('   providers:', dict(prov.most_common(8)))
    json.dump(dict(modules=summary, totals=dict(tot)), open(os.path.join(HERE, 'nro_summary.json'), 'w'), indent=1)
    with open(os.path.join(HERE, 'imports_resolution.txt'), 'w') as f:
        f.write('# name  users  provider\n')
        for nm, k in all_imports.most_common():
            p = 'main' if nm in main_exp else ('nro:' + ','.join(sorted(set(nro_exp[nm]))[:3]) if nm in nro_exp else 'sdk' if nm in sdk_exp else 'UNRESOLVED')
            f.write('%s %d %s\n' % (nm, k, p))
        f.write('\n# symbols main imports that NROs export\n')
        for nm in main_from_nro: f.write('%s %s\n' % (nm, ','.join(nro_exp[nm][:3])))
    if unresolved:
        print('unresolved examples:', list(unresolved.most_common(12)))


if __name__ == '__main__':
    if len(sys.argv) < 2: print(__doc__)
    elif sys.argv[1] == 'all': all_modules(sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'nro'))
    else: show(Nro(sys.argv[1]))
