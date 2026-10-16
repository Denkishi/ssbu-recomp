"""Make the dump folder that scripts/setup.py --dump expects, from your own title packages.

    python dump_folder.py <base.nsp|base.xci> <update.nsp|update.xci> <dump folder> [--exefs-only] [--firmware <folder>]

Writes <dump>/exefs/ (the ExeFS of the update's Program NCA: main, sdk, rtld, subsdk0, main.npdm) and <dump>/romfs/
(the base RomFS with the update applied, romfs_extract.py). With --firmware, the system fonts of a firmware dump go to
<dump>/sysdata/fonts (fw_fonts.py). The base game may be a cartridge image (.xci) or an .nsp; so may the update. Keys
come from ~/.switch/prod.keys (and pycryptodome is needed), as for the other tools here. Nothing is written anywhere else.
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import romfs_extract as rx  # noqa: E402


def extract_exefs(nca, out_dir):
    """The PartitionFs section (fs type 1) of a Program NCA is the ExeFS; its PFS0 starts at the hash table's data
    offset (fs header +0x40), as nca_exefs.py reads it."""
    sec = nca.section(1)
    pfs = struct.unpack('<Q', sec['fs'][0x40:0x48])[0]
    head = nca.read(sec, pfs, 16)
    assert head[:4] == b'PFS0', 'no ExeFS in %s' % nca.name
    count, strings_size = struct.unpack('<II', head[4:12])
    table = nca.read(sec, pfs + 16, 24 * count + strings_size)
    strings = table[24 * count:]
    data = pfs + 16 + 24 * count + strings_size
    os.makedirs(out_dir, exist_ok=True)
    for i in range(count):
        off, size, name_off, _ = struct.unpack('<QQII', table[i * 24:i * 24 + 24])
        name = strings[name_off:strings.index(b'\0', name_off)].decode()
        with open(os.path.join(out_dir, name), 'wb') as f:
            f.write(nca.read(sec, data + off, size))
        print('  exefs %10d  %s' % (size, name))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 3:
        print(__doc__)
        return 1
    base_path, update_path, out = args[:3]
    firmware = sys.argv[sys.argv.index('--firmware') + 1] if '--firmware' in sys.argv else None
    base, update = rx.Nca(rx.open_package(base_path)), rx.Nca(rx.open_package(update_path))
    for label, nca in (('base', base), ('update', update)):
        print('%-6s %s  program NCA %s, title %016x' % (label, type(nca.nsp).__name__.upper(), nca.name,
                                                       struct.unpack('<Q', nca.header[0x210:0x218])[0]))
    extract_exefs(update, os.path.join(out, 'exefs'))
    if '--exefs-only' not in sys.argv:
        rx.extract(base, update, os.path.join(out, 'romfs'))
    if firmware:
        subprocess.run([sys.executable, os.path.join(HERE, 'fw_fonts.py'), firmware, os.path.join(out, 'sysdata', 'fonts')], check=True)
    print('dump folder ready: python scripts/setup.py --dump "%s"' % os.path.abspath(out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
