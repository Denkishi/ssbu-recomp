"""Skyline plugin NROs for build_plugins.py: where they are, their names, and their stable guest addresses.

    find_plugins(sd)            the plugin NROs of an SD card layout (read only)
    module_name(path)           sky_<name>: the module path record at the start of .rodata ({u32 0, i32 len, path})
    load_map / assign(...)      plugin_map.json: name -> base, slot size; bases never move unless the image outgrows
                                its slot (then it goes to the end and assign() says so)
    registry_line(...)          one line of plugins.txt (NroInfo of nro_registry.c: name base image_size file_size build_id)
    exports(path)               the module's defined dynamic symbols with their offsets
    auto_patches(path)          instruction patches and extra resume entries found by the shape of the code (no lists of
                                offsets of particular plugin builds: those would be part of the plugins)
"""
import glob
import json
import os
import struct

import nro_info

PLUGIN_BASE = 0x7280000000      # game modules end near 0x7210a00000; the module area of the loader ends at 0x7300000000
PLUGIN_END = 0x7300000000
SLOT_ALIGN = 0x200000           # 2 MB
PLUGINS_DIR = os.path.join('atmosphere', 'contents', '01006A800016E000', 'romfs', 'skyline', 'plugins')


def find_plugins(sd):
    found = sorted(glob.glob(os.path.join(sd, PLUGINS_DIR, '*.nro')))
    found += sorted(glob.glob(os.path.join(sd, 'ultimate', 'mods', '*', 'plugin.nro')))
    return found


def module_name(path):
    d = open(path, 'rb').read()
    ro = struct.unpack_from('<I', d, 0x28)[0]
    zero, n = struct.unpack_from('<Ii', d, ro)
    if zero != 0 or not 0 < n < 256:
        raise ValueError('%s: no module path record at the start of .rodata' % path)
    raw = d[ro + 8:ro + 8 + n].decode('latin-1')
    raw = raw.replace('\\', '/').split('/')[-1]
    if raw.lower().endswith('.nro'):
        raw = raw[:-4]
    return 'sky_' + ''.join(ch if ch.isalnum() else '_' for ch in raw)


def sizes(path):
    """(image_size, file_size, build id hex) as nro_registry.c's NroInfo (nro_info.Nro, the game modules' definitions)."""
    n = nro_info.Nro(path)
    return n.image_size, len(n.d), n.build_id[:40]


def load_map(path):
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    return {}


def save_map(path, m):
    with open(path, 'w') as f:
        json.dump(m, f, indent=1, sort_keys=True)


def assign(m, name, image_size, source):
    """Give `name` a base in map m (changed in place). Returns a note for the log, or ''."""
    slot = (image_size + SLOT_ALIGN - 1) // SLOT_ALIGN * SLOT_ALIGN
    end = max([PLUGIN_BASE] + [int(e['base'], 16) + e['slot'] for e in m.values()])
    note = ''
    e = m.get(name)
    if e and image_size <= e['slot']:
        e.update(image_size=image_size, source=source)
        return ''
    if e:
        note = '%s: image %#x outgrew its slot %#x at %s: moved to %#x' % (name, image_size, e['slot'], e['base'], end)
    else:
        note = '%s: new plugin at %#x (slot %#x)' % (name, end, slot)
    if end + slot > PLUGIN_END:
        raise SystemExit('plugin area full: %s needs %#x at %#x' % (name, slot, end))
    m[name] = dict(base='%#x' % end, slot=slot, image_size=image_size, source=source)
    return note


def registry_line(name, base, path):
    image_size, file_size, bid = sizes(path)
    return '%s %#x %#x %#x %s %s' % (name, base, image_size, file_size, bid, os.path.abspath(path))


def exports(path):
    """[(offset, kind, size, name)] of the symbols the module defines (FUNC / OBJECT / ...), sorted by offset."""
    n = nro_info.Nro(path)
    kinds = {0: 'NOTYPE', 1: 'OBJECT', 2: 'FUNC', 3: 'SECTION', 4: 'FILE', 6: 'TLS'}
    out = [(value, kinds.get(t, str(t)), size, name) for name, t, bind, shndx, value, size in n.syms if shndx and name]
    return sorted(out)


# ---------------------------------------------------------------------------------------------------- automatic patches
# Two things this port needs changed in plugin code, both found by the shape of the code, so that no offsets of particular
# plugin builds are kept here. build_plugins.py passes the result to recomp_nro.py (RECOMP_PATCHES / RECOMP_EXTRA_RESUME);
# lists of your own (build_plugins.py --patches) are added to them.
#
# 1. The emulator check. skyline-rs plugins compare getRegionAddress(Text) with the text bases of Ryujinx and yuzu
#    (0x8004000, 0x8504000) and take a code path of their own for those emulators. This port is treated like them: main is
#    at 0x7100000000 here, and the comparison is changed to hold for that address. The constant is built as
#        movz wN, #0x4000 ; [one or two other instructions] ; movk wN, #0x0800 or #0x0850, lsl #16
#    which becomes  movz xN, #0x71, lsl #32 ; ... ; nop  (the comparison then holds here).
# 2. smashline's interpreter stub (smashline src/interpreter.asm, the same shape that runtime/skyline.c looks for): its
#    landing pad is a nop that a custom personality resumes at + 4, which .eh_frame does not describe. That address gets a
#    resume entry like a landing pad.
EMU_MOVZ = 0x52880000           # movz wN, #0x4000
EMU_MOVK = (0x72a10000, 0x72a10a00)     # movk wN, #0x0800 / #0x0850, lsl #16
MAIN_MOVZ = 0xd2c00e20          # movz xN, #0x71, lsl #32
NOP = 0xd503201f
SMASHLINE_STUB = (0xa9bf7bfd, 0xa9bf53f3, 0xf81f0ff5, 0xaa0003f3, 0xaa0103f4, 0xaa0203f5)


def auto_patches(path):
    """([(module offset, original word, new word, note)], [resume module offset]) for the plugin at path."""
    n = nro_info.Nro(path)
    count = n.text_size // 4
    w = struct.unpack_from('<%dI' % count, n.d, n.text_off)
    patches, resume = [], []
    for i in range(count):
        x = w[i]
        if (x & 0xffffffe0) == EMU_MOVZ:
            r = x & 31
            for k in range(i + 1, min(i + 4, count)):
                if (w[k] & 0xffffffe0) in EMU_MOVK and (w[k] & 31) == r:
                    base = 0x8004000 if (w[k] & 0xffffffe0) == EMU_MOVK[0] else 0x8504000
                    note = 'emulator check: compares with %#x' % base
                    patches.append((n.text_off + 4 * i, x, MAIN_MOVZ | r, note + ', here with 0x7100000000'))
                    patches.append((n.text_off + 4 * k, w[k], NOP, note))
                    break
        if w[i:i + 6] == SMASHLINE_STUB and i + 7 < count:
            adr = w[i + 6]
            if (adr & 0x9f00001f) != 0x10000000:                    # adr x0, <landing pad>
                continue
            imm = ((adr >> 5) & 0x7ffff) << 2 | (adr >> 29) & 3
            if imm & (1 << 20):
                imm -= 1 << 21
            lp = 4 * (i + 6) + imm
            if 0 <= lp < 4 * count and w[lp // 4] == NOP:
                resume.append(n.text_off + lp + 4)
    return patches, resume
