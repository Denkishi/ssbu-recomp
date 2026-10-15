"""python gen_imports.py [main.elf] [out]  -> imports_all.txt: the names main imports (undefined dynamic symbols), in symbol-table order.
Read by sdklift.py (which SDK functions the game itself imports) and gen_sdkdata.py. Default input is the converted
dump (..\\binaries\\13.0.4\\main.elf relative to this folder), default output imports_all.txt next to this file."""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
elf = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get('RECOMP_BIN') or os.path.join(HERE, '..', 'binaries', '13.0.4'), 'main.elf')
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'imports_all.txt')
sys.path.insert(0, HERE)
import recomp                                  # noqa: E402  (same constants and image as the lifter)

img = recomp.IMG
names = []                                     # symbol table order
for i in range(1, (recomp.STRTAB - recomp.SYMTAB) // 24):
    no, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', img, recomp.FILE_OFF + recomp.SYMTAB + i * 24)
    if shndx == 0 and no:
        p = recomp.FILE_OFF + recomp.STRTAB + no
        names.append(img[p:img.index(b'\0', p)].decode('latin-1'))
with open(out, 'w', newline='\n') as f:
    f.writelines(n + '\n' for n in names)
print('%d imports -> %s' % (len(names), out))
