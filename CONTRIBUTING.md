# Contributing

Thanks for wanting to help. This project is a static recompilation of *Super Smash Bros. Ultimate* 13.0.4 / 13.0.5 to
a native PC program: the tools that translate the game, the runtime that replaces the console, and the
tooling that turns fighter code back into editable source. It is early, it moves fast, and there is a lot
that one person cannot test alone.

Please read the three rules below before anything else. They are not style preferences; a contribution that
breaks one of them cannot be merged, however good the code is.

## The three rules

### 1. Nothing derived from the game goes into the repository

No game code, no lifted or decompiled game functions, no assets, no parameter files, no shader binaries, no
save data, no keys, no firmware. Everything of that kind is generated on each person's PC from their own
dump and stays there. `.gitignore` lists the generated paths; `tools/audit` and the export script check for
them.

In practice:

- Tools may *read* the game's files and may contain names (function names, constant names, file paths inside
  the archive) and addresses. They may not contain the content those names point to.
- A fix for one game function is expressed as a rule in a tool (the lifter, a decompiler, a patch list), not
  as a pasted copy of the function.
- Screenshots and clips of the running port are fine in issues. Dumps, extracted files and generated
  sources are not — do not attach them, do not link to them.
- Do not ask for, offer or link to copies of the game, keys or firmware anywhere in this project.

### 2. Clean sources only

Work only from: your own legally obtained dump of the game, public documentation and headers, and
open-source projects under their licences (credited in the README). Do not use leaked or confidential
material of any kind — in particular no proprietary console SDK, its headers, its documentation, or notes
derived from them. If you have seen such material for the part you want to work on, please work on a
different part.

If you bring technique from another open-source project, say which one in the pull request and keep to its
licence.

### 3. Every claim is measured

The README and the documents say what was tested and what was not. Keep it that way:

- "Works" means you ran it and can say how: which GPU, which mode, which test, how long.
- "Equal to the original" means a comparison said so (see *Testing* below), not that it looked right.
- Say what you did not verify. A pull request that lists its own gaps is easier to merge than one that
  claims everything.
- If you find that something documented as working does not, open an issue. That is a contribution.

## Using AI

This project was built with AI coding agents directed by one person, and says so. AI-assisted contributions
are welcome on the same terms as any other: you understand what you submit, you ran it, and the three rules
hold. Tell us in the pull request what was AI-written and how you checked it. An unverified AI answer about
how the game or the console works is not evidence; a test is.

## Ways to help

You do not need to be a reverse engineer for most of these.

- **Testing on your hardware.** Other GPUs (AMD above all, older integrated graphics), other controllers,
  other Windows versions. Report the GPU and driver version, the options you used, and the log.
- **Controllers.** The input layer uses SDL3 and passes its automated tests with virtual devices; reports
  with physical pads (which pad, how connected, what the `--pad-test` screen showed) are missing.
- **Game modes.** Most modes beyond 1-v-1 against the CPU are untested. A report "mode X: reached this
  screen, then this happened, log attached" is useful.
- **Performance.** Frame-time logs from a first run and a second run on your machine; anything that still
  hitches in a fight.
- **Renderer.** The NVN-to-Vulkan layer, the shader translator, graphics options, low-end profiles.
- **Fighter tooling.** The script decompilers, the fighter SDK, the test host, documentation for modders.
- **Portability.** Linux is not started. Isolating the Windows-specific calls of the runtime behind one
  layer is a well-defined first step.
- **Documentation.** If a step in `BUILDING.md` did not work for you, fix the document.

If you plan something large, open an issue first and describe it. It saves you from building on a part that
is being rewritten.

## Building

See [BUILDING.md](BUILDING.md). You need your own dump of version 13.0.4 or 13.0.5; the build takes about 45 minutes
the first time and generates several gigabytes of files that git ignores.

## Reporting a problem

Include:

- what you did, in steps, and what you expected;
- your GPU, driver version, and the contents of `config/graphics.ini` if you changed it;
- the console log of the run;
- if the game stopped responding: the file `<exe name>_freeze.txt` that the watchdog writes next to the
  executable after a few seconds without a frame — it lists what every thread was waiting for;
- whether it happens every time.

Before attaching a log, check that it contains nothing personal (user names in paths).

## Code

- **Languages.** The runtime is C (C11), the tools are Python 3.10+, the fighter SDK is C++. No new
  dependencies without discussion; the runtime builds with clang and a handful of official libraries.
- **Style.** Match the file you are editing: its naming, its comment density, its layout. Comments explain
  *why* and record what was measured, not what the next line does.
- **Switches.** Anything experimental or test-only goes behind an option or an environment variable that
  defaults to the old behaviour, with one line of documentation where the other switches are listed.
- **Game-specific knowledge** (an address, a structure layout, a table size) is written with how it was
  found: seen in the binary at an address, taken from a public source, or inferred. An inference is marked
  as one.
- **No silent fakes.** If a system call is answered with a simplification instead of the real behaviour,
  it is registered as such in the audit so that it shows up in the report. Do not make something "work" by
  returning success from a function that does nothing.
- **One change per pull request**, with a description of what it does, how it was tested, and what was not
  tested.

## Testing

Tests that start the game should use the test modes instead of real-time play: speed-up, movie skip, direct
choice of fighters and stage, a fixed end after a number of fight frames. Frame-time measurements are the
exception and run at normal speed.

Two checks carry most of the weight:

- **Lifter and decompiler changes:** the differential tests (`tools/lifter/difftest.py` against the Unicorn
  emulator for instruction translation; the offline and in-match differentials of the fighter SDK for
  decompiled scripts). A function counts as equal when it was compared, not when it compiles.
- **Anything that could change game behaviour:** the deterministic mode with the per-frame state log. Run
  the same scripted fight before and after your change; the two logs must be identical. If they are not,
  the first differing line tells you the frame and the fighter.

Renderer changes additionally need a run under the Vulkan validation layer with no new messages, and — for
anything that can cost time in a fight — frame-time numbers before and after, on the GPU you have.

Say in the pull request which of these you ran.

## Commits and licence

- Write commit messages that say what changed and why. Mention the test you ran.
- By contributing you agree that your contribution is licensed under the project's licence, the GNU General
  Public License version 3 or later (see `LICENSE`), and that you have the right to contribute it.
- Keep personal data out of commits: no local paths with user names, no e-mail addresses in files.

## Conduct

Be direct and be kind. Disagree with the idea, not the person. People arrive here from modding, emulation,
graphics, competitive play and plain curiosity, with very different backgrounds; explain rather than
assume. Harassment, and anything that puts the project at legal risk (requests for game files, links to
them), gets removed.

## Legal

This project is not affiliated with Nintendo, Bandai Namco or Sora Ltd. It contains none of their code or
assets and does not help anyone obtain them. If you are unsure whether something you want to contribute is
acceptable, ask in an issue before sending it.
