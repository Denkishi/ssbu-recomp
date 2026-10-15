# Building from a fresh clone

The repository holds only this project's own tools and runtime. Nothing that comes from the game or from the console's
software is in it, with one stated exception: a list of function names (see "The SDK function list"). Everything else of that kind (the lifted C code, the lifted system-library code, the generated headers and
tables, the script-module libraries) is **generated on your PC from your own dump**, into `build/` (ignored by git).

```
git clone https://github.com/Denkishi/ssbu-recomp
cd ssbu-recomp
python scripts/setup.py --dump <your dump folder> --suyu <folder> [--llvm ... --sdl3 ... --vulkan-headers ... --glslang ...]
```

One command, eleven resumable steps. When it ends it prints how to start the game.

## What you need

| Requirement | Version tested | Where from |
|---|---|---|
| Windows 10/11 x64 | 11 | |
| Python | 3.14 (3.10 or newer should do) | https://www.python.org/downloads/ |
| Python packages | numpy, lz4, zstandard, Pillow, unicorn | `pip install numpy lz4 zstandard Pillow unicorn` (the build needs only these; the fighter tools and `tools/lifter/patches/kinds_scan.py` also use `capstone`) |
| LLVM/clang | **9.0.0** (8.0.0 also works) | https://github.com/llvm/llvm-project/releases/tag/llvmorg-9.0.0 (Windows installer, 64-bit) |
| MSVC Build Tools, with the "C++ x64/x86 build tools" and a Windows 10/11 SDK | 2026 (v18) / SDK 10.0.26100 | https://visualstudio.microsoft.com/downloads/ ("Build Tools"). clang finds it by itself; `build_glslang.bat` finds it with `vswhere` |
| SDL3, development package for Visual C++ (`SDL3-devel-<ver>-VC.zip`) | 3.4.18 | https://github.com/libsdl-org/SDL/releases |
| Khronos Vulkan-Headers | the 1.4.x tag matching your driver | https://github.com/KhronosGroup/Vulkan-Headers |
| glslang, Windows x86_64 release package (`glslang-*-windows-x86_64-release.zip`, has `include/` and `lib/`) | 16.6.0 | https://github.com/KhronosGroup/glslang/releases |
| Source checkout of a yuzu-family emulator (only `src/core/hle/service/mii/` is read, by `tools/lifter/gen_sdktest.py`, for the reference data of the self-test) | suyu | any yuzu-lineage open-source repository that still has `src/core/hle/service/mii/types/raw_data.cpp` |
| A GPU with a Vulkan 1.3 driver (to run, not to build) | | your GPU vendor |
| Media Foundation (movies) | part of Windows | |
| libopus as a DLL (the music and the game's other Opus sounds; **without it they are silent**, the rest runs) | 1.6.1, built from source as below | the source release from https://opus-codec.org/downloads/ (check it against Xiph's `SHA256SUMS`), or MSYS2's `mingw-w64-ucrt-x86_64-opus` |

Unpack SDL3, Vulkan-Headers, glslang and the emulator checkout anywhere and give the folders with `--sdl3`,
`--vulkan-headers`, `--glslang`, `--suyu`, or put them under `deps/SDL3`, `deps/Vulkan-Headers`, `deps/glslang`,
`deps/suyu` (ignored by git), or set `SDL3_ROOT`, `VULKAN_HEADERS`, `GLSLANG_ROOT`, `SUYU_ROOT`. clang: `--llvm <folder
with bin\clang.exe>` or `LLVM_ROOT`, default `C:\llvm-9.0.0`. The Vulkan SDK is not needed to build.

libopus: give the DLL or its folder with `--opus`, or put it under `deps/opus` (`opus.dll` or `libopus-0.dll`), or set
`OPUS_DLL`; step `link` copies it beside the exe. Built from the source release with CMake (the "C++ CMake tools for
Windows" component of the Build Tools, or cmake.org) and the MSVC tools:

```
cmake -S opus-1.6.1 -B opus-build -DBUILD_SHARED_LIBS=ON -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
cmake --build opus-build --config Release
python scripts/setup.py ... --opus opus-build
```

MSYS2's `libopus-0.dll` (`C:\msys64\ucrt64\bin`, `mingw64`, `clang64`) needs nothing: the runtime loads it from there.
Without any of these the deps step and the link step say so, and the log of a run says
`nn::codec: libopus NOT FOUND: Opus streams will be silent`. At run time `SSBU_OPUS_DLL=<file>` names another DLL.

## What you dump from your own console

The port handles **Super Smash Bros. Ultimate 13.0.4** (base game with the 13.0.4 update applied) and nothing else; step
`convert` refuses another build. From your own copy of the game, with the homebrew dumping tools you prefer (for example
nxdumptool, running on a console you own), produce a folder:

```
<dump>/exefs/      the whole ExeFS: main, sdk, rtld, subsdk0, main.npdm   (with the update applied)
<dump>/romfs/      the RomFS with the update applied (it contains data.arc)
<dump>/sysdata/fonts and <dump>/sysdata/zoneinfo      optional: the system shared fonts (.ttf) and the tz database files
```

If you only have `.nsp` packages and your own keys, `tools/extract/romfs_extract.py <base.nsp> <update.nsp> <out>`
(RomFS with the update applied), `tools/extract/nca_exefs.py` (ExeFS from an NCA) and `tools/extract/fw_fonts.py`
(system fonts from a firmware dump) do the same; they read the keys from `~/.switch/prod.keys` and need
`pip install pycryptodome`. This repository contains no keys and no link to any.

## The SDK function list

`sdklift.py` lifts selected functions of the console SDK module (`sdk`) to C. Which functions, and where they must be cut
or trapped, is a curated list of that module's function names with this project's notes:
`tools/lifter/sdklift_roots.txt`. It holds names and addresses only, no code. `--sdk-roots <file>` uses another list; the
format is documented in `tools/lifter/sdklift.py` and `tools/audit/lift_select.py` regenerates its automatic block.

## The steps

| Step | What it does |
|---|---|
| `deps` | checks every tool above and the dump; runs a test compile and link |
| `stage` | lays the sources out in `build/work` (the tools expect their old single-folder layout) |
| `convert` | `main` and `sdk` (NSO) to `build/binaries/13.0.4/*.elf`; checks the build and the segment hashes |
| `modules` | extracts the game's script modules (NRO) from `data.arc`, writes their address map and registry |
| `tables` | generates every header and table the runtime needs (imports, SDK data, self-test vectors, exception-table tests) |
| `lift` | lifts the main executable (`recomp_eh.py`) |
| `sdk` | lifts the system-library functions the runtime uses (`sdklift.py`) |
| `compile` | compiles the lifted main with clang -O1 into `ssbu_game_eh_o1.dll` (functions over 20,000 statements stay unoptimised, 180 s per file) |
| `nro` | lifts and compiles the script modules into `build/work/nro_bin_eh_o2/` (`--modules a,b` for a subset) |
| `link` | builds the glslang shim DLL and links the host runtime into `build/work/ssbu_native.exe`, copies `SDL3.dll` (and libopus, when found) |
| `check` | `SSBU_SDK_SELFTEST`, `SSBU_PAD_SELFTEST`, and the differential lifter test against the Unicorn emulator |

`--status` shows what is done, `--redo <step>` and `--from <step>` repeat steps, `--only <step>` runs one, `--jobs N` sets
the compiler parallelism (default `min(24, cpus)`). A failed step is reported with the tail of its log (`build/logs/<step>.log`);
fix the cause and run the same command again.

### Measured (clean clone, 32-thread PC, `--jobs 20`, other programs running)

| Step | Time |
|---|---|
| deps, stage, convert, modules, tables | 1 s, 0.1 s, 1.4 s, 3 s, 1.3 s |
| lift (112,890 functions, 1,161 C files, 1.3 GB) | 19 s |
| sdk (1,375 functions) | 1.2 s |
| compile (1,161 files, 511 MB DLL) | 27 min 22 s |
| nro (98 modules) | 15 min 20 s |
| link (glslang shim + 44 host files) | 34 s |
| check (SDK self-test 0 failures, pad self-test 49/49, differential test 32,043/32,043 identical) | about 1 min |
| **Total** | **about 45 min** |

Disk: `build/` is about 7 GB (generated C 3.8 GB, objects 1.6 GB, libraries 1.2 GB), plus shader caches that grow as you
play. Verified end to end: the scripted match (`tests/seq_battle_play.txt`, 80 steps) ran 242 s on the built exe: the
input sequence reached its last step, 12,613 frames presented, 0 Vulkan call failures, clean shutdown.

## Running

```
python scripts/run.py --userdata <folder for your saves>
```

`--userdata` is where the game writes its save data (use a copy of a folder while testing). Without `--seconds` the game
runs until you close it. The first run builds shader and pipeline caches in `build/cache` and stutters while it does.

Scripted match, as used to verify the build (window off-screen and muted because the exe is not named `ssbu_native.exe` or
`ssbu_user.exe`):

```
python scripts/run.py --userdata <copy of a save folder> --seq tests/seq_battle_play.txt --seconds 240 --seed 1 --exe ssbu_match.exe --log match.log
```

Fast test run (the debug mode, `tests/DEBUG_MODE.md`; the link step now also compiles `debug_mode.c`; the 44 host files of the
timing table above are 45 since then):

```
python scripts/run.py --userdata <copy of a save folder> --seq tests/seq_battle_play_fast.txt --seconds 240 --exe ssbu_fast.exe --log fast.log --env SSBU_SPEED=0 --env SSBU_SKIP_MOVIES=1 --env SSBU_AUDIO_MUTE=1 --env SSBU_VK_PREP=0 --env SSBU_DEBUG_END=20s
```

`SSBU_SPEED=0` runs uncapped, `SSBU_DEBUG_END=20s` ends the run after 20 s of fight, `SSBU_VK_PREP=0` leaves out the background
shader preparation (on a normal run it works for about 15 minutes of play and writes about 2.5 GB into `build/cache`).

Options are environment variables, set with `--env NAME=VALUE` (full list: comments at the top of the runtime sources):
`SSBU_RENDERER=vk|gl`, `SSBU_SHOW=1` (show the window of a test exe), `SSBU_STRICT=1|report|path` (list the host
functions that are not the real behaviour), `SSBU_TRACE`, `SSBU_PAD_BACKEND=xinput`, `SSBU_FIXED_SEED=<n>`.
Pad mapping file and tools: `ssbu_native.exe --pad-test`, `--pad-map <n>`.

## Troubleshooting

- `"build_glslang.bat" is not recognised` / `vswhere.exe is not recognised`: Visual Studio Build Tools are missing or
  incomplete. Install "Build Tools" with the "MSVC C++ x64/x86 build tools" component and a Windows SDK. The script finds
  `vswhere.exe` at `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\` and calls the batch file by its absolute path;
  a harmless "vswhere" notice from `vcvars64.bat` in `build/logs/link.log` can be ignored when the DLL is produced.
- `missing input: ...main.npdm`: the ExeFS dump lacks `main.npdm`; dump the whole ExeFS.
- `sdk is not the expected build`: not 13.0.4 with the update applied.
- `the list of SDK functions to lift was not found`: `tools/lifter/sdklift_roots.txt` is missing from your clone.
- No music (the menus and fights are silent apart from some effects), and the run's log says `nn::codec: libopus NOT
  FOUND`: there is no libopus beside the exe; see libopus under "What you need", then run `--redo link`.
- A compiler job that fails after a power loss or while the PC is under load: run the same command again; only what
  changed is rebuilt. Closing a running game first matters: a running exe holds its DLLs open and the link fails.
- Output with non-ASCII characters from Windows tools is shown with `?`; the logs in `build/logs` are the originals.
