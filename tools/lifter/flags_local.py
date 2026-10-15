"""The lifted main with the condition flags in a local (runtime\\flags_local.h).

    python flags_local.py [src_gen] [dst_gen]        default gen_eh_o2 -> gen_fl

Rewrites every generated file of src_gen into dst_gen (only files whose result changed are written, so the compile stays
incremental) and copies the others (funcs.h, table.c, imports.c, ...). In each lifted function: `FlagsL fl_; FL_IN();` after
the opening line; the flag helpers of runtime.h (adds32/64, logic32/64, set_nzcv, cond) and direct c->N/Z/C/V uses go to
fl_; a statement that can leave the function (a call of a lifted function or of the host, a return, a trap) is wrapped
in FL_OUT() ... FL_IN(). Compile the result with RECOMP_GEN=dst_gen (build_opt.py / build.py)."""
import os
import re
import shutil
import sys

LEAVE = re.compile(r'\b(f_[0-9a-f]+|u_[0-9a-f]+|call_indirect|host_import|guest_svc|guest_unimpl|guest_trap|nro_catch_call|stx_diag)\s*\(|\breturn\b')
HELPERS = [(re.compile(r'\b%s\(c,' % h), '%sf(&fl_,' % h) for h in ('adds64', 'adds32', 'logic64', 'logic32', 'set_nzcv', 'cond')]
FLAG_FIELD = re.compile(r'\bc->([NZCV])\b')
FN_OPEN = re.compile(r'^void [uf]_[0-9a-f]+\(Ctx \*c(, uint64_t rp)?\) \{$')
STMT = re.compile(r'^(L_[0-9a-f]+: |  case 0x[0-9a-f]+ULL: |  default: |  )(.*)$')


def transform(text):
    out = []
    in_fn = False
    for line in text.split('\n'):
        if line == '#include "funcs.h"':
            out.append(line)
            out.append('#include "flags_local.h"')
            continue
        if FN_OPEN.match(line):
            out.append(line)
            out.append('  FlagsL fl_; FL_IN();')
            in_fn = True
            continue
        if line == '}':
            in_fn = False
        if not in_fn:
            out.append(line)
            continue
        body = line
        for pat, rep in HELPERS:
            body = pat.sub(rep, body)
        body = FLAG_FIELD.sub(r'fl_.\1', body)
        m = STMT.match(body)
        if m and LEAVE.search(m.group(2)) and not m.group(2).startswith(('c->fn', 'uint64_t jt', 'if (rp) switch')):
            stmt, tail = m.group(2), ''
            if stmt.endswith('; }') and stmt.count('{') < stmt.count('}'):      # the closing brace of a switch (L_dispatch, resume entries)
                stmt, tail = stmt[:-2], ' }'
            body = m.group(1) + '{ FL_OUT(); ' + stmt + ' FL_IN(); }' + tail
        out.append(body)
    return '\n'.join(out)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'gen_eh_o2'
    dst = sys.argv[2] if len(sys.argv) > 2 else 'gen_fl'
    os.makedirs(dst, exist_ok=True)
    changed = same = 0
    for name in sorted(os.listdir(src)):
        s, d = os.path.join(src, name), os.path.join(dst, name)
        if not os.path.isfile(s):
            continue
        if re.match(r'^c_[0-9a-f]+\.c$', name):
            with open(s, encoding='latin-1') as f:
                text = transform(f.read())
            if os.path.exists(d):
                with open(d, encoding='latin-1') as f:
                    if f.read() == text:
                        same += 1
                        continue
            with open(d, 'w', encoding='latin-1', newline='\n') as f:
                f.write(text)
            changed += 1
        elif not os.path.exists(d) or os.path.getmtime(d) < os.path.getmtime(s):
            shutil.copy2(s, d)
    print('flags_local: %d files written, %d unchanged -> %s' % (changed, same, dst))


if __name__ == '__main__':
    main()
