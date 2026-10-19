"""Skyline plugin NROs for build_plugins.py: where they are, their names, and their stable guest addresses.

    find_plugins(sd)            the plugin NROs of an SD card layout (read only)
    module_name(path)           sky_<name>: the module path record at the start of .rodata ({u32 0, i32 len, path})
    load_map / assign(...)      plugin_map.json: name -> base, slot size; bases never move unless the image outgrows
                                its slot (then it goes to the end and assign() says so)
    registry_line(...)          one line of plugins.txt (NroInfo of nro_registry.c: name base image_size file_size build_id)
    exports(path)               the module's defined dynamic symbols with their offsets
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
