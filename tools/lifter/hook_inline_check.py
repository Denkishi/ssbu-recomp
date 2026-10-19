"""Can the hook table hook these functions in the compiled game? (relift_sites.py `noinline`, fighter_sdk\\hook_host.c)

    python hook_inline_check.py <address file | site list | addr,addr,...> [--obj=obj_eh_o1] [--gen=gen_eh_o2] [--module=<name>]
                                [--emit=<file>] [--census-all]

The hook table rewrites only the ENTRY of a lifted function in the loaded DLL (5 bytes: jmp). That is complete only if
every static caller still reaches the entry: a direct call or a tail jump to f_<addr>. Callers in other bucket files
always do (separate translation units, no LTO); a caller in the SAME file may have had f_<addr> inlined (an optimised
build can: clang 23 -O2 does, e.g. the fall-through `f_<next>(c); return;` at the end of a function), or call a clone of it.
Defaults: the base set of scripts/setup.py (gen_eh_o2 / obj_eh_o1; --module: gen_nro_eh_o2\\<m> / obj_nro_eh_o2\\<m>).
For each address:
  1. the lifted callers in its own file and how many call sites each has (`f_<addr>(c)` in the generated C: BL, B to
     another function, fall-through), from <gen>\\c_XXXX.c, split into reachable and dead sites (control flow of the
     lifted function: a fall-through call behind the padding after a `ret` can never run, the compiler drops it);
  2. the compiled callers: call / jmp / jcc instructions whose relocation (or symbolised target) is f_<addr>, per
     function, from `llvm-objdump -d -r` of <obj>\\c_XXXX.obj; plus the constant <addr> itself (the inlined copy's
     `c->fn = <addr>` store) in a function other than f_<addr>;
  3. clones: symbols f_<addr>.<suffix>;
  4. the entry: hook_host.c's hk_len / hk_move (dry run) on the first bytes of f_<addr> in the object.
Verdict INLINED when some caller in the file has fewer compiled call/jump sites than reachable lifted ones (conservative:
two sites the compiler merged count too), CLONED when a clone exists, SUSPECT when only the address constant is found
elsewhere and the lifted C of that function has no such constant (a BL just before f_<addr> sets x30 = <addr>); else OK. --emit writes `noinline <addr>` lines for every address that is not OK.
--census-all: the entry census (4) over every function of <obj> (SSBU_HOOK_SELFTEST's census, offline).
"""
import collections
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OBJDUMP = os.environ.get('RECOMP_OBJDUMP') or os.path.join(os.path.dirname(os.environ.get('RECOMP_CLANG', r'C:\Program Files\LLVM\bin\clang.exe')), 'llvm-objdump.exe')
BASE, BUCKET = 0x7100000000, 0xC000


# ---------------------------------------------------------------- hook_host.c's x86-64 entry mover, in Python (dry run)
def hk_modrm(p, i):
    mod, rm, n, rip = p[i] >> 6, p[i] & 7, 1, 0
    if mod == 3:
        return 1, 0
    if rm == 4:
        n += 1
        if mod == 0 and (p[i + 1] & 7) == 5:
            n += 4
    if mod == 0 and rm == 5:
        rip = n
        n += 4
    elif mod == 1:
        n += 1
    elif mod == 2:
        n += 4
    return n, rip


def hk_len(p, at):
    """(length, fix, kind) as hook_host.c hk_len; length 0 = refused"""
    i, op16, rexw, imm, modrm = 0, 0, 0, 0, 0
    fix, kind = 0, 0
    while True:
        b = p[at + i]
        if b == 0x66:
            op16 = 1
        elif b not in (0xF2, 0xF3, 0x2E, 0x3E, 0x36, 0x26, 0x64, 0x65):
            break
        if i > 3:
            return 0, 0, 0
        i += 1
    if (p[at + i] & 0xF0) == 0x40:
        rexw = p[at + i] & 8
        i += 1
    o = p[at + i]
    i += 1
    if o == 0x0F:
        o2 = p[at + i]
        i += 1
        if (o2 & 0xF0) == 0x80:
            return i + 4, i, 0
        if o2 == 0x0B or 0xC8 <= o2 <= 0xCF:
            return i, 0, 0
        if o2 in (0x05, 0x34, 0x77, 0xA2, 0x31, 0x01):
            return 0, 0, 0
        if o2 == 0x38:
            i += 1
            modrm = 1
        elif o2 == 0x3A:
            i += 1
            modrm, imm = 1, 1
        else:
            modrm = 1
            if 0x70 <= o2 <= 0x73 or o2 in (0xA4, 0xAC, 0xBA, 0xC2, 0xC4, 0xC5, 0xC6):
                imm = 1
    elif o < 0x40:
        k = o & 7
        if k < 4:
            modrm = 1
        elif k == 4:
            imm = 1
        elif k == 5:
            imm = 2 if op16 else 4
        else:
            return 0, 0, 0
    elif 0x50 <= o <= 0x5F:
        pass
    elif o == 0x63 or 0x84 <= o <= 0x8F:
        modrm = 1
    elif o == 0x68:
        imm = 4
    elif o == 0x6A:
        imm = 1
    elif o == 0x69:
        modrm, imm = 1, (2 if op16 else 4)
    elif o == 0x6B:
        modrm, imm = 1, 1
    elif 0x70 <= o <= 0x7F:
        if i != 1:
            return 0, 0, 0
        return 2, 0, 3
    elif o in (0x80, 0x83, 0xC0, 0xC1, 0xC6):
        modrm, imm = 1, 1
    elif o in (0x81, 0xC7):
        modrm, imm = 1, (2 if op16 else 4)
    elif o in (0x90, 0x98, 0x99, 0xC9, 0xCC):
        pass
    elif o == 0xA8:
        imm = 1
    elif o == 0xA9:
        imm = 2 if op16 else 4
    elif 0xB0 <= o <= 0xB7:
        imm = 1
    elif 0xB8 <= o <= 0xBF:
        imm = 8 if rexw else 2 if op16 else 4
    elif o == 0xC3:
        return i, 0, 5
    elif 0xD0 <= o <= 0xD3:
        modrm = 1
    elif o == 0xE9:
        return i + 4, i, 5
    elif o == 0xEB:
        if i != 1:
            return 0, 0, 0
        return 2, 0, 4
    elif o in (0xF6, 0xF7):
        modrm = 1
        if ((p[at + i] >> 3) & 7) < 2:
            imm = 1 if o == 0xF6 else 2 if op16 else 4
    elif o == 0xFE:
        modrm = 1
    elif o == 0xFF:
        r = (p[at + i] >> 3) & 7
        modrm = 1
        if r in (2, 3, 5):
            return 0, 0, 0
        if r == 4:
            kind = 5
    else:
        return 0, 0, 0
    if modrm:
        m, rip = hk_modrm(p, at + i)
        if rip:
            fix = i + rip
        i += m
    return i + imm, fix, kind


def hk_move(p, at):
    """(bytes moved, why) as hook_host.c hk_move(dry=1): 0 = refused"""
    n, ended, tos = 0, False, []
    while n < 5:
        if ended:
            if p[at + n] != 0xCC:
                return 0, 'function shorter than 5 bytes'
            n += 1
            continue
        ln, fix, kind = hk_len(p, at + n)
        if not ln:
            return 0, 'entry instruction not known to the mover'
        if kind in (3, 4):
            rel = p[at + n + 1] - 256 if p[at + n + 1] > 127 else p[at + n + 1]
            tos.append(n + 2 + rel)
            if kind == 4:
                ended = True
        else:
            if fix:
                rel = struct.unpack_from('<i', p, at + n + fix)[0]
                tos.append(n + ln + rel)
            if kind == 5:
                ended = True
        n += ln
    for t in tos:
        if 0 < t < n:
            return 0, 'branch into the moved entry bytes'
    return n, ''


# ---------------------------------------------------------------- COFF
def coff_functions(path):
    """{symbol: (bytes of its section, offset)} for the f_ / u_ symbols of an object; also the list of all names"""
    data = open(path, 'rb').read()
    machine, nsec, _, symptr, nsym, optsz, _ = struct.unpack_from('<HHIIIHH', data, 0)
    sections = []
    for i in range(nsec):
        name, vsize, va, rsize, rptr = struct.unpack_from('<8sIIII', data, 20 + optsz + i * 40)
        sections.append((rptr, rsize))
    strtab = symptr + nsym * 18
    out, names = {}, []
    i = 0
    while i < nsym:
        e = symptr + i * 18
        raw = data[e:e + 8]
        if raw[:4] == b'\0\0\0\0':
            o = struct.unpack_from('<I', raw, 4)[0]
            name = data[strtab + o:data.index(b'\0', strtab + o)].decode('latin-1')
        else:
            name = raw.rstrip(b'\0').decode('latin-1')
        value, secno, typ, cls, aux = struct.unpack_from('<IhHBB', data, e + 8)
        names.append(name)
        if secno > 0 and name[:2] in ('f_', 'u_'):
            rptr, rsize = sections[secno - 1]
            out[name] = (rptr + value, rptr + rsize)
        i += 1 + aux
    return data, out, names


# ---------------------------------------------------------------- lifted C and disassembly
DEF = re.compile(r'^void ([fu]_[0-9a-f]+)\(Ctx \*c(?:, uint64_t rp)?\) \{')


LABEL = re.compile(r'^(L_[0-9a-f]+(?:_m)?|L_dispatch): ')
GOTO = re.compile(r'\bgoto (L_[0-9a-f]+(?:_m)?|L_dispatch);')
TAIL = re.compile(r'^(return;|goto L_|\{ jt = |\{ (f|u|imp|h)_\w+\(c[,)]|f_[0-9a-f]+\(c\); return;|default: )')


def reachable_lines(body):
    """indices of the statements of a lifted function that can run: from the first statement, falling through unless the
    statement always leaves (return, goto, tail call, br), along every goto; `goto L_dispatch` reaches every case.
    A dead call site (e.g. the fall-through call behind padding after a `ret`) is no call the compiler had to keep."""
    labels, dispatch, info = {}, [], []
    in_switch = False
    for i, line in enumerate(body):
        m = LABEL.match(line)
        core = line[m.end():] if m else line.strip()
        if m:
            labels[m.group(1)] = i
            if m.group(1) == 'L_dispatch':
                in_switch = True
        elif in_switch and core.startswith('case '):
            dispatch.append(i)
        if core.startswith('case 0x') and ': ' in core:
            core = core[core.index(': ') + 2:]
        if core.startswith('{ FL_OUT(); ') and core.endswith(' FL_IN(); }'):
            core = core[len('{ FL_OUT(); '):-len(' FL_IN(); }')]
        info.append((core, GOTO.findall(line)))
    todo = [0]
    if body and body[0].startswith('  if (rp) switch'):         # resume entries (recomp_eh): every case, and rp = 0 behind the switch
        k = next(i for i, line in enumerate(body) if line.startswith('  default:'))
        todo = list(range(k + 2))
    seen = set()
    while todo:
        i = todo.pop()
        if i in seen or i >= len(body):
            continue
        seen.add(i)
        core, gotos = info[i]
        for g in gotos:
            if g == 'L_dispatch':
                todo.extend(dispatch)
            elif g in labels:
                todo.append(labels[g])
        if not TAIL.match(core):
            todo.append(i + 1)
    return seen


def c_calls(path, targets):
    """{target: {caller function: (reachable `f_<target>(c)` sites, dead ones)}}"""
    want = {'f_%x' % t: t for t in targets}
    pat = re.compile(r'\b(f_[0-9a-f]+)\(c\)')
    live = collections.defaultdict(collections.Counter)
    dead = collections.defaultdict(collections.Counter)

    tset = set(targets)
    cpat = re.compile(r'0x([0-9a-f]+)ULL')
    src_const = collections.defaultdict(set)      # target -> functions whose C holds the address as a constant (x30 = F etc.)

    def done(name, body):
        for line in body:
            for k in cpat.findall(line):
                if int(k, 16) in tset:
                    src_const[int(k, 16)].add(name)
        hits = [(i, callee) for i, line in enumerate(body) for callee in pat.findall(line) if callee in want]
        if not hits:
            return
        ok = reachable_lines(body)
        for i, callee in hits:
            (live if i in ok else dead)[want[callee]][name] += 1
    cur, body = None, []
    with open(path, encoding='latin-1') as f:
        for line in f:
            m = DEF.match(line)
            if m:
                if line.rstrip().endswith('}'):                   # one-line wrapper: void f_x(Ctx *c) { u_x(c, 0); }
                    done(m.group(1), [line[line.index('{') + 1:].strip()])
                    continue
                cur, body = m.group(1), []
                continue
            if cur is None:
                continue
            if line.startswith('}'):
                done(cur, body)
                cur = None
                continue
            line = line.rstrip('\n')
            if line.startswith('  FlagsL fl_;') or line.startswith('  c->fn = ') or line.startswith('  uint64_t jt = 0;'):
                continue
            body.append(line)
    out = collections.defaultdict(dict)
    for t in set(live) | set(dead):
        for g in set(live[t]) | set(dead[t]):
            out[t][g] = (live[t].get(g, 0), dead[t].get(g, 0))
    return out, src_const


FN_HDR = re.compile(r'^[0-9a-f]{16} <([^>]+)>:')
INSN = re.compile(r'^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$')
RELOC = re.compile(r'^\s+[0-9a-f]+:\s+IMAGE_REL_AMD64_\w+\s+(\S+)')


def disasm_refs(obj, targets):
    """({target: {function: branch references}}, {target: {function: movabs of the address}})"""
    names = {'f_%x' % t: t for t in targets}
    consts = {'$0x%x' % t: t for t in targets}
    r = subprocess.run([OBJDUMP, '-d', '-r', '--no-show-raw-insn', obj], capture_output=True, text=True, errors='replace')
    refs = collections.defaultdict(collections.Counter)
    marks = collections.defaultdict(collections.Counter)
    cur, pending = None, None                 # pending = [function, target of the last branch instruction]

    def flush():
        if pending and pending[1] in names:
            refs[names[pending[1]]][pending[0]] += 1
    for line in r.stdout.split('\n'):
        m = FN_HDR.match(line)
        if m:
            flush()
            pending, cur = None, m.group(1)
            continue
        m = RELOC.match(line)
        if m:
            if pending is not None:
                pending[1] = m.group(1)       # the relocation is the real target (the symbolised one is the unrelocated field)
            continue
        m = INSN.match(line)
        if not m:
            continue
        flush()
        pending = None
        mn, ops = m.group(2), m.group(3)
        if mn.startswith(('call', 'jmp')) or (mn.startswith('j') and len(mn) <= 4):
            t = re.search(r'<([^>]+)>$', ops)
            pending = [cur, t.group(1) if t else None]      # `<f_x>` exactly = its entry; `<f_x+0x..>` = inside
        elif mn.startswith('movabs'):
            c = ops.split(',')[0].strip()
            if c in consts:
                marks[consts[c]][cur] += 1
    flush()
    return refs, marks


def fmt_caller(g, live, dead, compiled, const):
    """`f_g compiled/live` (+ dead sites, + the address constant found in g)"""
    return '%s %d/%d%s%s' % (g, compiled, live, ' +%d dead' % dead if dead else '', ' +const' if const else '')


def check(targets, gen, objdir, base=BASE):
    funcs = sorted(int(m, 16) for m in re.findall(r'^void f_([0-9a-f]+)\(Ctx \*c\);', open(os.path.join(gen, 'funcs.h')).read(), re.M))
    fset = set(funcs)
    by_bucket = collections.defaultdict(list)
    results = {}
    for t in targets:
        if t not in fset:
            results[t] = dict(verdict='NOT AN ENTRY', bucket=None, callers=[], entry='-')
            continue
        by_bucket[(t - base) // BUCKET].append(t)
    for b, ts in sorted(by_bucket.items()):
        src = os.path.join(gen, 'c_%04x.c' % b)
        obj = os.path.join(objdir, 'c_%04x.obj' % b)
        calls, src_const = c_calls(src, ts)
        refs, marks = disasm_refs(obj, ts)
        data, syms, names = coff_functions(obj)
        for t in ts:
            fn = 'f_%x' % t
            rows, verdict = [], 'OK'
            for g, (s, dead) in sorted(calls[t].items()):
                d = refs[t].get(g, 0)
                rows.append((g, s, dead, d, marks[t].get(g, 0)))
                if d < s:
                    verdict = 'INLINED'
            clones = [n for n in names if n.startswith(fn + '.')]
            if clones:
                verdict = 'CLONED' if verdict == 'OK' else verdict + '+CLONED'
            sus = [g for g, k in marks[t].items() if k and g not in (fn, 'u' + fn[1:]) and g not in src_const[t] and calls[t].get(g, (0, 0))[0] <= refs[t].get(g, 0)]
            if sus and verdict == 'OK':
                verdict = 'SUSPECT'
            if fn in syms:
                at, _ = syms[fn]
                n, why = hk_move(data, at)
                entry = 'movable (%d bytes)' % n if n else 'REFUSED: %s (%s)' % (why, ' '.join('%02x' % x for x in data[at:at + 6]))
            else:
                entry = 'no symbol %s in the object' % fn
                verdict = 'NO SYMBOL'
            results[t] = dict(verdict=verdict, bucket=b, callers=rows, entry=entry, clones=clones, suspect=sus)
    return results


def census_all(objdir):
    total = bad = 0
    why = collections.Counter()
    first = []
    for n in sorted(os.listdir(objdir)):
        if not (n.startswith('c_') and n.endswith('.obj')):
            continue
        data, syms, _ = coff_functions(os.path.join(objdir, n))
        for name, (at, _) in syms.items():
            if not name.startswith('f_'):
                continue
            total += 1
            k, w = hk_move(data, at)
            if not k:
                bad += 1
                why[w] += 1
                if len(first) < 10:
                    first.append('%s %s: %s (%s)' % (n, name, w, ' '.join('%02x' % x for x in data[at:at + 6])))
    return total, bad, why, first


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict(a[2:].split('=', 1) if '=' in a else (a[2:], '1') for a in sys.argv[1:] if a.startswith('--'))
    base = BASE
    if 'module' in opts:                        # a script module: gen_nro_eh_o2\<m>, obj_nro_eh_o2\<m>, its guest base
        import relift_sites
        _, b, m, span = relift_sites.module_info(opts['module'])
        base = int(b, 16)
        opts.setdefault('gen', os.path.join('gen_nro_eh_o2', m))
        opts.setdefault('obj', os.path.join('obj_nro_eh_o2', m))
    gen = os.path.join(HERE, opts.get('gen', 'gen_eh_o2'))
    objdir = os.path.join(HERE, opts.get('obj', 'obj_eh_o1'))
    targets, labels = [], {}
    if args:
        src = args[0]
        lines = open(src).read().split(chr(10)) if os.path.exists(src) else src.split(',')
        for line in lines:
            t = line.split('#', 1)
            f = t[0].split()
            if not f or (len(f) > 1 and f[0] not in ('hook', 'noinline', 'inline')):   # a site list: only the entry hooks
                continue
            a = int(f[-1], 16)
            if a < base:
                a += base
            if not base <= a < base + span:
                continue                        # another module's address
            targets.append(a)
            if len(t) > 1:
                labels[a] = t[1].strip()
    targets = sorted(set(targets))
    res = check(targets, gen, objdir, base) if targets else {}
    bad = []
    for t in targets:
        r = res[t]
        callers = ', '.join(fmt_caller(*c) for c in r['callers']) or 'none in its file'
        print('%x  %-10s c_%s  entry %s  callers in file (compiled / reachable lifted sites): %s%s  %s' % (
            t, r['verdict'], '%04x' % r['bucket'] if r['bucket'] is not None else '----', r['entry'], callers,
            ('  clones ' + ' '.join(r['clones'])) if r.get('clones') else '', labels.get(t, '')))
        if r['verdict'] != 'OK':
            bad.append(t)
    refused = [t for t in targets if res[t]['entry'].startswith('REFUSED')]
    print('%d functions: %d OK, %d not (inlined / cloned / suspect / no symbol), %d entries refused by the mover'
          % (len(targets), len(targets) - len(bad), len(bad), len(refused)))
    if 'emit' in opts:
        with open(opts['emit'], 'w') as f:
            for t in bad:
                if res[t]['verdict'] in ('NOT AN ENTRY',):
                    continue
                f.write('noinline 0x%x    # %s %s\n' % (t, res[t]['verdict'], labels.get(t, '')))
    if 'census-all' in opts:
        total, nbad, why, first = census_all(objdir)
        print('entry census of %s: %d functions, %d refused' % (os.path.basename(objdir), total, nbad))
        for w, n in why.most_common():
            print('  %6d  %s' % (n, w))
        for line in first:
            print('  e.g. ' + line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
