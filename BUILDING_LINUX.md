# Building and running on Linux

A native Linux build: the game becomes an ELF executable and shared libraries, with SDL3 for the window, sound and
controllers, Vulkan for graphics and FFmpeg for the movies. No Wine and no emulator. As on Windows, the repository holds
no game code: everything is generated on your PC from **your own dump** of Super Smash Bros. Ultimate **13.0.4 or 13.0.5**
(only 13.0.4 has been tried on Linux so far).

```
git clone <this repository> ssbu-recomp
cd ssbu-recomp
scripts/linux/install_deps.sh                                   # system packages (sudo), Python venv, small source deps
deps/venv/bin/python scripts/setup.py --target linux --dump <your dump folder>     # about 1.5 hours, resumable
./play.sh                                                       # play
```

## Status

Tested on one PC: CachyOS (Arch-based, kernel 7.2), 24 threads, 31 GB RAM, NVIDIA RTX 3080 (proprietary driver), Wayland.

| | |
|---|---|
| Build from the dump | works: game library, 98 script modules, executable; `check` step passes (SDK self-test, pad self-test 56/56, C++ runtime self-test 145/145, differential test 32,043 / 32,043; it runs only with `--full-check`) |
| Opening movie, title, menus, character and stage select | work |
| F1 settings menu | built and drawn as on Windows (Dear ImGui over the Vulkan picture; keyboard, mouse and controller), see [Settings menu (F1)](#settings-menu-f1). **Compiles and links in CI; not yet tried on a real Linux desktop or a Steam Deck** |
| On-screen keyboard | not on Linux (it draws with Windows APIs). Text input, for example naming a Mii, opens a zenity or kdialog dialog instead |
| A whole Smash match (scripted input, 1v1 vs CPU) | works: steady 60 fps, sound, no audio underruns |
| Keyboard | works (tested); controllers through SDL3 are the same code as on Windows but **not tested with a real pad on Linux** |
| Album movie recording (the game's video capture to the album) | **not implemented** on Linux; the 7 self-test cases for it are accepted as known failures |
| OpenGL renderer | not on Linux: Vulkan only |
| Online play (experimental, `SSBU_ONLINE`, [docs/ONLINE.md](docs/ONLINE.md)) | the same code as on Windows. Tried: two games on one PC through a lobby on the PC (`tests/lwr_pair.py`, rollback off): a Local Wireless room match from start to results, identical frame by frame in both games; Local Wireless rooms of the public lobby, matches against players on Windows over the internet (one PC, 2026-10-21). UPnP is the IGD protocol on plain sockets: tried only against a simulated router (mapping made; its removal at exit does not run, `ExitProcess` is `_exit` here: the next start removes it), not on a real one. Rollback netcode: not on Linux (it needs the state save / restore of `r1_state.c`) |
| Other distributions, AMD / Intel GPUs, X11, Steam Deck | not tested. The package lists for Debian/Ubuntu and Fedora in `install_deps.sh` are best guesses; `setup.py` checks every library and says which one is missing |

## What you need

- **A dump of the game, 13.0.4 or 13.0.5** (base game with the update applied), from a console you own. The layout is the
  same as on Windows, see [BUILDING.md, "What you dump from your own console"](BUILDING.md#what-you-dump-from-your-own-console):
  ```
  <dump>/exefs/          main, sdk, rtld, subsdk0, main.npdm
  <dump>/romfs/          data.arc and the rest of the RomFS
  <dump>/sysdata/fonts/  optional, the console's shared fonts (see "Fonts" below)
  ```
  From `.nsp` files and your own keys: `deps/venv/bin/python tools/extract/romfs_extract.py <base.nsp> <update.nsp> <out>`
  and `tools/extract/nca_exefs.py`; they read the keys from `~/.switch/prod.keys`.
- **RAM:** 16 GB or more. `setup.py` runs one compiler per CPU thread by default (at most 24; `--jobs N` overrides it),
  and the compilers share RAM - 4 GB: each one reserves what its file is expected to need before it starts (at least
  1.5 GB; most files need well under 1 GB, a few need many GB) and is held to it. A file that needs more is compiled again
  with twice as much, up to the whole share; a few generated files need more than that at full optimisation (one passed
  20 GB) and are compiled with less optimisation instead, and the build log lists those files. Running out of memory
  would not slow the build down, it would get processes killed, possibly your desktop's: that is what the sharing
  prevents. (Before 2026-10-20 the rule was one compiler per 4 GB of RAM: 3 on a 16 GB Steam Deck with 8 threads.)
- **Disk:** about 12 GB for `build-linux/`, plus about 4 GB of shader cache after the first play session, plus the dump
  itself (about 16 GB).
- **GPU:** a Vulkan 1.3 driver. With `VK_EXT_graphics_pipeline_library` (current NVIDIA, Mesa RADV and ANV drivers have
  it) shaders are prepared ahead of time and nothing is compiled during a fight.
- **A distribution with SDL3 and clang 19 or newer:** current Arch and its derivatives, Fedora 42+, Debian 13,
  Ubuntu 25.04+.

## 1. Dependencies

```
scripts/linux/install_deps.sh
```

It installs, using your package manager (pacman, apt or dnf; asks for sudo):
clang, pkg-config, SDL3, FreeType, OpenSSL, FFmpeg, glslang, the Vulkan loader, libGL, Opus, Python, git.
Then, without root, into `deps/` (ignored by git):

- `deps/venv`: a Python virtual environment with numpy, lz4, zstandard, Pillow, unicorn, capstone, pycryptodome
- `deps/Vulkan-Headers`: the Khronos headers, tag v1.4.351 (the version the port was built with)
- `deps/suyu`: a 2 MB sparse checkout of the Eden emulator's Mii service sources; only reference data for a self-test is
  read from it

`--no-system` skips the package manager if you install the packages yourself. On another distribution the script lists
what to install.

**Steam Deck (SteamOS):** SteamOS is read-only and every update removes what pacman installed, so `install_deps.sh` stops
there and shows how to build inside a [distrobox](https://distrobox.it) container instead (SteamOS 3.5 and newer
include distrobox and podman), in Desktop Mode, from Konsole, with this folder in your home folder:

```
distrobox create --name ssbu --image docker.io/library/archlinux:latest
distrobox enter ssbu
scripts/linux/install_deps.sh            # inside the container; on a Deck it also installs vulkan-radeon and libpulse
deps/venv/bin/python scripts/setup.py --target linux --dump <your dump folder>
./play.sh                                # also from inside the container (distrobox enter ssbu)
```

`install_deps.sh --steamos-host` installs on SteamOS itself instead (it needs a sudo password,
`sudo steamos-readonly disable` and the pacman keys, and the next SteamOS update removes the packages again). A Deck now runs 8 compilers instead of 3: the build should take
roughly 2 to 2.5 hours instead of 4.5 (an estimate from the job counts, not measured on a Deck yet).

The FFmpeg must be able to decode VP9 and Vorbis (the game's movies). Every distribution's normal FFmpeg can;
`setup.py` checks it.

Optional: `zenity` or `kdialog`. The game's on-screen keyboard (naming a Mii, a ruleset, ...) and the settings menu's
"Type in a dialog" buttons (your name, a room code) open a dialog with one of them.

`install_deps.sh` also fetches Dear ImGui v1.91.9b (MIT) into `deps/imgui`: the in-game settings menu (F1). Without it the
game builds without the menu (`setup.py` says so; `--imgui <folder>` or `IMGUI_ROOT` point to another copy).

## 2. Build

```
deps/venv/bin/python scripts/setup.py --target linux --dump <your dump folder>
```

Use the venv's Python, as shown. The build goes into `build-linux/`, in eleven steps. The same table as in
[BUILDING.md](BUILDING.md#the-steps) applies, with `.so` libraries instead of DLLs. Measured on the PC above:

| Step | Time |
|---|---|
| deps, stage, convert, modules, tables, lift, sdk | under a minute together |
| compile (1,161 files, 567 MB game library) | about 1.5 hours with 4 jobs (measured: 77 min for 967 of the files) |
| nro (98 script modules) | 14 min |
| link, check | about 1 min |

The build can be stopped at any time (Ctrl+C, or a crash). Run the same command again and it continues where it
stopped; finished steps are skipped. Every invocation needs `--dump`. Other options: `--status`, `--redo <step>`,
`--from <step>`, `--jobs N`, `--full-check` (the check step also runs the translator's long differential test), `--help`.
Files that fail to compile (often for lack of memory) are compiled again with fewer jobs before the build stops.
A file that runs into the 180 s time limit is compiled again with more of its longest functions unoptimised; a retry
that would leave exactly the same functions unoptimised as the attempt that just failed is skipped (it would fail the
same way), so the few hopeless files reach their unoptimised fallback after one or two 180 s attempts, not three.

When something is missing, step `deps` stops at once with what to install. It compiles and runs a small test program
against every library, so a broken package shows there and not an hour later.

## 3. Play

```
./play.sh
```

It works from any folder (a symlink to it in `~/bin` is fine) and takes the options of `scripts/run.py` (`./play.sh --help`).
The game's output shows in the terminal and is kept in `~/.local/share/ssbu-recomp/last_run.log`.

- Saves go to `~/.local/share/ssbu-recomp/userdata` (`--userdata <folder>` for another one).
- `scripts/linux/install_desktop.sh` adds **Super Smash Bros. Ultimate (recomp)** to your application menu; the game's
  output of the last start is then in `~/.local/share/ssbu-recomp/last_run.log`. `--remove` takes it out again.
- **First start:** the game waits at a progress screen while it reads its shaders out of `data.arc` and prepares every
  one for your GPU. On the PC above, with the dump on a hard disk, reading took 101 s and the preparation was 61 % done
  200 s after the start (the screen then estimated 2 more minutes). After that the cache in `build-linux/cache/` is
  used, and the next start goes straight to the game. A driver update starts the preparation once more.
- **On a new save**, after the title screen a "Milestones" banner waits for a few presses of A.
- Close the window to quit.

### Controls

Controllers: any pad SDL3 knows (Xbox, PlayStation, Switch Pro / Joy-Con, 8BitDo, GameCube adapters, ...), up to 8
players, hot-plug. The keyboard is one more player:

| Keys | Button |
|---|---|
| Arrow keys | left stick |
| Z  X  C  V | A  B  X  Y (in the positions of a Switch controller) |
| Q  E | L  R |
| A  S | ZL  ZR |
| I  J  K  L | d-pad |
| Enter, Backspace | +  − |

Remapping, player slots, dead zones and rumble: `build-linux/config/controllers.ini`. It is written on the first
start, explains itself, and is re-read within a second of being saved. `build-linux/work/ssbu_native --pad-test` shows
what a pad sends.

Window: `build-linux/config/graphics.ini` (`window = windowed | borderless | fullscreen`, size, vsync), or F1 > Display.
Only `windowed` was tested on Linux.

### Settings menu (F1)

The same menu as on Windows ([BUILDING.md, "In-game menu (F1)"](BUILDING.md#in-game-menu-f1)), drawn over the game.

| To open or close it | |
|---|---|
| Keyboard | **F1** (F2: the stats overlay) |
| Controller | the **Home / Guide** button or the **Capture / Share** button (when the system lets SDL see it) |
| Any controller, Steam Deck | hold **− and + together for 1 second** (Xbox-style pads: View + Menu; Steam Deck: the two buttons left and right of the screen, View and Menu) |
| Close | also **+** or **Esc**, or the Close button |

Inside: d-pad / left stick move, A selects, B goes back, L / R switch pages; mouse and keyboard work too.

**Steam Deck:** the Steam button and the "..." button belong to Steam, so use **View + Menu held for 1 s**. While you hold
them the game sees − and + (a match may pause) until the menu opens; after that the game gets no input until you
release every button. Another way: in the game's Steam Input layout, bind a back button (L4 / R4 / L5 / R5) to the
keyboard key F1. Text boxes: in Desktop Mode use the keyboard or "Type in a dialog" (zenity / kdialog); in Game Mode,
Steam's on-screen keyboard (Steam + X) types into the active box (SDL may also open it by itself when a box is
selected; not verified).

Not on Linux yet: **Rollback netcode** (online matches use the game's own online netcode),
and the Log page shows the log only when the game's output goes to a file (`play.sh` does that).

## Fonts

The game asks the system for the console's shared fonts, which are not part of the game dump. Without them, the
open-source replacements the runtime ships in `runtime/fonts_open` are used, as on Windows (see its README: Latin
text in Open Sans and Source Sans Pro, no Chinese, Japanese or Korean characters). With a firmware dump from your
console: `deps/venv/bin/python tools/extract/fw_fonts.py <firmware folder> <dump>/sysdata/fonts`, then run setup with
the same `--dump` (add `--redo convert` if you built before).

## Troubleshooting

- **Something gets killed during `compile` or `nro`** (the desktop freezes, then a program is gone; `journalctl` shows
  `oom`): too many compiler jobs for your RAM. Run again with a lower `--jobs`; finished files are kept. With
  systemd you can also cap the build:
  `systemd-run --user --scope -p MemoryMax=20G deps/venv/bin/python scripts/setup.py --target linux --dump <dump>`.
- **The game exits at once with code 7:** a font file is missing (see "Fonts"). The log line starts with `nn::pl:`.
- **A black window that never changes:** look at the output for `nvn_vk:` lines. `vulkaninfo --summary` must list your GPU.
- **No sound:** the game plays through SDL3, so PipeWire / PulseAudio must be running. The log says which audio device was opened.
- **To report a problem**, run `scripts/linux/collect_logs_linux.sh`: it makes `~/ssbu_linux_report_<date>.zip` with the
  build logs, the last game logs and facts about your PC (distribution, GPU and driver, Vulkan, library versions), never
  your dump or a game file. Send that file, or at least `~/.local/share/ssbu-recomp/last_run.log` (written by every `./play.sh`). It lists the
  libraries, the GPU and driver, the controllers found, and the error. If no frame was shown for 5 s,
  `build-linux/work/ssbu_native_freeze.txt` has a report of every thread.

## How the port works (for developers)

The runtime is written against the Win32 API. Rather than rewriting it, the Linux build keeps those calls and supplies
them: `runtime/plat/windows.h` and the other headers in `runtime/plat/` declare the subset of Win32 the runtime uses, and
`runtime/plat_posix.c` implements it on POSIX. That covers threads, events, condition variables on futexes,
`VirtualAlloc` with a page map, `GetWriteWatch` on userfaultfd write protection (as Wine does it; kernel 6.7+), files,
`LoadLibrary` (a `.dll` name maps to the `.so`), fibers, waveOut on SDL3 audio, and CryptoAPI on OpenSSL. Where its behaviour differs from Windows in a way a caller
could notice, a note says so at the function. `runtime/plat_mf.c` provides the part of Media Foundation the movie
player uses, on FFmpeg. Code that really is
Windows-only is under `#ifdef _WIN32`, with Linux branches: the SDL3 window, the Vulkan surface, FreeType glyph
outlines, the time zone from `/etc/localtime`, `/proc` thread reports, and the signal crash handler.

`RECOMP_TARGET=linux` (set by `setup.py --target linux`) switches the build scripts to `-fPIC`, `-fvisibility=hidden`, `.so` outputs, and an
executable that finds its libraries next to itself (`rpath $ORIGIN`). `tests/plat_selftest.c` tests the compatibility
layer by itself:
`clang -I runtime/plat -include runtime/plat/plat_pre.h tests/plat_selftest.c runtime/plat_posix.c $(pkg-config --cflags --libs sdl3 openssl) -lm -o plat_selftest`
(55 checks, 0 failures on the PC above).

## What to report (testers)

Please answer these, even if the answer is "no", and attach the file of `scripts/linux/collect_logs_linux.sh`:

1. Your distribution and version, GPU and driver, X11 or Wayland.
2. Did `install_deps.sh` and `setup.py` finish? If not, which step failed (the line starting with `FAILED at step`)?
3. Did the `check` step pass (the line `SSBU_SDK_SELFTEST:` and the differential test line)?
4. Did the game window open? Did the opening movie play, with sound?
5. Did the title screen appear? Did a fight start, and how did the speed feel?
6. Which controller did you use (keyboard, which pad)?
