# ssbu-recomp

A **static recompilation** of *Super Smash Bros. Ultimate* (version 13.0.4) from the Nintendo Switch to a
native Windows x64 program, with its own runtime, a Vulkan renderer, and tools to turn the fighters' script
code back into readable, editable C++.

It is **not an emulator**: there is no CPU interpreter and no JIT. The game's AArch64 machine code is
translated to C ahead of time and compiled for the PC; the console's system libraries, graphics API, audio,
video, input and file system are re-implemented by this project's runtime.

> **This repository contains no game code, no game assets, no keys and no firmware.**
> You need your own legally obtained copy of the game (13.0.4). Everything derived from it is generated on
> your PC, from your dump, and never leaves it. The only game-related text in the tree is names: a list of
> system-library function names chosen for translation, and function names used by the tools.

![The menus running natively](media/menus.webp)

## Status (October 2026)

Early, playable in parts, not finished. What is written here was measured on the development PC; where
something was not checked, it says so.

| Area | State |
|---|---|
| Boot → opening movie → title → menus | works |
| Smash (1v1 vs CPU), whole match to the results screen | works |
| Classic Mode, Training, Home-Run Contest | start and play (tested with scripted input only) |
| Spirits board and collection, Vault (sounds, replays, tips, shop), options, Mii Fighters | reachable |
| Adventure (World of Light) | intro reached; map and fights not tested |
| Team battles, Squad Strike, Tourney, 3-8 players, Stage Builder editing | not tested |
| Online | not available (the network services answer as an offline console) |
| Renderer | Vulkan by default, OpenGL as a fallback. Pipeline libraries per shader stage, prepared ahead by background threads from the game's shader containers (the runtime reads them from your `data.arc` itself), and a loading barrier that waits for the containers the game is about to use: **0 pipeline holds in fight frames** in the measured first-run tests on an NVIDIA and an Intel GPU, with one scripted fight (and 0 in three more fights with other fighters and stages in the fast test mode). Not shown: every window clean on a cold first run (NVIDIA: 6 frames of 34-38 ms at the very start of the fight in one of two runs), a pixel comparison with the OpenGL renderer, and the preparation of a changed `data.arc`. The preparation takes about 15 minutes of play on a first run and costs disk (about 2.5 GB of shader files, up to 0.9 GB of pipeline cache) |
| Freeze watchdog | no presented frame for 5 s writes a report (locks, waits, the stack of every thread) to the log and to `<exe name>_freeze.txt`; with a linker map next to the exe it names the host functions. A freeze that was reported once was not reproduced in long runs (26-27 min, three runs), so its cause is **not established** |
| Speed | 59-60 fps in a fight when nothing new has to be prepared; after a first run's preparation the second run needs a loading barrier of 0.2-0.3 s |
| Integrated GPU | a whole match runs on an Intel Raptor Lake iGPU at 59-60 fps; cold first run: 0 fight frames over 33.4 ms in 2 of 2 runs |
| Controllers | any pad through SDL3 (database mapping, hot-plug, 8 players, remapping file, rumble, gyro) — passes its automated tests with virtual devices; **not yet confirmed with physical pads** |
| Save data | journalled like the console's (two-phase commit) |
| Mods | fighter scripts as readable C++ rebuilt in about a second; a hook table makes every function of the game and of the script modules replaceable from a mod DLL without rebuilding the game (self-tested; shown with two small edits) |
| Fighter source | the shared state machine: 489 functions are installed as readable C++ in place of the lifted code, each proven on real calls (the lifted and the source version compared on the same arguments); the per-fighter status scripts of all 96 fighter modules: 20,256 of 23,834 functions (85.0%) decompile to source that is equal to the original offline (19,162 bound functions compared, 0 differing); 8 fighters (Mario, Luigi, Link, Samus, Fox, Captain Falcon, Donkey Kong, Ness) run their status source in deterministic matches with identical per-frame logs (2,010 fight frames each, against the lifted version; one fighter, Pikachu, was reverted after one differing function). Not shown: the other fighters in matches |
| Test modes | a deterministic mode (fixed clock seed, per-frame state hash log) to compare two builds frame for frame, and a debug fast mode (`tests/DEBUG_MODE.md`: speed-up, movie skip, fighters/stage/stocks chosen from the environment, a fixed end after N fight frames). Test-only; none set, the code paths are not taken |
| New fighter kinds | a 95th fighter kind fights as a copy of an existing fighter, on a separately built game DLL produced by a patch list (`tools/lifter/patches/`, 175 instruction patches in 73 functions; the default build is untouched); own kind hash, own parameter-table rows and a roster row work. Own files and own moves are **in progress**: a fighter under its own file name does not load yet. See [docs/NEW_FIGHTER.md](docs/NEW_FIGHTER.md) |
| Matching decompilation | paused |
| Rollback netcode | planned, not started |
| Linux / Steam Deck | not started (the runtime is Windows-only today) |
| Building from a fresh clone | **verified** (October 2026): `python scripts/setup.py --dump <your dump> ...` builds a running game in about 45 minutes and passes the scripted match; see [BUILDING.md](BUILDING.md) |

## How it works

```
your dump ──► tools/extract ──► main executable, SDK, script modules, data
                 │
                 ▼
        tools/lifter (recomp.py)        AArch64 ─► C, one C function per game function
                 │                       checked against Unicorn on 32,043 instruction cases
                 ▼
        clang ─► game DLL  +  one DLL per script module
                 │
                 ▼
        runtime/                         what the console provided, re-implemented:
          hostrt, os_threads             loader, threads, memory, C++ exceptions through lifted code
          nn*.c                          ~1,100 system-library entry points (files, time, audio, input,
                                         movies, fonts, applets, accounts, network as "offline")
          nvn_vk*.c, nvn_gl.c            the console's graphics API (NVN) on Vulkan / OpenGL
          mx_translate.c                 the game's GPU shader machine code ─► GLSL ─► SPIR-V
          nnpad_sdl.c                    controllers through SDL3
```

Three layers, three different natures:

- **Game code** is recompiled, not emulated. It is a mechanical translation: complete and exact, but not
  readable source.
- **System calls** are re-implemented on Windows, the way Wine re-implements Windows calls. Part of the
  console's own library code (shipped inside the game) is lifted the same way as the game.
- **Graphics** are translated at the API level (not at the level of GPU hardware), and the shaders are
  translated when they are loaded.

### Readable source where people edit

A fighter's moves are small scripts. `fighter_sdk/tools` decompiles them mechanically into C++ like this
(generated on your PC from your dump; not stored in this repository):

```cpp
ACMD(mario, game_attack11) {
    frame(agent, 2);
    if (is_excute(agent)) {
        ATTACK(agent, 0, 0, hash40("top"), 2.2f, 361, 30, 0, 20, 1.2f, 0.0f, 6.3f, 6.0f, /* ... */);
    }
    wait(agent, 2);
    if (is_excute(agent)) {
        AttackModule::clear_all(boma);
        WorkModule::on_flag(boma, FIGHTER_STATUS_ATTACK_FLAG_ENABLE_COMBO);
    }
}
```

On the development PC the tool turns 80,880 of 84,886 animation-command scripts of all fighters (95%)
into source that behaves the same as the original in a differential test; a fighter rebuilds in about a
second and the game picks it up. The shared fighter state machine (dash, turn, air dodge, landing, cancel
rules) is being converted with a second, general decompiler.

## Features

- Native Windows x64 executable; no CPU emulation at run time
- Vulkan renderer (pipelines prepared on worker threads, driver cache on disk) and an OpenGL renderer
- Options in `config/graphics.ini`: renderer, window / borderless / fullscreen, size, vsync mode, GPU
  choice, anisotropic filtering, frame-time line (internal resolution scale: in progress)
- Any controller through SDL3; `config/controllers.ini` for remapping; `--pad-test` and `--pad-map`
- The game's own movies (VP9), audio (Opus) and fonts, decoded by the runtime
- Deterministic scripted input and recording/replay for testing
- An audit that classifies every system call handler as real / lifted / simplified, with a strict mode
  that stops the game the moment it calls something that is not really implemented

## Plans

1. Every mode playable from start to end, steady 60 fps (fight frames are hold-free in the tests above; other modes are not measured)
2. Internal resolution scale and a low-end profile
3. Mod platform: file overrides, the function hook API with a small C++ SDK, documentation
4. The shared fighter state machine as readable source (movement tech, cancel rules), so that changes to
   the battle system (wavedash, perfect pivot, a turbo mode) are ordinary mods; further out, new fighters,
   new modes, extra character slots and a larger character select screen
5. Rollback netcode with an own network layer: deterministic stepping, state snapshots, then the protocol
6. Verified build from a fresh clone; then other platforms

## Requirements

To run: Windows 10/11 x64, a GPU with a Vulkan 1.3 driver, your own dump of the game (version 13.0.4).

To build: Python 3.10+ (numpy, lz4, zstandard, Pillow, unicorn), LLVM/clang 9.0.0, MSVC Build Tools with a Windows SDK,
SDL3 (VC development package), Khronos Vulkan-Headers, glslang (Windows release), a yuzu-family source checkout for the
self-test reference data, libopus as a DLL (for the music; without it the game runs silent), and your dump. Exact
versions, links and the one command: [BUILDING.md](BUILDING.md).

## Repository layout

| Folder | Contents |
|---|---|
| `runtime/` | the host runtime: loader, system libraries, graphics, audio, input |
| `tools/lifter/` | AArch64 → C recompiler, build scripts, differential tests |
| `tools/nro/` | the same for the game's script modules |
| `tools/extract/` | reading your dump |
| `tools/audit/` | the "what is really implemented" audit |
| `fighter_sdk/` | fighter script decompilers, the C++ SDK and runtime for fighter source, the hook API |
| `mods/` | small test mods (C++ source) for the hook API and the new-fighter experiments |
| `tests/` | scripted input sequences, test drivers and measuring scripts (`DEBUG_MODE.md` documents the test modes) |
| `docs/` | `MODDING.md` (mods and fighter source), `NEW_FIGHTER.md` (what a new fighter needs, what works) |

## Related work

The matching decompilation that this project also contributes to is a separate effort based on
[sbergeron42/ssbu-decomp](https://github.com/sbergeron42/ssbu-decomp); the project's own work on it is paused. Technique was learned from the
open-source emulators (yuzu and its successors), from SDL, from the N64 static-recompilation projects and
from other Switch recompilation and decompilation projects; names of script functions and constants come
from the Smash modding community's public work (param labels, skyline-smash, ARCropolis).

## How this was made

**This project was built with AI, and that is part of the point.**

I am not a reverse engineer or a security engineer, and I did not come up through whatever field normally
teaches this. Almost all of the code here was written by AI coding agents (Claude) that I direct: I decide
what to try, think up the angle of attack or the workaround, and the AI implements it quickly and comes
back with a result I can check. That loop — an idea, an implementation within the hour, a measurement that
says whether it held — is what made a project of this size possible for one person.

What I take from it: the bar for entry has dropped a great deal. The creativity is still yours — which
problem to solve, which trick might get around it, when a result is not good enough — but the distance
between having the idea and seeing whether it works is now very short.

Two rules kept it honest: every claim has to come from a test or a measurement (this README says what was
measured and what was not), and nothing derived from the game goes into the repository. It is a study
project; expect rough edges, and mistakes that a specialist would not have made.

## Contributing

Testers with other GPUs and controllers, and people who know the game's internals or the modding scene's
tooling, are very welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) first: three rules (nothing derived from
the game in the repository, clean sources only, every claim measured) apply to every contribution.

## Legal

This project is not affiliated with, endorsed or sponsored by Nintendo, Bandai Namco or Sora Ltd.
*Super Smash Bros.* and *Nintendo Switch* are trademarks of their owners and are named only to say what
the software works with. The repository contains no copyrighted game data, no console keys and no
firmware; it does not help anyone obtain them. You must own the game and dump it yourself.

Licensed under the GNU General Public License, version 3 or later (see `LICENSE`).
