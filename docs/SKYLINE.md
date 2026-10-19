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
- **Everything else needs the game re-lifted.** Code that has been compiled ahead of time cannot be patched by writing
  words into it, and an inline hook in the middle of a function has no place to go. So every hook, inline hook and patch
  the plugins make is written to `skyline_sites.log` (next to the exe) in a site-list format, and
  `tools/lifter/relift_sites.py` re-lifts only the affected bucket files of the game with those sites compiled in:
  patched words lifted as the new instruction, `sky_site_inline(c, addr)` before an inline-hooked instruction,
  `sky_site_midhook` plus a new lifted entry for a hook in the middle of a function. The result is a second game DLL
  (`ssbu_game_sky.dll`) and, for a script module that plugins patch (smashline patches the fighters' common module),
  `nro_bin_sky\<module>.dll`. The runtime reads which sites a DLL has compiled in (`sky_site_table`) and reports a hook
  that has no site as `NOT ACTIVE`. Without a re-lift these sites simply stay `NOT ACTIVE`; writes into data take effect
  either way.
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
   the changed lines are checked against what the sites predict. Output: `ssbu_game_sky.dll`, `nro_bin_sky\`.
   `python relift_sites.py check skyline_sites.log` only classifies the sites.
4. Link an exe against the re-lifted DLL:
   ```
   set RECOMP_DLL=ssbu_game_sky.dll
   set RECOMP_EXE=ssbu_sky.exe
   python build.py link
   ```
   `build.py` relinks a game DLL whose shim (`game_shim_eh.c`) is newer than it, from the base objects: after the
   sources were staged again with a changed shim, repeat step 3 before this one.
5. Play:
   ```
   python scripts/run.py --userdata <folder> --exe ssbu_sky.exe --env SSBU_SD=<sd folder> --env SSBU_SHOW=1 --env SSBU_VOLUME=100
   ```
   (`SSBU_SHOW` and `SSBU_VOLUME` because only an exe named `ssbu_native.exe` is shown and audible by default.) The log
   shows `[skyline] ssbu_game_sky.dll has N plugin sites compiled in` and `lua2cpp_common has N plugin sites compiled in`.
   If a later session logs new `NOT ACTIVE` sites in `skyline_sites.log`, repeat steps 3 and 4 with it.

The site list holds instruction words of the game (each patch line's original word): keep it on your PC, like the build.

Switches: `SSBU_SD=<folder>` (default `<exe>\..\..\sd`), `SSBU_SKYLINE=0` (plugins off even with the folder present),
`SSBU_PLUGIN_BIN=<folder>` (default `plugin_bin`), `SSBU_FSLOG=1` (file-system calls of the plugins). At exit the log
lists how often each hook was called.

## Example: HewDraw Remix

HewDraw Remix (HDR) is a set of Skyline plugins plus files that ARCropolis loads. Installed in the SD folder as its
release describes (its `hdr` folder under `ultimate\mods`, ARCropolis, smashline, nro_hook, stage_alts, stage_config and
the HDR launcher in the plugins folder), it loads with the steps above. HDR changes the game's code in places a lifted
build cannot follow without a re-lift (inline hooks and patches inside functions, and smashline's changes to the common
fighter module): steps 2 to 5 compile those in.

## Known problems

- Two of smashline's hooks are in the SDK (`NOT ACTIVE`): its unwinder hook, whose effect `nnunwind.c` provides instead,
  and an inline hook next to it.
- The site log only knows what the plugins hooked in the runs so far. A plugin that hooks something new later shows
  `NOT ACTIVE` in the log, and needs another re-lift + link.
- A plugin build whose emulator check has another shape keeps taking its console path; give that plugin a patch list of
  your own (`--patches`).
