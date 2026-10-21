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

A complete build (`Install SSBU.bat`, or `python scripts/setup.py`: [BUILDING.md](../BUILDING.md)) and an SD card folder
laid out as for the console, `sd\` beside `build\` (that path is ignored by git; `SSBU_SD=<folder>` names another one):

```
sd\
  atmosphere\contents\01006A800016E000\romfs\skyline\plugins\   libarcropolis.nro, libsmashline_plugin.nro, libnro_hook.nro, ...
  ultimate\mods\                                                  mod folders (ARCropolis loads them; a mod's plugin.nro is lifted too)
```

Skyline itself (`exefs\subsdk9`, `main.npdm`) is not used.

## Steps

Unpack the mod's release into `sd\`, as you would onto the SD card of a console, and double-click `Play SSBU.bat`. The
first start prepares the mods, which takes a few minutes (`scripts/mods.py`, called by `scripts/run.py --play`), and the
game starts when that is done:

1. every plugin is lifted into `build\work\plugin_bin` (`ro_study\build_plugins.py`), when it is new or changed;
2. the game boots once without a window, with the normal game DLL, until its common fighter module is loaded (the input
   sequence `@module:lua2cpp_common 60 @exit`), and the sites the plugins hooked or patched in that boot
   (`skyline_sites.log`, written by the runtime) are kept as `plugin_bin\sites.log`;
3. the bucket files those sites touch are re-lifted with the sites compiled in (`relift_sites.py`: `ssbu_game_sky.dll`,
   and `nro_bin_sky\<module>.dll` for a script module the plugins hook).

The exe loads `ssbu_game_sky.dll` instead of `ssbu_game_eh_o1.dll` when the plugin folder exists and `plugin_bin\plugins.txt`
lists lifted plugins (`runtime/gamedll.c` finds the game DLL by name at start; no import library), and `nro_bin_sky\` for a
module re-lifted there (`nnro.c`). Without the folder, or with `SSBU_SKYLINE=0`, the same exe runs the normal game. A re-lifted
DLL older than `ssbu_game_eh_o1.dll` (the installer's Resume or an update relinked the game DLL) is made again at the next
start. The log shows `game DLL: ssbu_game_sky.dll (...)`,
`[skyline] ssbu_game_sky.dll has N plugin sites compiled in` and `lua2cpp_common has N plugin sites compiled in`.

After every game with plugins started by `run.py --play`, that run's site list is kept as `plugin_bin\sites_last.log`: a
plugin that hooks something new in a mode played for the first time is noticed at the next start, which adds those sites
and re-lifts again (if they cannot be compiled in, the previous preparation stays and the game says so). `plugin_bin\mods_state.json` records what was prepared. `python scripts/mods.py --check` says what the next
start would do, `--force` prepares everything again. When a step fails, the game starts without the plugins and says
so; `build\logs\mods.log` has the details (and `mods_record.log` the recording boot's log).

A build made with the prebuilt runtime package (`Install SSBU.bat` without the MSVC Build Tools) links the plugin DLLs
and the re-lifted DLLs with LLVM alone, as its own game DLL (`RECOMP_NOCRT`: lld-link and `nocrt\`, `build.py link_dll`).

### By hand

The same steps one by one, all commands in `build\work`, with `RECOMP_CLANG` naming the build's clang (and for a build
made with a runtime package `RECOMP_NOCRT=1` and `RECOMP_BIN=<build>\binaries\<game version>`, as `mods.py` sets them):

1. Lift the plugins: `cd ro_study` and `python build_plugins.py --sd <sd folder>`: `plugin_bin\<name>.dll` and
   `plugin_bin\plugins.txt`.
2. Record the sites: run the game once with the plugins and the normal DLLs, and play into a match (smashline patches
   the common fighter module when the first fight loads it):
   ```
   python scripts/run.py --userdata <folder> --env SSBU_SD=<sd folder> --env SSBU_SKY_RELIFT=0      (from the repository root)
   ```
   `build\work\skyline_sites.log` now lists every hook, inline hook and patch, many of them `NOT ACTIVE` in this run,
   which is expected. Copy it: the runtime rewrites it at every start.
3. Re-lift the game and the common module with those sites:
   ```
   python relift_sites.py build <copy of the site list>
   python relift_sites.py build <copy of the site list> --module=common
   ```
   Each bucket file is first lifted without the sites and compared with the base set byte for byte, then with the sites;
   the changed lines are checked against what the sites predict. Output: `ssbu_game_sky.dll`, `nro_bin_sky\`.
   `python relift_sites.py check <site file>` only classifies the sites.
4. Play: `python scripts/run.py --userdata <folder> --env SSBU_SD=<sd folder>`. If a later session logs new
   `NOT ACTIVE` sites in `skyline_sites.log`, repeat step 3 with a copy of it (without `--play` nothing keeps the list for
   you). After the installer's Resume or an update relinked `ssbu_game_eh_o1.dll`, repeat step 3 too: the exe refuses a
   re-lifted DLL built for another game-data interface.

The site list holds instruction words of the game (each patch line's original word): keep it on your PC, like the build.

Switches: `SSBU_SD=<folder>` (default `<exe>\..\..\sd`), `SSBU_SKYLINE=0` (plugins off even with the folder present),
`SSBU_PLUGIN_BIN=<folder>` (default `plugin_bin`), `SSBU_GAME_DLL=<name>` (this game DLL, whatever the folder holds),
`SSBU_SKY_RELIFT=0` (the normal DLLs although plugins are present: how the sites are recorded), `SSBU_FSLOG=1`
(file-system calls of the plugins). At exit the log lists how often each hook was called.

## Example: HewDraw Remix

HewDraw Remix (HDR) is a set of Skyline plugins plus files that ARCropolis loads. Installed in the SD folder as its
release describes (its `hdr` folder under `ultimate\mods`, ARCropolis, smashline, nro_hook, stage_alts, stage_config and
the HDR launcher in the plugins folder), it loads with the steps above. HDR changes the game's code in places a lifted
build cannot follow without a re-lift (inline hooks and patches inside functions, and smashline's changes to the common
fighter module): steps 2 and 3 compile those in. smashline's patches to the common module are made when the first
fight loads, after the recording boot: the first game runs with them `NOT ACTIVE` once, and the next start re-lifts with
them (`sites_last.log`).

## Example: UltimateTrainingModpack

[UltimateTrainingModpack](https://github.com/jugeeya/UltimateTrainingModpack) (v7.1.1, the release ZIP: `libtraining_modpack.nro`,
`libnro_hook.nro`, `libparam_hook.nro`): unpack the ZIP into `sd\` (its `atmosphere\` and `ultimate\` folders land there) and
start the game; its menu opens in Training with B + d-pad up or a long press of Start, and its settings go to
`sd\ultimate\TrainingModpack\`. What it needed beyond HewDraw Remix, now part of the tree:

- **SDK entry points only plugins import** (`runtime/sky_compat.c`, registered by `skyline.c`): the modpack's start-up builds a
  telemetry event from `nn::oe::GetPseudoDeviceId` and `nn::account::GetLastOpenedUser`, its updater and telemetry use
  `nn::ssl`, `nn::socket::Connect` / `Poll` / `GetLastError` and `nnsocketGetAddrInfo`, and its Rust standard library uses
  `clock_gettime`. The game never imports any of them, so the runtime had no handler and stopped at the first call
  ("calls X, which nothing provides"). Now: a fixed device id, the port's one user ({1, 0}), "no network" from every socket
  and SSL call the game does not import itself (a name main imports keeps the game's handler; the modpack's version check
  is skipped anyway: the emulator check holds here), and the host clock.
- **NEON instructions the lifter did not translate** (`tools/nro/recomp_nro.py`, `tools/lifter/recomp.py`, `runtime/neon.h`):
  the modpack finds its hook addresses with a byte search (Rust `memchr`, NEON code): `bic` / `orr` with a modified
  immediate, `mla` / `mls`, `rev16` / `rev32`, the integer by-element multiplies (`mla`, `mls`, `mul`, `sqdmulh`,
  `sqrdmulh`) and the widening three-register forms (`uaddw`, `umlal`, `umull` and their relatives). An untranslated
  instruction stops the game when it runs (`guest_unimpl`); the game's own code has none of these, so its lifted files are
  unchanged.
- Its sites: 1 patch (a `nop` written over the stage-select check of Training), 6 inline hooks inside functions (the stale-move
  menu, the Training reset check, the layout-archive allocation, a Pokemon Trainer switch) and 57 function hooks on main,
  13 of which the base objects had inlined into their callers; 3 function hooks on `lua2cpp_common`, 2 of them inlined.
  All of them are made when the plugins start and when the common module loads, so the recording boot of step 2 sees
  them all; `relift_sites.py build` for main (15 bucket files, about a minute) and `--module=common` (2 files) compile
  them in. The game then runs with everything active (no `NOT ACTIVE` site).

A prebuilt runtime package made before these changes (its `manifest.json` names the commit) lacks the SDK entry points
above: the modpack stops at the first of them. The package has to be made from a tree with them (`scripts/make_release.py`).

## Known problems

- Two of smashline's hooks are in the SDK (`NOT ACTIVE`): its unwinder hook, whose effect `nnunwind.c` provides instead,
  and an inline hook next to it.
- The recording boot sees what the plugins hook at their start and when the common module loads. A plugin that hooks
  something new later (a mode played for the first time) shows `NOT ACTIVE` in that game's log; the next start notices
  it (`plugin_bin\sites_last.log`) and re-lifts again, so that game runs with the hook inactive once.
- A plugin build whose emulator check has another shape keeps taking its console path; give that plugin a patch list of
  your own (`--patches`).
