"""Prototype static recompiler: AArch64 (SSBU 13.0.4 main) -> C.

Feasibility study, not a product. Every guest function becomes a C function
`void f_<addr>(Ctx *c)` working on a register context; guest memory is mapped
at its real address (0x7100000000), so guest pointers are host pointers.

Usage:
    python recomp.py lift  <out.c> <root_addr>[,<root_addr>...]   lift roots and their callees
    python recomp.py cover                                         whole-binary coverage statistics
"""
import os
import struct
import sys
import collections

ELF = os.path.join(os.environ['RECOMP_BIN'], 'main.elf') if os.environ.get('RECOMP_BIN') else r'..\binaries\13.0.4\main.elf'   # RECOMP_BIN: scripts/setup.py
BASE = 0x7100000000
FILE_OFF = 0x1000

IMG = open(ELF, 'rb').read()


def _layout(img):
    """main's layout, read from the converted dump itself (tools/extract/nso2elf.py): the size of the code (the first
    PT_LOAD) and the relocation and symbol tables of the dynamic section that MOD0 points to. Read rather than written
    down, so each game build gives its own. 13.0.4: code 0x39c7e90, RELA 0x39c8070 / 0x7ad0d8, JMPREL 0x4175148 / 0xc600,
    SYMTAB 0x4197610, STRTAB 0x41c11f0 (the constants this replaces); 13.0.5: code 0x39c8440 and every table one page
    (0x1000) later with the same size (docs/VERSIONS.md)."""
    phoff, = struct.unpack_from('<Q', img, 0x20)
    phnum, = struct.unpack_from('<H', img, 0x38)
    text = next(struct.unpack_from('<Q', img, phoff + i * 56 + 32)[0] for i in range(phnum) if struct.unpack_from('<I', img, phoff + i * 56)[0] == 1)
    mod0 = struct.unpack_from('<I', img, FILE_OFF + 4)[0]
    assert img[FILE_OFF + mod0:FILE_OFF + mod0 + 4] == b'MOD0', 'main.elf: no MOD0 header'
    dyn, tags = mod0 + struct.unpack_from('<i', img, FILE_OFF + mod0 + 4)[0], {}
    while True:
        tag, val = struct.unpack_from('<qQ', img, FILE_OFF + dyn)
        dyn += 16
        if tag == 0:
            break
        tags.setdefault(tag, val)
    return text, tags[7], tags[8], tags[23], tags[2], tags[6], tags[5]


TEXT_SIZE, RELA, RELASZ, JMPREL, PLTRELSZ, SYMTAB, STRTAB = _layout(IMG)


# Instruction patch list (N3, plan\phases\N3_new_kind.md): DEFAULT OFF. A patch replaces one instruction word of the
# game by another before it is lifted: the way a table size or a member offset that is fixed in the game's code becomes
# ours without editing generated C by hand. File format, one patch per line:  <address> <original word> <new word>  # note
# (hex; the original word is checked against the binary). Switched on by RECOMP_PATCHES=<file> (a full lift with
# recomp_eh.py then applies it) or by recomp_partial.py, which re-lifts only the files that hold a patched address.
# With no file the dictionary is empty and word() returns the binary's word: the lift is unchanged.
PATCHES = {}


def load_patches(path):
    out = {}
    with open(path) as f:
        for line in f:
            t = line.split('#', 1)[0].split()
            if not t:
                continue
            addr, old, new = int(t[0], 16), int(t[1], 16), int(t[2], 16)
            have = struct.unpack_from('<I', IMG, FILE_OFF + addr - BASE)[0]
            if have != old:
                raise ValueError('patch %x: the binary has %08x, the patch expects %08x' % (addr, have, old))
            out[addr] = new
    return out


if __import__('os').environ.get('RECOMP_PATCHES'):
    PATCHES = load_patches(__import__('os').environ['RECOMP_PATCHES'])


def word(addr):
    if PATCHES and addr in PATCHES:
        return PATCHES[addr]
    return struct.unpack_from('<I', IMG, FILE_OFF + addr - BASE)[0]


def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


class Unimpl(Exception):
    pass


# ---------------------------------------------------------------- operands
def X(n):
    return '0ULL' if n == 31 else 'c->x[%d]' % n


def XS(n):
    return 'c->x[%d]' % n            # register 31 is SP here


def W(n):
    return '0U' if n == 31 else '(uint32_t)c->x[%d]' % n


def WS(n):
    return '(uint32_t)c->x[%d]' % n


def R(n, sf, sp=False):
    if sf:
        return XS(n) if sp else X(n)
    return WS(n) if sp else W(n)


def SET(n, sf, expr, sp=False):
    if n == 31 and not sp:
        return '(void)(%s);' % expr
    return 'c->x[%d] = (%s)(%s);' % (n, 'uint64_t' if sf else 'uint32_t', expr)


def T(sf):
    return 'uint64_t' if sf else 'uint32_t'


def decode_bitmasks(n, imms, immr, datasize):
    v = (n << 6) | (~imms & 0x3f)
    length = v.bit_length() - 1
    if length < 1:
        raise Unimpl('bitmask')
    levels = (1 << length) - 1
    s, r = imms & levels, immr & levels
    esize = 1 << length
    d = (s - r) & levels
    welem = (1 << (s + 1)) - 1
    telem = (1 << (d + 1)) - 1
    welem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1)
    wmask = tmask = 0
    for i in range(0, datasize, esize):
        wmask |= welem << i
        tmask |= telem << i
    return wmask & ((1 << datasize) - 1), tmask & ((1 << datasize) - 1)


SHIFT = ['lsl', 'lsr', 'asr', 'ror']


def shifted(expr, kind, amount, sf):
    if amount == 0:
        return expr
    if kind == 0:
        return '(%s << %d)' % (expr, amount)
    if kind == 1:
        return '(%s >> %d)' % (expr, amount)
    if kind == 2:
        return '(%s)((%s)%s >> %d)' % (T(sf), 'int64_t' if sf else 'int32_t', expr, amount)
    return 'ror%d(%s, %d)' % (64 if sf else 32, expr, amount)


def extended(n, option, shift):
    src = X(n)
    cast = ['(uint64_t)(uint8_t)', '(uint64_t)(uint16_t)', '(uint64_t)(uint32_t)', '(uint64_t)',
            '(uint64_t)(int64_t)(int8_t)', '(uint64_t)(int64_t)(int16_t)', '(uint64_t)(int64_t)(int32_t)',
            '(uint64_t)'][option]
    e = '(%s%s)' % (cast, src)
    return '(%s << %d)' % (e, shift) if shift else e


# ---------------------------------------------------------------- lifter
class Fn:
    """Lifting context for one guest function (address range [entry, end))."""

    def __init__(self, entry, end, env):
        self.entry, self.end, self.env = entry, end, env
        self.has_br = False

    def jump(self, target):
        if self.entry <= target < self.end:
            return 'goto L_%x;' % target
        return '{ %s return; }' % self.env.call(target)

    def lift(self, w, pc):
        op0 = bits(w, 28, 25)
        if op0 in (8, 9):
            return self.dp_imm(w, pc)
        if op0 in (10, 11):
            return self.branch(w, pc)
        if op0 in (4, 6, 12, 14):
            return self.loadstore(w, pc)
        if op0 in (5, 13):
            return self.dp_reg(w, pc)
        if op0 in (7, 15):
            return self.fpsimd(w, pc)
        raise Unimpl('class%d' % op0)

    # ---- data processing, immediate
    def dp_imm(self, w, pc):
        sf, rd, rn = bits(w, 31, 31), bits(w, 4, 0), bits(w, 9, 5)
        g = bits(w, 25, 23)
        if g in (0, 1):
            imm = sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21)
            val = ((pc & ~0xfff) + (imm << 12)) if sf else pc + imm
            return SET(rd, 1, '0x%xULL' % (val & (2 ** 64 - 1)))
        if g == 2:
            imm = bits(w, 21, 10) << (12 if bits(w, 22, 22) else 0)
            sub, s = bits(w, 30, 30), bits(w, 29, 29)
            a = R(rn, sf, sp=True)
            if s:
                return SET(rd, sf, '%s%d(c, %s, %s, %d)' % ('adds', 64 if sf else 32, a,
                                                              ('~(%s)%dULL' % (T(sf), imm)) if sub else '%dULL' % imm, sub))
            return SET(rd, sf, '%s %s %dULL' % (a, '-' if sub else '+', imm), sp=True)
        if g == 4:
            opc = bits(w, 30, 29)
            wmask, _ = decode_bitmasks(bits(w, 22, 22), bits(w, 15, 10), bits(w, 21, 16), 64 if sf else 32)
            a = R(rn, sf)
            e = '%s %s 0x%xULL' % (a, '&|^&'[opc], wmask)
            if opc == 3:
                return SET(rd, sf, 'logic%d(c, %s)' % (64 if sf else 32, e))
            return SET(rd, sf, e, sp=True)
        if g == 5:
            opc, hw, imm = bits(w, 30, 29), bits(w, 22, 21) * 16, bits(w, 20, 5)
            if opc == 0:
                return SET(rd, sf, '~(0x%xULL)' % (imm << hw))
            if opc == 2:
                return SET(rd, sf, '0x%xULL' % (imm << hw))
            if opc == 3:
                return SET(rd, sf, '(%s & ~0x%xULL) | 0x%xULL' % (R(rd, sf), 0xffff << hw, imm << hw))
            raise Unimpl('movwide')
        if g == 6:
            opc, n, immr, imms = bits(w, 30, 29), bits(w, 22, 22), bits(w, 21, 16), bits(w, 15, 10)
            size = 64 if sf else 32
            wmask, tmask = decode_bitmasks(n, imms, immr, size)
            if opc == 3:
                raise Unimpl('bitfield3')
            dst = R(rd, sf) if opc == 1 else '0'
            top = dst if opc == 1 else ('((s >> %d) & 1 ? ~(%s)0 : 0)' % (imms, T(sf)) if opc == 0 else '0')
            return ('{ %s s = %s; %s bot = (%s & ~0x%xULL) | (ror%d(s, %d) & 0x%xULL); %s top = %s; %s }'
                    % (T(sf), R(rn, sf), T(sf), dst, wmask, size, immr, wmask, T(sf), top,
                       SET(rd, sf, '(top & ~0x%xULL) | (bot & 0x%xULL)' % (tmask, tmask))))
        if g == 7:
            rm, lsb = bits(w, 20, 16), bits(w, 15, 10)
            size = 64 if sf else 32
            if lsb == 0:
                return SET(rd, sf, R(rm, sf))
            return SET(rd, sf, '(%s >> %d) | (%s << %d)' % (R(rm, sf), lsb, R(rn, sf), size - lsb))
        raise Unimpl('dpimm%d' % g)

    # ---- branches and system
    def branch(self, w, pc):
        if bits(w, 30, 26) == 5:
            target = pc + sext(bits(w, 25, 0), 26) * 4
            if bits(w, 31, 31):
                return 'c->x[30] = 0x%xULL; %s' % (pc + 4, self.env.call(target))
            return self.jump(target)
        if bits(w, 31, 24) == 0x54:
            target = pc + sext(bits(w, 23, 5), 19) * 4
            return 'if (cond(c, %d)) %s' % (bits(w, 3, 0), self.jump(target))
        if bits(w, 30, 25) == 0x1a:
            target = pc + sext(bits(w, 23, 5), 19) * 4
            return 'if (%s %s 0) %s' % (R(bits(w, 4, 0), bits(w, 31, 31)), '!=' if bits(w, 24, 24) else '==', self.jump(target))
        if bits(w, 30, 25) == 0x1b:
            bit = (bits(w, 31, 31) << 5) | bits(w, 23, 19)
            target = pc + sext(bits(w, 18, 5), 14) * 4
            return 'if (((%s >> %d) & 1) %s 0) %s' % (X(bits(w, 4, 0)), bit, '!=' if bits(w, 24, 24) else '==', self.jump(target))
        if bits(w, 31, 25) == 0x6b:
            opc, rn = bits(w, 24, 21), bits(w, 9, 5)
            if opc == 2:
                return 'return;'
            if opc == 1:
                return '{ uint64_t t = %s; c->x[30] = 0x%xULL; call_indirect(c, t); }' % (X(rn), pc + 4)
            if opc == 0:
                self.has_br = True
                return '{ jt = %s; goto L_dispatch; }' % X(rn)
            raise Unimpl('br%d' % opc)
        if (w & 0xfffff01f) == 0xd503201f:
            return ';'                                   # nop / hint
        if (w & 0xffe0001f) == 0xd4000001:
            return 'guest_svc(c, 0x%x);' % bits(w, 20, 5)
        if (w & 0xffe0001f) == 0xd4200000:
            return 'guest_trap("brk", 0x%xULL);' % pc
        if (w & 0xffffffe0) in (0xd53bd040, 0xd53bd060):
            return SET(bits(w, 4, 0), 1, 'c->tpidr')
        if (w & 0xffffffe0) == 0xd51bd040:
            return 'c->tpidr = %s;' % X(bits(w, 4, 0))
        if (w & 0xffffffe0) == 0xd53b00e0:
            return SET(bits(w, 4, 0), 1, '0x10ULL')     # dczid_el0: DC ZVA prohibited
        if (w & 0xffffffe0) in (0xd53b4400, 0xd53b4420):
            return SET(bits(w, 4, 0), 1, '0ULL')        # fpcr / fpsr read
        if (w & 0xffffffe0) in (0xd51b4400, 0xd51b4420):
            return ';'                                   # fpcr / fpsr write
        if (w & 0xfffff0ff) in (0xd503305f, 0xd50330bf, 0xd503309f, 0xd50330df):
            return ';'                                   # clrex / dmb / dsb / isb
        raise Unimpl('system')

    # ---- loads and stores
    def addr_mode(self, w, scale):
        """Single-register addressing. Returns (pre, address, post)."""
        rn = bits(w, 9, 5)
        base = XS(rn)
        if bits(w, 24, 24):
            return '', '%s + %dULL' % (base, bits(w, 21, 10) << scale), ''
        if bits(w, 21, 21) == 0:
            imm, mode = sext(bits(w, 20, 12), 9), bits(w, 11, 10)
            if mode == 0:
                return '', '%s + (uint64_t)%dLL' % (base, imm), ''
            if mode == 1:
                return '', base, '%s += (uint64_t)%dLL;' % (base, imm)
            if mode == 3:
                return '%s += (uint64_t)%dLL;' % (base, imm), base, ''
            raise Unimpl('ldst-unpriv')
        if bits(w, 11, 10) == 2:
            option, s, rm = bits(w, 15, 13), bits(w, 12, 12), bits(w, 20, 16)
            if option not in (2, 3, 6, 7):
                raise Unimpl('ldst-ext')
            return '', '%s + %s' % (base, extended(rm, option, scale if s else 0)), ''
        raise Unimpl('ldst-atomic')

    def loadstore(self, w, pc):
        v = bits(w, 26, 26)
        g = bits(w, 29, 27)
        rt, rn = bits(w, 4, 0), bits(w, 9, 5)
        if g == 5:                                       # pairs
            opc, mode, load = bits(w, 31, 30), bits(w, 24, 23), bits(w, 22, 22)
            rt2 = bits(w, 14, 10)
            if v:
                size = 4 << opc
            else:
                if opc == 3 or (opc == 1 and not load):
                    raise Unimpl('pair')
                size = 8 if opc == 2 else 4
            imm = sext(bits(w, 21, 15), 7) * size
            base = XS(rn)
            pre = '%s += (uint64_t)%dLL;' % (base, imm) if mode == 3 else ''
            post = '%s += (uint64_t)%dLL;' % (base, imm) if mode == 1 else ''
            off = imm if mode in (2, 0) else 0
            body = 'uint64_t a = %s + (uint64_t)%dLL; ' % (base, off)
            if v:
                if load:
                    body += 'V128 t1 = rdv(a, %d), t2 = rdv(a + %d, %d); c->v[%d] = t1; c->v[%d] = t2;' % (size, size, size, rt, rt2)
                else:
                    body += 'wrv(a, &c->v[%d], %d); wrv(a + %d, &c->v[%d], %d);' % (rt, size, size, rt2, size)
            elif load:
                rd = 'rd%d' % (size * 8)
                if opc == 1:
                    body += 'uint64_t t1 = (uint64_t)(int64_t)(int32_t)rd32(a), t2 = (uint64_t)(int64_t)(int32_t)rd32(a + 4); '
                else:
                    body += 'uint64_t t1 = %s(a), t2 = %s(a + %d); ' % (rd, rd, size)
                body += SET(rt, 1, 't1') + ' ' + SET(rt2, 1, 't2')
            else:
                body += 'wr%d(a, %s); wr%d(a + %d, %s);' % (size * 8, R(rt, size == 8), size * 8, size, R(rt2, size == 8))
            return '{ %s %s %s }' % (pre, body, post)
        if g == 7:                                       # single register
            size, opc = bits(w, 31, 30), bits(w, 23, 22)
            if v:
                nbytes = 16 if opc & 2 else 1 << size
                if opc & 2 and size != 0:
                    raise Unimpl('ldst-v')
                scale = 4 if opc & 2 else size
                pre, a, post = self.addr_mode(w, scale)
                if opc & 1:
                    body = 'c->v[%d] = rdv(a, %d);' % (rt, nbytes)
                else:
                    body = 'wrv(a, &c->v[%d], %d);' % (rt, nbytes)
                return '{ %s uint64_t a = %s; %s %s }' % (pre, a, body, post)
            if size == 3 and opc == 2:
                return ';'                               # prfm
            pre, a, post = self.addr_mode(w, size)
            nb = 8 << size
            if opc == 0:
                body = 'wr%d(a, %s);' % (nb, R(rt, size == 3))
            elif opc == 1:
                body = SET(rt, 1, 'rd%d(a)' % nb)
            else:
                ext = '(int64_t)(int%d_t)rd%d(a)' % (nb, nb)
                body = SET(rt, 1 if opc == 2 else 0, ext)
            return '{ %s uint64_t a = %s; %s %s }' % (pre, a, body, post)
        if g == 3 and bits(w, 24, 24) == 0:              # literal
            opc = bits(w, 31, 30)
            a = '0x%xULL' % (pc + sext(bits(w, 23, 5), 19) * 4)
            if v:
                return 'c->v[%d] = rdv(%s, %d);' % (rt, a, 4 << opc)
            if opc == 3:
                return ';'
            return SET(rt, 1, ['rd32(%s)', 'rd64(%s)', '(int64_t)(int32_t)rd32(%s)'][opc] % a)
        if bits(w, 29, 24) == 8 and not v:               # exclusive / ordered
            size, o2, load, o1 = bits(w, 31, 30), bits(w, 23, 23), bits(w, 22, 22), bits(w, 21, 21)
            if o1:
                raise Unimpl('excl-pair')
            nb = 8 << size
            rs = bits(w, 20, 16)
            if o2:                                       # ldar / stlr: ordered, not exclusive
                if load:
                    return SET(rt, 1, 'rd%d(%s)' % (nb, XS(rn)))
                return 'wr%d(%s, %s);' % (nb, XS(rn), R(rt, size == 3))
            if load:                                     # ldxr / ldaxr: remember address and value
                return '{ uint64_t a = %s; c->ex_addr = a; c->ex_val = rd%d(a); %s }' % (XS(rn), nb, SET(rt, 1, 'c->ex_val'))
            # stxr / stlxr: succeeds only if the value is still the one loaded (compare-and-swap)
            return SET(rs, 0, 'stx%d(c, %s, %s)' % (nb, XS(rn), R(rt, size == 3)))
        if bits(w, 31, 31) == 0 and bits(w, 29, 24) in (0x0c, 0x0d):   # AdvSIMD structure load/store
            q, post, load, rm = bits(w, 30, 30), bits(w, 23, 23), bits(w, 22, 22), bits(w, 20, 16)
            size = bits(w, 11, 10)
            if bits(w, 24, 24) == 0:
                opcode = bits(w, 15, 12)
                nb = 16 if q else 8
                nreg = {7: 1, 10: 2, 6: 3, 2: 4}.get(opcode)          # LD1/ST1: one structure, 1..4 registers
                if nreg is not None:
                    if load:
                        body = ' '.join('V128 t%d = rdv(a + %d, %d);' % (i, i * nb, nb) for i in range(nreg))
                        body += ' ' + ' '.join('c->v[%d] = t%d;' % ((rt + i) & 31, i) for i in range(nreg))
                    else:
                        body = ' '.join('wrv(a + %d, &c->v[%d], %d);' % (i * nb, (rt + i) & 31, nb) for i in range(nreg))
                    total = nb * nreg
                else:
                    nstruct = {8: 2, 4: 3, 0: 4}.get(opcode)           # LD2..LD4 / ST2..ST4: interleaved
                    if nstruct is None or (size == 3 and not q):
                        raise Unimpl('ldN')
                    body = '%s(c, a, %d, %d, %d, %d);' % ('v_ldn' if load else 'v_stn', rt, nstruct, size, q)
                    total = nb * nstruct
            else:
                opcode, s_bit = bits(w, 15, 13), bits(w, 12, 12)
                if bits(w, 21, 21):
                    raise Unimpl('ldN')
                if opcode == 6 and load:
                    body = 'v_dup(&c->v[%d], rd%d(a), %d, %d);' % (rt, 8 << size, size, (16 if q else 8) >> size)
                    total = 1 << size
                else:
                    if opcode == 0:
                        es, idx = 0, (q << 3) | (s_bit << 2) | size
                    elif opcode == 2:
                        es, idx = 1, (q << 2) | (s_bit << 1) | (size >> 1)
                    elif opcode == 4 and size == 0:
                        es, idx = 2, (q << 1) | s_bit
                    elif opcode == 4 and size == 1:
                        es, idx = 3, q
                    else:
                        raise Unimpl('ldN')
                    if load:
                        body = 'vset(&c->v[%d], %d, %d, rd%d(a));' % (rt, es, idx, 8 << es)
                    else:
                        body = 'wr%d(a, vget(&c->v[%d], %d, %d));' % (8 << es, rt, es, idx)
                    total = 1 << es
            wb = ''
            if post:
                wb = '%s += %s;' % (XS(rn), ('%dULL' % total) if rm == 31 else X(rm))
            return '{ uint64_t a = %s; %s %s }' % (XS(rn), body, wb)
        raise Unimpl('ldst%d' % g)

    # ---- data processing, register
    def dp_reg(self, w, pc):
        sf, rd, rn, rm = bits(w, 31, 31), bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
        size = 64 if sf else 32
        if bits(w, 28, 28) == 0:
            if bits(w, 24, 24) == 0:                     # logical, shifted register
                opc, n = bits(w, 30, 29), bits(w, 21, 21)
                b = shifted(R(rm, sf), bits(w, 23, 22), bits(w, 15, 10), sf)
                if n:
                    b = '~%s' % b
                e = '%s %s %s' % (R(rn, sf), '&|^&'[opc], b)
                if opc == 3:
                    return SET(rd, sf, 'logic%d(c, %s)' % (size, e))
                return SET(rd, sf, e)
            sub, s = bits(w, 30, 30), bits(w, 29, 29)
            if bits(w, 21, 21) == 0:                     # add/sub, shifted register
                b = shifted(R(rm, sf), bits(w, 23, 22), bits(w, 15, 10), sf)
                a = R(rn, sf)
                sp = False
            else:                                        # add/sub, extended register
                b = '(%s)%s' % (T(sf), extended(rm, bits(w, 15, 13), bits(w, 12, 10)))
                a = R(rn, sf, sp=True)
                sp = True
            if s:
                return SET(rd, sf, 'adds%d(c, %s, %s, %d)' % (size, a, ('~(%s)' % b) if sub else b, sub))
            return SET(rd, sf, '%s %s %s' % (a, '-' if sub else '+', b), sp=sp)
        g = bits(w, 24, 21)
        if bits(w, 24, 24):                              # three-source
            op31, o0, ra = bits(w, 23, 21), bits(w, 15, 15), bits(w, 14, 10)
            sign = '-' if o0 else '+'
            if op31 == 0:
                return SET(rd, sf, '%s %s %s * %s' % (R(ra, sf), sign, R(rn, sf), R(rm, sf)))
            if op31 == 1:
                return SET(rd, 1, '%s %s (uint64_t)((int64_t)(int32_t)%s * (int64_t)(int32_t)%s)' % (X(ra), sign, W(rn), W(rm)))
            if op31 == 5:
                return SET(rd, 1, '%s %s (uint64_t)%s * (uint64_t)%s' % (X(ra), sign, W(rn), W(rm)))
            if op31 == 2:
                return SET(rd, 1, 'smulh(%s, %s)' % (X(rn), X(rm)))
            if op31 == 6:
                return SET(rd, 1, 'umulh(%s, %s)' % (X(rn), X(rm)))
            raise Unimpl('mul3')
        if g == 0 and bits(w, 15, 10) == 0:              # adc / sbc
            sub, s = bits(w, 30, 30), bits(w, 29, 29)
            b = ('~%s' % R(rm, sf)) if sub else R(rm, sf)
            if s:
                return SET(rd, sf, 'adds%d(c, %s, %s, c->C)' % (size, R(rn, sf), b))
            return SET(rd, sf, '%s + %s + c->C' % (R(rn, sf), b))
        if g == 2:                                       # ccmn / ccmp
            sub, cc, nzcv = bits(w, 30, 30), bits(w, 15, 12), bits(w, 3, 0)
            b = ('%dULL' % rm) if bits(w, 11, 11) else R(rm, sf)
            b = ('~(%s)%s' % (T(sf), b)) if sub else '(%s)%s' % (T(sf), b)
            return ('if (cond(c, %d)) (void)adds%d(c, %s, %s, %d); else set_nzcv(c, %d);'
                    % (cc, size, R(rn, sf), b, sub, nzcv))
        if g == 4:                                       # csel family
            op, o2, cc = bits(w, 30, 30), bits(w, 10, 10), bits(w, 15, 12)
            b = R(rm, sf)
            alt = [b, '%s + 1' % b, '~%s' % b, '(%s)0 - %s' % (T(sf), b)][op * 2 + o2]
            return SET(rd, sf, 'cond(c, %d) ? %s : %s' % (cc, R(rn, sf), alt))
        if g == 6:
            opcode = bits(w, 15, 10)
            if bits(w, 30, 30) == 0:                     # two-source
                a, b = R(rn, sf), R(rm, sf)
                if opcode == 2:
                    return SET(rd, sf, '%s ? %s / %s : 0' % (b, a, b))
                if opcode == 3:
                    return SET(rd, sf, 'sdiv%d(%s, %s)' % (size, a, b))
                if opcode in (8, 9, 10, 11):
                    amt = '(%s & %d)' % (b, size - 1)
                    if opcode == 8:
                        return SET(rd, sf, '%s << %s' % (a, amt))
                    if opcode == 9:
                        return SET(rd, sf, '%s >> %s' % (a, amt))
                    if opcode == 10:
                        return SET(rd, sf, '(%s)%s >> %s' % ('int64_t' if sf else 'int32_t', a, amt))
                    return SET(rd, sf, 'ror%d(%s, %s)' % (size, a, amt))
                raise Unimpl('dp2')
            a = R(rn, sf)
            if opcode == 0:
                return SET(rd, sf, 'rbit%d(%s)' % (size, a))
            if opcode == 1:
                return SET(rd, sf, 'rev16_%d(%s)' % (size, a))
            if opcode == 2:
                return SET(rd, sf, 'rev32_64(%s)' % a if sf else 'bswap32(%s)' % a)
            if opcode == 3 and sf:
                return SET(rd, sf, 'bswap64(%s)' % a)
            if opcode == 4:
                return SET(rd, sf, 'clz%d(%s)' % (size, a))
            if opcode == 5:
                return SET(rd, sf, 'cls%d(%s)' % (size, a))
            raise Unimpl('dp1')
        raise Unimpl('dpreg%d' % g)

    # ---- scalar floating point (advanced SIMD is handled only where needed)
    def fpsimd(self, w, pc):
        rd, rn, rm = bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
        if bits(w, 30, 24) == 0x1e and bits(w, 21, 21) == 0:     # fixed-point <-> fp: the value is scaled by 2^fbits
            sf, ty, rmode, opcode, fbits = bits(w, 31, 31), bits(w, 23, 22), bits(w, 20, 19), bits(w, 18, 16), 64 - bits(w, 15, 10)
            if ty > 1 or fbits < 1 or fbits > (64 if sf else 32):
                raise Unimpl('fp16')
            f = 'df[0]' if ty else 'f[0]'
            ft = 'double' if ty else 'float'
            if rmode == 0 and opcode in (2, 3):              # SCVTF / UCVTF: one rounding, then an exact scale
                src = R(rn, sf)
                if opcode == 2:
                    src = '(%s)%s' % ('int64_t' if sf else 'int32_t', src)
                return '{ %s t = (%s)%s * (%s)%s; vzero(&c->v[%d]); c->v[%d].%s = t; }' % (ft, ft, src, ft, repr(2.0 ** -fbits), rd, rd, f)
            if rmode == 3 and opcode in (0, 1):              # FCVTZS / FCVTZU
                return SET(rd, sf, 'f2%s%d(trunc((double)c->v[%d].%s * %s))' % ('u' if opcode else 's', 64 if sf else 32, rn, f, repr(2.0 ** fbits)))
            raise Unimpl('fcvt')
        if bits(w, 31, 24) == 0x1e and bits(w, 21, 21) == 1 or bits(w, 30, 24) == 0x1e and bits(w, 21, 21) == 1 and bits(w, 15, 10) == 0:
            ty = bits(w, 23, 22)
            if bits(w, 15, 10) == 0:                     # int <-> fp
                sf, rmode, opcode = bits(w, 31, 31), bits(w, 20, 19), bits(w, 18, 16)
                if ty == 2 and rmode == 1 and opcode in (6, 7):
                    if opcode == 6:
                        return SET(rd, 1, 'c->v[%d].d[1]' % rn)
                    return 'c->v[%d].d[1] = %s;' % (rd, X(rn))
                if ty > 1:
                    raise Unimpl('fp16')
                f = 'df[0]' if ty else 'f[0]'
                ft = 'double' if ty else 'float'
                if rmode == 0 and opcode in (2, 3):
                    src = R(rn, sf)
                    if opcode == 2:
                        src = '(%s)%s' % ('int64_t' if sf else 'int32_t', src)
                    return '{ %s t = (%s)%s; vzero(&c->v[%d]); c->v[%d].%s = t; }' % (ft, ft, src, rd, rd, f)
                if rmode == 0 and opcode == 6:
                    return SET(rd, sf, 'c->v[%d].%s' % (rn, 'd[0]' if sf else 'w[0]'))
                if rmode == 0 and opcode == 7:
                    return '{ uint64_t t = %s; vzero(&c->v[%d]); c->v[%d].%s = t; }' % (R(rn, sf), rd, rd, 'd[0]' if sf else 'w[0]')
                rnd = {(3, 0): 'trunc', (3, 1): 'trunc', (0, 0): 'rint', (0, 1): 'rint', (1, 0): 'ceil', (1, 1): 'ceil',
                       (2, 0): 'floor', (2, 1): 'floor', (0, 4): 'round', (0, 5): 'round'}.get((rmode, opcode))
                if rnd is None:
                    raise Unimpl('fcvt')
                unsigned = opcode & 1
                return SET(rd, sf, 'f2%s%d(%s((double)c->v[%d].%s))' % ('u' if unsigned else 's', 64 if sf else 32, rnd, rn, f))
            if bits(w, 31, 24) == 0x1e and bits(w, 14, 10) == 0x10 and ty in (0, 1, 3):
                # FCVT to or from half precision (opcode 7 = to half; type 3 = from half, opcode 4 / 5 = to single / double)
                op = bits(w, 20, 15)
                if ty != 3 and op == 7:
                    return '{ uint16_t t = %s(c->v[%d].%s); vzero(&c->v[%d]); c->v[%d].h[0] = t; }' % ('f64_to_half' if ty else 'f32_to_half', rn, 'df[0]' if ty else 'f[0]', rd, rd)
                if ty == 3 and op in (4, 5):
                    tf, tt, fn = ('df[0]', 'double', 'half_to_f64') if op == 5 else ('f[0]', 'float', 'half_to_f32')
                    return '{ %s t = %s(c->v[%d].h[0]); vzero(&c->v[%d]); c->v[%d].%s = t; }' % (tt, fn, rn, rd, rd, tf)
            if bits(w, 31, 24) != 0x1e or ty > 1:
                raise Unimpl('fp16')
            f = 'df[0]' if ty else 'f[0]'
            ft = 'double' if ty else 'float'
            sfx = '' if ty else 'f'

            def setf(expr):
                return '{ %s t = fnan%d(%s); vzero(&c->v[%d]); c->v[%d].%s = t; }' % (ft, 64 if ty else 32, expr, rd, rd, f)
            a, b = 'c->v[%d].%s' % (rn, f), 'c->v[%d].%s' % (rm, f)
            if bits(w, 11, 10) == 2:
                op = bits(w, 15, 12)
                if op < 4:
                    return setf('%s %s %s' % (a, '*/+-'[op], b))
                if op in (4, 5, 6, 7):               # FMAX, FMIN, FMAXNM, FMINNM: exact NaN and signed-zero rules (fpexact.h); the result is an operand, no fnan
                    return '{ %s t = fp_maxmin%d(%s, %s, %d, %d); vzero(&c->v[%d]); c->v[%d].%s = t; }' % (ft, 64 if ty else 32, a, b, op & 1, op >> 1 & 1, rd, rd, f)
                if op == 8:
                    return setf('-(%s * %s)' % (a, b))
                raise Unimpl('fp2')
            if bits(w, 14, 10) == 0x10:
                op = bits(w, 20, 15)
                if op == 0:
                    return setf(a)
                if op == 1:
                    return setf('fabs%s(%s)' % (sfx, a))
                if op == 2:
                    return setf('-%s' % a)
                if op == 3:
                    return setf('sqrt%s(%s)' % (sfx, a))
                if op in (4, 5):
                    tf, tt = ('df[0]', 'double') if op == 5 else ('f[0]', 'float')
                    return '{ %s t = (%s)%s; vzero(&c->v[%d]); c->v[%d].%s = t; }' % (tt, tt, a, rd, rd, tf)
                rnd = {8: 'rint', 9: 'ceil', 10: 'floor', 11: 'trunc', 12: 'round', 14: 'rint', 15: 'rint'}.get(op)
                if rnd:
                    return setf('%s%s(%s)' % (rnd, sfx, a))
                raise Unimpl('fp1')
            if bits(w, 13, 10) == 8:
                b2 = '0' if bits(w, 3, 3) else b
                return 'fcmp(c, (double)%s, (double)%s);' % (a, b2)
            if bits(w, 12, 10) == 4:
                imm8 = bits(w, 20, 13)
                sign = -1.0 if imm8 & 0x80 else 1.0
                exp = ((imm8 >> 4) & 7) ^ 4
                val = sign * (16 + (imm8 & 0xf)) / 16.0 * (2.0 ** (exp - 3))
                return setf('(%s)%r' % (ft, val))
            if bits(w, 11, 10) == 1:
                return ('if (cond(c, %d)) fcmp(c, (double)%s, (double)%s); else set_nzcv(c, %d);'
                        % (bits(w, 15, 12), a, b, bits(w, 3, 0)))
            if bits(w, 11, 10) == 3:
                return setf('cond(c, %d) ? %s : %s' % (bits(w, 15, 12), a, b))
            raise Unimpl('fp')
        if bits(w, 31, 24) == 0x1f:                      # fmadd family
            ty = bits(w, 23, 22)
            if ty > 1:
                raise Unimpl('fp16')
            f = 'df[0]' if ty else 'f[0]'
            ft = 'double' if ty else 'float'
            a, b, r = 'c->v[%d].%s' % (rn, f), 'c->v[%d].%s' % (rm, f), 'c->v[%d].%s' % (bits(w, 14, 10), f)
            o1, o0 = bits(w, 21, 21), bits(w, 15, 15)
            fma = 'fma' if ty else 'fmaf'
            expr = ['%s(%s, %s, %s)', '%s(-%s, %s, %s)', '%s(-%s, %s, -%s)', '%s(%s, %s, -%s)'][o1 * 2 + o0] % (fma, a, b, r)
            return '{ %s t = fnan%d(%s); vzero(&c->v[%d]); c->v[%d].%s = t; }' % (ft, 64 if ty else 32, expr, rd, rd, f)
        return self.simd(w, pc)

    def simd(self, w, pc):
        rd, rn, rm = bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
        if (w & 0x9ff80c00) == 0x0f000400:               # movi / mvni / orr-imm (modified immediate)
            q, op, cmode = bits(w, 30, 30), bits(w, 29, 29), bits(w, 15, 12)
            imm8 = (bits(w, 18, 16) << 5) | bits(w, 9, 5)
            def vfp(double):
                sign = -1.0 if imm8 & 0x80 else 1.0
                value = sign * (16 + (imm8 & 0xf)) / 16.0 * (2.0 ** ((((imm8 >> 4) & 7) ^ 4) - 3))
                if double:
                    return struct.unpack('<Q', struct.pack('<d', value))[0]
                return struct.unpack('<I', struct.pack('<f', value))[0] * 0x100000001
            if cmode == 15:
                val = vfp(op == 1)
            elif cmode == 14 and op == 1:
                val = sum(0xff << (8 * i) for i in range(8) if imm8 & (1 << i))
            elif cmode == 14:
                val = imm8 * 0x0101010101010101
            elif cmode in (0, 2, 4, 6, 12, 13):
                v32 = imm8 << (8 * (cmode >> 1)) if cmode < 12 else ((imm8 << 8) | 0xff if cmode == 12 else (imm8 << 16) | 0xffff)
                if op:
                    v32 ^= 0xffffffff
                val = v32 * 0x100000001
            elif cmode in (8, 10):
                v16 = imm8 << (8 * ((cmode >> 1) & 1))
                if op:
                    v16 ^= 0xffff
                val = v16 * 0x0001000100010001
            else:
                raise Unimpl('simd-imm')
            return '{ c->v[%d].d[0] = 0x%xULL; c->v[%d].d[1] = %s; }' % (rd, val, rd, ('0x%xULL' % val) if q else '0')
        if (w & 0xbfe0fc00) == 0x0ea01c00 and rn == rm:  # mov vD.16b, vN.16b (orr)
            q = bits(w, 30, 30)
            return '{ V128 t = c->v[%d]; %s c->v[%d] = t; }' % (rn, '' if q else 't.d[1] = 0;', rd)
        bitwise = {0x0e201c00: 'a & b', 0x2e201c00: 'a ^ b', 0x0e601c00: 'a & ~b', 0x0ea01c00: 'a | b',
                   0x0ee01c00: 'a | ~b'}
        if (w & 0xbfe0fc00) in bitwise:                  # and / eor / bic / orr / orn
            q = bits(w, 30, 30)
            opn = bitwise[w & 0xbfe0fc00]
            body = ''.join('{ uint64_t a = c->v[%d].d[%d], b = c->v[%d].d[%d]; t.d[%d] = %s; } ' % (rn, i, rm, i, i, opn)
                           for i in range(2 if q else 1))
            return '{ V128 t; t.d[1] = 0; %s c->v[%d] = t; }' % (body, rd)
        if bits(w, 31, 31) or bits(w, 27, 25) != 7:
            raise Unimpl('simd')
        scalar, q, u = bits(w, 28, 28), bits(w, 30, 30), bits(w, 29, 29)
        if scalar and not q:
            raise Unimpl('simd')
        size, sz, a = bits(w, 23, 22), bits(w, 22, 22), bits(w, 23, 23)

        def V(n):
            return '&c->v[%d]' % n

        def fcount():
            return 1 if scalar else ((2 if sz else 4) if q else (1 if sz else 2))

        def icount(es):
            return 1 if scalar else (16 if q else 8) >> es

        if bits(w, 27, 24) == 0xe and bits(w, 21, 21):
            if bits(w, 10, 10):                          # three registers, same type
                opc = bits(w, 15, 11)
                if opc >= 0x18:
                    op = {(0, 0, 0x1a): 'F_ADD', (0, 1, 0x1a): 'F_SUB', (1, 0, 0x1b): 'F_MUL', (1, 0, 0x1f): 'F_DIV',
                          (0, 0, 0x19): 'F_MLA', (0, 1, 0x19): 'F_MLS', (0, 0, 0x1e): 'F_MAX', (0, 1, 0x1e): 'F_MIN',
                          (0, 0, 0x18): 'F_MAXNM', (0, 1, 0x18): 'F_MINNM', (1, 0, 0x1a): 'F_ADDP', (1, 1, 0x1a): 'F_ABD',
                          (0, 0, 0x1c): 'F_CMEQ', (1, 0, 0x1c): 'F_CMGE', (1, 1, 0x1c): 'F_CMGT',
                          (0, 0, 0x1f): 'F_RECPS', (0, 1, 0x1f): 'F_RSQRTS'}.get((u, a, opc))
                    if op is None or (op == 'F_ADDP' and scalar):
                        raise Unimpl('simd-f3')
                    return 'v_f3v(%s, %s, %s, %s, %d, %d);' % (V(rd), V(rn), V(rm), op, sz, fcount())
                if opc == 3 and u and size:
                    return 'v_bitsel(%s, %s, %s, %d, %d);' % (V(rd), V(rn), V(rm), size, q)
                op = {(0, 0x10): 'I_ADD', (1, 0x10): 'I_SUB', (0, 0x13): 'I_MUL', (1, 0x11): 'I_CMEQ', (0, 0x11): 'I_CMTST',
                      (0, 0x06): 'I_CMGT', (1, 0x06): 'I_CMHI', (0, 0x07): 'I_CMGE', (1, 0x07): 'I_CMHS'}.get((u, opc))
                if op is None:
                    raise Unimpl('simd-i3')
                return 'v_i3v(%s, %s, %s, %s, %d, %d);' % (V(rd), V(rn), V(rm), op, size, icount(size))
            if bits(w, 20, 17) == 0 and bits(w, 11, 10) == 2:    # two registers, miscellaneous
                opc = bits(w, 16, 12)
                op = {(0, 1, 0x0f): 'F1_ABS', (1, 1, 0x0f): 'F1_NEG', (1, 1, 0x1f): 'F1_SQRT', (1, 1, 0x1d): 'F1_RSQRTE',
                      (0, 1, 0x1d): 'F1_RECPE', (0, 0, 0x1d): 'F1_SCVTF', (1, 0, 0x1d): 'F1_UCVTF', (0, 1, 0x1b): 'F1_CVTZS',
                      (1, 1, 0x1b): 'F1_CVTZU', (0, 1, 0x0d): 'F1_CMEQ0', (0, 1, 0x0c): 'F1_CMGT0', (1, 1, 0x0c): 'F1_CMGE0',
                      (0, 1, 0x0e): 'F1_CMLT0', (1, 1, 0x0d): 'F1_CMLE0'}.get((u, a, opc))
                if op:
                    return 'v_f1v(%s, %s, %s, %d, %d);' % (V(rd), V(rn), op, sz, fcount())
                if (u, opc) == (0, 0x12) and not scalar and size < 3:
                    return 'v_xtn(%s, %s, %d);' % (V(rd), V(rn), size)
                op = {(1, 0x05): 'I1_NOT', (0, 0x05): 'I1_CNT', (1, 0x0b): 'I1_NEG', (0, 0x0b): 'I1_ABS',
                      (0, 0x09): 'I1_CMEQ0', (0, 0x00): 'I1_REV64'}.get((u, opc))
                if op is None or (op in ('I1_NOT', 'I1_CNT') and size):
                    raise Unimpl('simd-misc')
                return 'v_i1v(%s, %s, %s, %d, %d);' % (V(rd), V(rn), op, size, icount(size))
            if scalar and u and bits(w, 20, 17) == 8 and bits(w, 16, 12) == 0x0d and bits(w, 11, 10) == 2:
                f, ft = ('df', 'double') if sz else ('f', 'float')   # faddp scalar
                return '{ %s t = c->v[%d].%s[0] + c->v[%d].%s[1]; vzero(%s); c->v[%d].%s[0] = t; }' % (ft, rn, f, rn, f, V(rd), rd, f)
            raise Unimpl('simd-e')
        if bits(w, 27, 24) == 0xe:
            if bits(w, 15, 15) == 0 and bits(w, 10, 10) == 1:    # copy: dup / ins / umov / smov
                imm5, imm4 = bits(w, 20, 16), bits(w, 14, 11)
                if imm5 & 0xf == 0:
                    raise Unimpl('simd-copy')
                es = (imm5 & -imm5).bit_length() - 1
                idx = imm5 >> (es + 1)
                if scalar:
                    if u or imm4:
                        raise Unimpl('simd-copy')
                    return '{ uint64_t t = vget(%s, %d, %d); vzero(%s); vset(%s, %d, 0, t); }' % (V(rn), es, idx, V(rd), V(rd), es)
                if u:
                    return 'vset(%s, %d, %d, vget(%s, %d, %d));' % (V(rd), es, idx, V(rn), es, imm4 >> es)
                if imm4 == 0:
                    return 'v_dup(%s, vget(%s, %d, %d), %d, %d);' % (V(rd), V(rn), es, idx, es, icount(es))
                if imm4 == 1:
                    return 'v_dup(%s, %s, %d, %d);' % (V(rd), X(rn), es, icount(es))
                if imm4 == 3:
                    return 'vset(%s, %d, %d, %s);' % (V(rd), es, idx, X(rn))
                if imm4 == 7:
                    return SET(rd, 1, 'vget(%s, %d, %d)' % (V(rn), es, idx))
                if imm4 == 5:
                    return SET(rd, q, 'vsext(vget(%s, %d, %d), %d)' % (V(rn), es, idx, es))
                raise Unimpl('simd-copy')
            if bits(w, 15, 15) == 0 and bits(w, 10, 10) == 0 and not scalar:
                if u:
                    return 'v_ext(%s, %s, %s, %d, %d);' % (V(rd), V(rn), V(rm), bits(w, 14, 11), q)
                if bits(w, 11, 10) == 0:
                    return 'v_tbl(c, %d, %d, %d, %d, %d, %d);' % (rd, rn, bits(w, 14, 13) + 1, rm, q, bits(w, 12, 12))
                return 'v_perm(%s, %s, %s, %d, %d, %d);' % (V(rd), V(rn), V(rm), bits(w, 14, 12), size, icount(size))
            raise Unimpl('simd-e')
        if bits(w, 10, 10) == 0:                          # by element
            op = {9: 'F_MUL', 1: 'F_MLA', 5: 'F_MLS'}.get(bits(w, 15, 12))
            if op is None or u or not a:
                raise Unimpl('simd-elem')
            idx = bits(w, 11, 11) if sz else (bits(w, 11, 11) << 1) | bits(w, 21, 21)
            n = 1 if scalar else ((2 if sz else 4) if q else 2)
            return 'v_f3e(%s, %s, %s, %d, %s, %d, %d);' % (V(rd), V(rn), V(bits(w, 20, 16)), idx, op, sz, n)
        immh, immhb, opc = bits(w, 22, 19), bits(w, 22, 16), bits(w, 15, 11)
        if immh == 0:
            raise Unimpl('simd-imm')
        es = immh.bit_length() - 1
        if opc == 0x14 and not scalar and es < 3:
            return 'v_shll(%s, %s, %d, %d, %d, %d);' % (V(rd), V(rn), immhb - (8 << es), es, u, q)
        if opc == 0x0a and not u:
            return 'v_shift(%s, %s, %d, %d, 0, %d);' % (V(rd), V(rn), immhb - (8 << es), es, icount(es))
        if opc == 0:
            return 'v_shift(%s, %s, %d, %d, %d, %d);' % (V(rd), V(rn), (16 << es) - immhb, es, 2 if u else 1, icount(es))
        raise Unimpl('simd-shift')


# ---------------------------------------------------------------- program level
class Env:
    def __init__(self):
        self.starts = None
        self.wanted = collections.OrderedDict()
        self.overrides = {}
        self.plt_cache = {}
        self.plt_slot = {}
        self.plt_syms = None
        self.full = False

    def load_plt(self):
        self.plt_syms = {}
        self.plt_info = {}
        for i in range(PLTRELSZ // 24):
            off, info, _ = struct.unpack_from('<QQq', IMG, FILE_OFF + JMPREL + i * 24)
            name_off = struct.unpack_from('<I', IMG, FILE_OFF + SYMTAB + (info >> 32) * 24)[0]
            s = FILE_OFF + STRTAB + name_off
            slot = BASE + off if off < BASE else off
            self.plt_syms[slot] = IMG[s:IMG.index(b'\0', s)].decode('latin-1')
            shndx, value = struct.unpack_from('<HQ', IMG, FILE_OFF + SYMTAB + (info >> 32) * 24 + 6)
            self.plt_info[slot] = (info >> 32, shndx, value)

    def plt_symbol(self, target):
        """If target is a PLT stub, return the imported symbol name."""
        if target in self.plt_cache:
            return self.plt_cache[target]
        name = None
        if target + 16 <= BASE + TEXT_SIZE:
            w0, w1, w2, w3 = (word(target + i) for i in (0, 4, 8, 12))
            if (w0 & 0x9f00001f) == 0x90000010 and (w1 & 0xffc003ff) == 0xf9400211 and w3 == 0xd61f0220:
                if self.plt_syms is None:
                    self.load_plt()
                page = (target & ~0xfff) + (sext((bits(w0, 23, 5) << 2) | bits(w0, 30, 29), 21) << 12)
                name = self.plt_syms.get(page + bits(w1, 21, 10) * 8)
                if name is not None:
                    self.plt_slot[target] = page + bits(w1, 21, 10) * 8
        self.plt_cache[target] = name
        return name

    def plt_entry(self, target):
        """(symbol index, section index, value) when target is a PLT stub, else None."""
        if self.plt_symbol(target) is None:
            return None
        return self.plt_info[self.plt_slot[target]]

    def call(self, target):
        if target in self.overrides:
            return 'h_%s(c);' % self.overrides[target]
        if self.full:
            self.wanted.setdefault(target, None)
            return 'f_%x(c);' % target
        name = self.plt_symbol(target)
        if name:
            self.imports.add(name)
            return 'imp_%s(c);' % mangle(name)
        self.wanted.setdefault(target, None)
        return 'f_%x(c);' % target

    def compute_starts(self):
        """Function entries: every BL target plus every code address stored in data (relative relocs)."""
        starts = set()
        text = IMG[FILE_OFF:FILE_OFF + TEXT_SIZE]
        for i, (w,) in enumerate(struct.iter_unpack('<I', text)):
            if (w & 0xfc000000) == 0x94000000:
                starts.add(BASE + i * 4 + sext(w & 0x3ffffff, 26) * 4)
        for i in range(RELASZ // 24):
            off, info, addend = struct.unpack_from('<QQq', IMG, FILE_OFF + RELA + i * 24)
            if info == 0x403 and 0 <= addend < TEXT_SIZE and addend % 4 == 0:
                starts.add(BASE + addend)
        # code addresses materialised in registers (callbacks, thread entry points): ADRP+ADD pairs and ADR
        page = {}
        for i, (w,) in enumerate(struct.iter_unpack('<I', text)):
            if (w & 0x9f000000) == 0x90000000:                       # adrp
                imm = sext((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21)
                page[w & 31] = (i, ((i * 4) & ~0xfff) + (imm << 12))
            elif (w & 0xffc00000) == 0x91000000:                     # add xd, xn, #imm12
                hit = page.get((w >> 5) & 31)
                if hit and i - hit[0] <= 8:
                    target = hit[1] + ((w >> 10) & 0xfff)
                    if 0 <= target < TEXT_SIZE and target % 4 == 0:
                        starts.add(BASE + target)
            elif (w & 0x9f000000) == 0x10000000:                     # adr
                imm = sext((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3), 21)
                target = i * 4 + imm
                if 0 <= target < TEXT_SIZE and target % 4 == 0:
                    starts.add(BASE + target)
        # functions other modules call by name: exported FUNC symbols
        for i in range((STRTAB - SYMTAB) // 24):
            _, info, _, shndx, value, _ = struct.unpack_from('<IBBHQQ', IMG, FILE_OFF + SYMTAB + i * 24)
            if shndx and (info & 0xf) == 2 and value % 4 == 0:
                starts.add(BASE + value)
        self.starts = sorted(a for a in starts if BASE <= a < BASE + TEXT_SIZE)
        return self.starts

    def extent(self, entry):
        import bisect
        i = bisect.bisect_right(self.starts, entry)
        return self.starts[i] if i < len(self.starts) else BASE + TEXT_SIZE


def mangle(name):
    return ''.join(ch if ch.isalnum() else '_' for ch in name)


def lift_function(env, entry):
    if env.full:
        plt = env.plt_entry(entry)
        if plt:
            sym, shndx, value = plt
            if shndx:                                    # symbol defined inside this module
                env.wanted.setdefault(BASE + value, None)
                return 'void f_%x(Ctx *c) { f_%x(c); }' % (entry, BASE + value), 0, []
            return 'void f_%x(Ctx *c) { host_import(c, %d); }' % (entry, sym), 0, []
    end = env.extent(entry)
    fn = Fn(entry, end, env)
    lines, bad = [], []
    pc = entry
    while pc < end:
        w = word(pc)
        try:
            stmt = fn.lift(w, pc)
        except Unimpl as e:
            stmt = 'guest_unimpl(0x%xULL, 0x%08xU, "%s");' % (pc, w, e)
            bad.append((pc, w, str(e)))
        lines.append((pc, stmt))
        pc += 4
    out = ['void f_%x(Ctx *c) {' % entry, '  c->fn = 0x%xULL;' % entry]   # for crash reports
    if fn.has_br:
        out.append('  uint64_t jt = 0;')
    for pc, stmt in lines:
        out.append('L_%x: %s' % (pc, stmt))
    out.append('  f_%x(c); return;' % end if end < BASE + TEXT_SIZE and not lines[-1][1].startswith(('return;', 'goto', '{ jt', '{ f_', '{ imp_', '{ h_')) else '  return;')
    if fn.has_br:
        out.append('L_dispatch: switch (jt) {')
        out.extend('  case 0x%xULL: goto L_%x;' % (pc, pc) for pc, _ in lines)
        out.append('  default: call_indirect(c, jt); return; }')
    out.append('}')
    if lines and not lines[-1][1].startswith(('return;', 'goto', '{ jt', '{ f_', '{ imp_', '{ h_')) and end < BASE + TEXT_SIZE:
        env.wanted.setdefault(end, None)
    return '\n'.join(out), len(lines), bad


def cmd_lift(out_path, roots, overrides):
    env = Env()
    env.imports = set()
    env.overrides = overrides
    env.compute_starts()
    for r in roots:
        env.wanted.setdefault(r, None)
        if r not in env.starts:
            import bisect
            bisect.insort(env.starts, r)
    bodies, done, total, allbad = [], [], 0, []
    while True:
        todo = [a for a in env.wanted if a not in done]
        if not todo:
            break
        for a in todo:
            body, n, bad = lift_function(env, a)
            bodies.append(body)
            done.append(a)
            total += n
            allbad += bad
    with open(out_path, 'w') as f:
        f.write('/* generated by recomp.py -- do not edit */\n#include "runtime.h"\n\n')
        for name in sorted(env.imports):
            f.write('void imp_%s(Ctx *c);\n' % mangle(name))
        for name in sorted(set(overrides.values())):
            f.write('void h_%s(Ctx *c);\n' % name)
        for a in done:
            f.write('void f_%x(Ctx *c);\n' % a)
        f.write('\n' + '\n\n'.join(bodies) + '\n\n')
        f.write('void call_indirect(Ctx *c, uint64_t a) {\n  switch (a) {\n')
        for a in done:
            f.write('  case 0x%xULL: f_%x(c); return;\n' % (a, a))
        for a, name in overrides.items():
            f.write('  case 0x%xULL: h_%s(c); return;\n' % (a, name))
        f.write('  default: guest_trap("indirect call to a function that was not lifted", a);\n  }\n}\n')
    print('functions lifted: %d   instructions: %d   not translated: %d' % (len(done), total, len(allbad)))
    print('imports used:', ', '.join(sorted(env.imports)))
    reasons = collections.Counter(r for _, _, r in allbad)
    for r, n in reasons.most_common(15):
        ex = next('%x:%08x' % (pc, w) for pc, w, rr in allbad if rr == r)
        print('  unimplemented %-14s %5d   e.g. %s' % (r, n, ex))


_worker_env = None


def _worker_init(starts):
    global _worker_env
    _worker_env = Env()
    _worker_env.imports = set()
    _worker_env.full = True
    _worker_env.starts = starts


def _worker_lift(entries):
    env = _worker_env
    env.wanted = collections.OrderedDict()
    out, total, bad = [], 0, 0
    for a in entries:
        body, n, b = lift_function(env, a)
        out.append(body)
        total += n
        bad += len(b)
    return entries[0], '\n\n'.join(out), total, bad, list(env.wanted)


def cmd_full(outdir, jobs, chunk_insns=12000):
    """Lift every function of the binary into <outdir>/c_NNNN.c plus funcs.h, table.c, imports.c."""
    import multiprocessing
    import os
    import time
    t0 = time.time()
    env = Env()
    env.imports = set()
    env.full = True
    starts = env.compute_starts()
    print('function entries: %d  (%.0fs)' % (len(starts), time.time() - t0))
    os.makedirs(outdir, exist_ok=True)
    known = set(starts)
    pending = list(starts)
    files, all_funcs = [], []
    total = bad = 0
    round_no = 0
    with multiprocessing.Pool(jobs, _worker_init, (starts,)) as pool:
        while pending:
            pending.sort()
            chunks, cur, size = [], [], 0
            for i, a in enumerate(pending):
                cur.append(a)
                size += (env.extent(a) - a) // 4
                if size >= chunk_insns:
                    chunks.append(cur)
                    cur, size = [], 0
            if cur:
                chunks.append(cur)
            new = set()
            for first, text, n, b, wanted in pool.imap_unordered(_worker_lift, chunks):
                name = 'c_%x.c' % (first - BASE)
                path = os.path.join(outdir, name)
                content = '/* generated by recomp.py -- do not edit */\n#include "runtime.h"\n#include "funcs.h"\n\n' + text + '\n'
                if not os.path.exists(path) or open(path).read() != content:
                    open(path, 'w').write(content)
                files.append(name)
                total += n
                bad += b
                new.update(w for w in wanted if w not in known)
            all_funcs += pending
            round_no += 1
            print('round %d: %d functions, %d files so far, %d extra targets  (%.0fs)'
                  % (round_no, len(pending), len(files), len(new), time.time() - t0))
            known |= new
            pending = [a for a in new if BASE <= a < BASE + TEXT_SIZE]
            outside = [a for a in new if not (BASE <= a < BASE + TEXT_SIZE)]
            if outside:
                print('  branch targets outside .text (ignored):', ', '.join('%x' % a for a in outside[:8]))
            all_funcs += []
    all_funcs = sorted(set(all_funcs))
    with open(os.path.join(outdir, 'funcs.h'), 'w') as f:
        f.write('/* generated by recomp.py -- do not edit */\n')
        f.writelines('void f_%x(Ctx *c);\n' % a for a in all_funcs)
    with open(os.path.join(outdir, 'table.c'), 'w') as f:
        f.write('/* generated by recomp.py -- do not edit */\n#include "runtime.h"\n#include "funcs.h"\n\n')
        f.write('const FnEntry fn_table[] = {\n')
        f.writelines('  { 0x%xULL, f_%x },\n' % (a, a) for a in all_funcs)
        f.write('};\nconst int fn_table_count = %d;\n' % len(all_funcs))
    # every dynamic symbol by index: name, and whether this module defines it
    nsym = (STRTAB - SYMTAB) // 24
    with open(os.path.join(outdir, 'imports.c'), 'w') as f:
        f.write('/* generated by recomp.py -- do not edit */\n#include "runtime.h"\n\nconst DynSym dyn_syms[] = {\n')
        for i in range(nsym):
            name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', IMG, FILE_OFF + SYMTAB + i * 24)
            p = FILE_OFF + STRTAB + name_off
            name = IMG[p:IMG.index(b'\0', p)].decode('latin-1')
            f.write('  { "%s", 0x%xULL, %d },\n' % (name, value, 1 if shndx else 0))
        f.write('};\nconst int dyn_sym_count = %d;\n' % nsym)
    stale = set(n for n in os.listdir(outdir) if n.startswith('c_') and n.endswith('.c')) - set(files)
    for n in stale:
        os.remove(os.path.join(outdir, n))
    print('functions: %d   files: %d   instructions: %d   not translated: %d   (%.0fs)'
          % (len(all_funcs), len(files), total, bad, time.time() - t0))


BUCKET = 0xC000          # one output file per 48 KB of guest code: file contents stay stable across runs


def _worker_scan(entries):
    """Discovery pass: lift without keeping the text, report statistics and new branch targets."""
    env = _worker_env
    env.wanted = collections.OrderedDict()
    total = bad = 0
    for a in entries:
        _, n, b = lift_function(env, a)
        total += n
        bad += len(b)
    return total, bad, list(env.wanted)


def _worker_write(job):
    outdir, name, entries = job
    import os
    env = _worker_env
    env.wanted = collections.OrderedDict()
    text = '\n\n'.join(lift_function(env, a)[0] for a in entries)
    content = '/* generated by recomp.py -- do not edit */\n#include "runtime.h"\n#include "funcs.h"\n\n' + text + '\n'
    path = os.path.join(outdir, name)
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == content:
                return name, False
    with open(path, 'w') as f:
        f.write(content)
    return name, True


def cmd_full2(outdir, jobs):
    """Lift every function of the binary into <outdir>/c_<bucket>.c plus funcs.h, table.c, imports.c."""
    import multiprocessing
    import os
    import time
    t0 = time.time()
    env = Env()
    env.imports = set()
    env.full = True
    starts = env.compute_starts()
    print('function entries: %d  (%.0fs)' % (len(starts), time.time() - t0))
    os.makedirs(outdir, exist_ok=True)
    known = set(starts)
    pending = list(starts)
    all_funcs = []
    total = bad = 0
    with multiprocessing.Pool(jobs, _worker_init, (starts,)) as pool:
        while pending:
            pending.sort()
            parts = [pending[i:i + 400] for i in range(0, len(pending), 400)]
            new = set()
            for n, b, wanted in pool.imap_unordered(_worker_scan, parts):
                total += n
                bad += b
                new.update(w for w in wanted if w not in known and BASE <= w < BASE + TEXT_SIZE)
            all_funcs += pending
            print('discovery: %d functions, %d extra branch targets  (%.0fs)' % (len(pending), len(new), time.time() - t0))
            known |= new
            pending = list(new)
        all_funcs.sort()
        buckets = collections.OrderedDict()
        for a in all_funcs:
            buckets.setdefault((a - BASE) // BUCKET, []).append(a)
        jobs_list = [(outdir, 'c_%04x.c' % b, entries) for b, entries in buckets.items()]
        changed = 0
        files = []
        for name, was_changed in pool.imap_unordered(_worker_write, jobs_list):
            files.append(name)
            changed += was_changed
    with open(os.path.join(outdir, 'funcs.h'), 'w') as f:
        f.write('/* generated by recomp.py -- do not edit */\n')
        f.writelines('void f_%x(Ctx *c);\n' % a for a in all_funcs)
    with open(os.path.join(outdir, 'table.c'), 'w') as f:
        f.write('/* generated by recomp.py -- do not edit */\n#include "runtime.h"\n#include "funcs.h"\n\n')
        f.write('const FnEntry fn_table[] = {\n')
        f.writelines('  { 0x%xULL, f_%x },\n' % (a, a) for a in all_funcs)
        f.write('};\nconst int fn_table_count = %d;\n' % len(all_funcs))
    nsym = (STRTAB - SYMTAB) // 24
    with open(os.path.join(outdir, 'imports.c'), 'w') as f:
        f.write('/* generated by recomp.py -- do not edit */\n#include "runtime.h"\n\nconst DynSym dyn_syms[] = {\n')
        for i in range(nsym):
            name_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', IMG, FILE_OFF + SYMTAB + i * 24)
            p = FILE_OFF + STRTAB + name_off
            name = IMG[p:IMG.index(b'\0', p)].decode('latin-1')
            f.write('  { "%s", 0x%xULL, %d },\n' % (name, value, 1 if shndx else 0))
        f.write('};\nconst int dyn_sym_count = %d;\n' % nsym)
    keep = set(files) | {'table.c', 'imports.c'}
    for n in os.listdir(outdir):
        if n.endswith('.c') and n not in keep:
            os.remove(os.path.join(outdir, n))
    print('functions: %d   files: %d (%d rewritten)   instructions: %d   not translated: %d   (%.0fs)'
          % (len(all_funcs), len(files), changed, total, bad, time.time() - t0))


def cmd_cover():
    env = Env()
    env.imports = set()
    starts = env.compute_starts()
    fn = Fn(BASE, BASE + TEXT_SIZE, env)
    ok = bad = 0
    reasons = collections.Counter()
    cache = {}
    text = IMG[FILE_OFF:FILE_OFF + TEXT_SIZE]
    for i, (w,) in enumerate(struct.iter_unpack('<I', text)):
        r = cache.get(w)
        if r is None:
            try:
                fn.lift(w, BASE + 0x1000000)
                r = ''
            except Unimpl as e:
                r = str(e)
            except Exception as e:                       # decoder bug: count as unimplemented
                r = 'error'
            if len(cache) < 4000000:
                cache[w] = r
        if r:
            bad += 1
            reasons[r] += 1
        else:
            ok += 1
    total = ok + bad
    print('function entries (BL targets + code pointers in data): %d' % len(starts))
    print('instruction words: %d   translated: %d (%.2f%%)   not translated: %d (%.2f%%)'
          % (total, ok, 100.0 * ok / total, bad, 100.0 * bad / total))
    for r, n in reasons.most_common(20):
        print('  %-14s %9d  %.2f%%' % (r, n, 100.0 * n / total))


if __name__ == '__main__':
    if sys.argv[1] == 'cover':
        cmd_cover()
    elif sys.argv[1] == 'full':
        cmd_full2(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 16)
    else:
        ov = {}
        for item in sys.argv[4:]:
            a, name = item.split('=')
            ov[int(a, 16)] = name
        cmd_lift(sys.argv[2], [int(a, 16) for a in sys.argv[3].split(',')], ov)
