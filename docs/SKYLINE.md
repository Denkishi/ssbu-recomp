# Skyline plugins

The runtime can load Skyline plugins, the NRO modules that console mods are made of (ARCropolis, smashline, nro_hook and
the mods built on them, such as HewDraw Remix). Nothing of any plugin or mod is part of this repository: you install
them yourself in an SD card folder, as for a modded console, and the tools below lift them on your PC like the game's own
script modules. Without a plugin folder nothing of this runs and the game is exactly as before.

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
  for an imported function through a host-entry hook. Plugins that call "orig" get the previous layer. An inline hook at
  a function entry works the same way.
- **What compiled code has no place for** is not active: an inline hook or a function hook in the middle of a function,
  and a patch of the game's code (writes into data take effect). Every hook, inline hook and patch the plugins make is
  written to `skyline_sites.log` beside the exe, marked `active` or `NOT ACTIVE`, so you can see what a plugin did that
  has no effect.
- Plugins also need parts of the system the game never used: pthreads, thread keys, reader/writer locks
  (`os_threads.c`), file systems registered by plugins and writes to the SD card (`nnfs.c`), rtld module lists and
  `nn::ro::LookupSymbol` (smashline walks them), late binding of a plugin's imports to modules loaded after it
  (`nnro.c`), and smashline's exception landing in its script interpreter (`nnunwind.c`).
- `build_plugins.py` changes two things in plugin code, found by the shape of the code rather than by offsets of a
  particular plugin build: the emulator check of skyline-rs plugins (they compare the text address with the one Ryujinx
  or yuzu uses and take a code path of their own there; here main is at 0x7100000000, and the comparison is changed to
  hold for that address), and a resume entry for smashline's interpreter landing pad. It prints what it changed.
  `--patches <folder>` adds lists of your own (`<name>.txt`: `<offset> <original word> <new word>` per line, hex;
  `<name>.resume.txt`: one offset per line); the original word is checked against the plugin.

## What you need

A complete build (`python scripts/setup.py`, see [BUILDING.md](../BUILDING.md)) and an SD card folder laid out as for
the console:

```
sd\
  atmosphere\contents\01006A800016E000\romfs\skyline\plugins\   libarcropolis.nro, libsmashline_plugin.nro, libnro_hook.nro, ...
  ultimate\mods\                                                  mod folders (ARCropolis loads them; a mod's plugin.nro is lifted too)
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
   This writes `plugin_bin\<name>.dll` and `plugin_bin\plugins.txt`.
2. Play:
   ```
   python scripts/run.py --userdata <folder> --env SSBU_SD=<sd folder> --env SSBU_SHOW=1 --env SSBU_VOLUME=100      (from the repository root)
   ```
   (`SSBU_SHOW` and `SSBU_VOLUME` because only an exe named `ssbu_native.exe` is shown and audible by default.) The log
   shows `[skyline] N plugins loaded` and the plugins' own output as `[plugin ...]` lines.

Switches: `SSBU_SD=<folder>` (default `<exe>\..\..\sd`), `SSBU_SKYLINE=0` (plugins off even with the folder present),
`SSBU_PLUGIN_BIN=<folder>` (default `plugin_bin`), `SSBU_FSLOG=1` (file-system calls of the plugins). At exit the log
lists how often each hook was called.

## Example: HewDraw Remix

HewDraw Remix (HDR) is a set of Skyline plugins plus files that ARCropolis loads. Installed in the SD folder as its
release describes (its `hdr` folder under `ultimate\mods`, ARCropolis, smashline, nro_hook, stage_alts, stage_config and
the HDR launcher in the plugins folder), its plugins load with the steps above and their function hooks run. HDR and
ARCropolis also change the game's code in places a lifted build cannot follow (inline hooks and patches inside
functions): those show up as `NOT ACTIVE` in `skyline_sites.log`, and without them HDR does not play as HDR (ARCropolis's
file system, for one, stays uninitialised).

## Known problems

- Two of smashline's hooks are in the SDK (`NOT ACTIVE`): its unwinder hook, whose effect `nnunwind.c` provides instead,
  and an inline hook next to it.
- A plugin build whose emulator check has another shape keeps taking its console path; give that plugin a patch list of
  your own (`--patches`).
