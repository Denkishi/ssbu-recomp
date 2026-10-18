# Skyline plugins (HewDraw Remix, ARCropolis, smashline)

The runtime can load Skyline plugins, the NRO modules that console mods such as HewDraw Remix (HDR) are made of. Nothing
of HDR or of the other plugins is part of this repository: you install them yourself, as for a modded console, and the
tools below lift them on your PC like the game's own script modules. Without a plugin folder nothing of this runs and
the game is exactly as before.

State: tested on one PC (Ryzen 7 3700X, RTX 3080, Windows 10) with HDR, ARCropolis, smashline, nro_hook, stage_alts,
stage_config and the HDR launcher, through many matches of play. Known problems are listed at the end.

## How it works

On the console, Skyline hooks the game's first `nn::fs::MountRom`, loads every NRO under
`sd:/atmosphere/contents/01006A800016E000/romfs/skyline/plugins` and calls each plugin's `main`. The plugins then
change the game while it runs: function hooks (a branch written at a function), inline hooks (a callback that sees all
registers before one instruction), and patches (instruction words written into the game's code).

Here (`runtime/skyline.c`):

- **Plugins are lifted ahead of time** (`tools/nro/build_plugins.py`) into `plugin_bin\<name>.dll` with a fixed guest
  address each, and loaded by the same `nn::ro` loader as the game's modules. Skyline's exports (`A64HookFunction`,
  `A64InlineHook`, `sky_memcpy`, `getRegionAddress`, ...) are host functions.
- **A hook at a function entry** goes through the existing hook table (the lifted function's entry is redirected), or
  for an imported function through a host-entry hook. Plugins that call "orig" get the previous layer.
- **Everything else needs the game re-lifted.** Code that has been compiled ahead of time cannot be patched by writing
  words into it, and an inline hook in the middle of a function has no place to go. So every hook, inline hook and patch
  the plugins make is written to `skyline_sites.log` (next to the exe) in a site-list format, and
  `tools/lifter/relift_sites.py` re-lifts only the affected bucket files of the game with those sites compiled in:
  patched words lifted as the new instruction, `sky_site_inline(c, addr)` before an inline-hooked instruction,
  `sky_site_midhook` plus a new lifted entry for a hook in the middle of a function. The result is a second game DLL
  (`ssbu_game_hdr.dll`) and, for the fighters' common module that smashline patches, `nro_bin_hdr\lua2cpp_common.dll`.
  The runtime reads which sites a DLL has compiled in (`hdr_site_table`) and reports a hook that has no site as
  `NOT ACTIVE`.
- Plugins also need parts of the system the game never used: pthreads, thread keys, reader/writer locks
  (`os_threads.c`), file systems registered by plugins and writes to the SD card (`nnfs.c`), rtld module lists and
  `nn::ro::LookupSymbol` (smashline walks them), late binding of a plugin's imports to modules loaded after it
  (`nnro.c`), and smashline's exception landing in its script interpreter (`nnunwind.c`).

## What you need

A complete build (`python scripts/setup.py`, see [BUILDING.md](../BUILDING.md)) and an SD card folder laid out as for
the console:

```
sd\
  atmosphere\contents\01006A800016E000\romfs\skyline\plugins\   libarcropolis.nro, libsmashline_plugin.nro, libnro_hook.nro, ...
  ultimate\mods\                                                  HDR's folders (ARCropolis loads them, and hdr\plugin.nro)
```

Skyline itself (`exefs\subsdk9`, `main.npdm`) is not used. Keep the folder outside the repository, or at `sd\` beside
`build\` (that path is ignored by git).

## Steps

All commands in `build\work` unless said otherwise; `RECOMP_CLANG` should name the same clang that `setup.py` used.

1. Lift the plugins:
   ```
   cd ro_study
   python build_plugins.py --sd <sd folder>
   ```
   This writes `plugin_bin\<name>.dll` and `plugin_bin\plugins.txt`. A plugin build that the patch lists in
   `ro_study\plugin_patches` do not match is refused (the lists are checked against the file); see "Versions" below.
2. Record the sites: run the normal exe once with the plugins and play into a match (smashline patches the common
   fighter module when the first fight loads it):
   ```
   python scripts/run.py --userdata <folder> --env SSBU_SD=<sd folder>        (from the repository root)
   ```
   `build\work\skyline_sites.log` now lists every hook, inline hook and patch. Many of them are `NOT ACTIVE` in this run,
   which is expected.
3. Re-lift the game and the common module with those sites:
   ```
   python relift_sites.py build skyline_sites.log
   python relift_sites.py build skyline_sites.log --module=common
   ```
   Each bucket file is first lifted without the sites and compared with the base set byte for byte, then with the sites;
   the changed lines are checked against what the sites predict. Output: `ssbu_game_hdr.dll`, `nro_bin_hdr\`.
   `python relift_sites.py check skyline_sites.log` only classifies the sites.
4. Link an exe against the re-lifted DLL:
   ```
   set RECOMP_DLL=ssbu_game_hdr.dll
   set RECOMP_EXE=ssbu_hdr.exe
   python build.py link
   ```
5. Play:
   ```
   python scripts/run.py --userdata <folder> --exe ssbu_hdr.exe --env SSBU_SD=<sd folder> --env SSBU_SHOW=1 --env SSBU_VOLUME=100
   ```
   (`SSBU_SHOW` and `SSBU_VOLUME` because only an exe named `ssbu_native.exe` is shown and audible by default.) The log
   shows `[skyline] ssbu_game_hdr.dll has N plugin sites compiled in` and `lua2cpp_common has N plugin sites compiled in`.
   If a later session logs new `NOT ACTIVE` sites in `skyline_sites.log`, repeat steps 3 and 4 with it.

Switches: `SSBU_SD=<folder>` (default `<exe>\..\..\sd`), `SSBU_SKYLINE=0` (plugins off even with the folder present),
`SSBU_PLUGIN_BIN=<folder>` (default `plugin_bin`), `SSBU_FSLOG=1` (file-system calls of the plugins). At exit the log
lists how often each hook was called.

## Versions

`tools/nro/plugin_patches/*.txt` patch the plugins' Ryujinx / yuzu check (they compare the text address with the one those
emulators use; here main is at 0x7100000000) and add smashline's interpreter landing pad as a resume entry. They are
offsets into the plugin builds that were tested and are refused for any other build: a newer HDR or ARCropolis needs new
offsets (the instructions to look for are named in the files).

## Known problems

- Some stages flash or lose parts of their background or water now and then (Small Battlefield and Midgar water, Yoshi's
  Story, HDR's Ganon's Tower); seen at `resolution_scale` 2.0, not at 1.0.
- Fountain of Dreams runs slowly.
- Two of smashline's hooks are in the SDK (`NOT ACTIVE`): its unwinder hook, whose effect `nnunwind.c` provides instead,
  and an inline hook next to it.
