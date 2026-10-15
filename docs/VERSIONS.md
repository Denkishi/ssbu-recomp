# Game builds: 13.0.4 and 13.0.5

The port runs **13.0.4** and **13.0.5** (update v2031616, released 1 September 2026, which replaced 13.0.4 on consoles;
Nintendo's notes list one change: "Fixed behavior that occurs when invalid data is sent or received in online battles").
Both are built from the same sources. 13.0.4 stays the reference: every game address written in the sources is a 13.0.4
address.

## How a build is recognised

Step `convert` reads the size of `main`'s code (the first segment of the converted dump) and refuses anything else:

| `main` code size | build | converted dump |
|---|---|---|
| 0x39c7e90 | 13.0.4 | `build/binaries/13.0.4/` |
| 0x39c8440 | 13.0.5 | `build/binaries/13.0.5/` |

The build goes into `build/state.json` (`game_version`); every later step and `scripts/run.py` take the folder from there,
and the tools find the dump through `RECOMP_BIN`. A build folder that held the other build runs every later step again.
The runtime recognises the build the same way when it loads `main.elf` and prints it:

```
main: build 13.0.5, text 0x39c8440, data at 0x4f17000, RELA 0x39c9070 / 0x7ad0d8, JMPREL 0x4176148 / 0xc600, SYMTAB 0x4198610, INIT_ARRAY 0x52a8be0 / 0x990
```

## What differs, measured on the binaries

| | 13.0.4 | 13.0.5 |
|---|---|---|
| `main` code | 0x39c7e90 bytes | 0x39c8440 (+0x5b0) |
| `main` read-only data | at 0x39c8000, 0x154df60 bytes | at 0x39c9000, same size |
| `main` data | at 0x4f16000, 0x392570 bytes in the file | at 0x4f17000, same size |
| `main` RELA / JMPREL / SYMTAB / STRTAB / INIT_ARRAY | 0x39c8070 / 0x4175148 / 0x4197610 / 0x41c11f0 / 0x52a7be0 | each +0x1000, same sizes |
| `sdk` | code 0x540950, image 0xcfb000, 25,400 symbols | same layout; the same 1,375 functions lift |
| script modules in `data.arc` | 98 | 98 |

Code: the 13.0.4 function list of the matching decompilation (sbergeron42/ssbu-decomp, `data/functions.csv`, 37,496
functions inside the code) was lined up with 13.0.5's own unwind table (`.eh_frame_hdr`, 110,842 entries) by function size.
Code is at the same address up to about 0x710236e830, apart from a small region near 0x71016ab000..0x71018cc000 that moved
by -0x1a0 / -0x40; after that it moves by +0x450, +0x490, +0x590 and finally +0x5b0 to the end. 26,897 functions match by
size under these offsets; most of the rest are entries whose Ghidra boundaries are approximate. Read-only data, data and
bss moved by exactly one page (+0x1000).

## How the sources handle both

- **The layout of `main`** is read from `main.elf` instead of being written down: the lifter (`tools/lifter/recomp.py`,
  `_layout`) and the runtime (`runtime/hostrt.c`, `load_layout` / `load_dynamic`) read the segments from the program
  headers and the relocation tables, symbol table and init array from the dynamic section that MOD0 points to. The
  constants this replaces are in the comments next to that code.
- **Game addresses** are written as their 13.0.4 value and passed through `GA()` (`runtime/hostrt.h`), which gives the
  address in the build that runs. On 13.0.4 it returns its argument. On 13.0.5 an address in read-only data, data or bss
  moves by one page, and an address in code is looked up in `g_code_13_0_5` (`runtime/hostrt.c`), one line per
  address the runtime uses. A code address that is not in that table stops the program with a message: calling the wrong
  function silently would be worse. So does `GA()` before `main.elf` has been read.
- `main`'s layout fields (`g_main`) replace the constants that described it: the init array in `boot.c` and in the C++
  runtime self-test, the start of the data segment in the image-protection check of `nnmisc.c`.

## Checking the table on your own dump

```
python tools/port/verify_13_0_5.py build/binaries/13.0.5/main.elf <functions.csv>
```

`<functions.csv>` is `data/functions.csv` of sbergeron42/ssbu-decomp (addresses, sizes and names; no code). The script
finds every game address in `runtime/` and `fighter_sdk/` and checks what `GA()` answers on 13.0.5: a code address must
be in `g_code_13_0_5`, and its delta must be the one at which the same-size functions on both sides of it (or its own
function) line up in the 13.0.5 unwind table; a data address must be formed by 13.0.5 code (ADRP + ADD/LDR/STR) more
often at its new address than at its old one. It also checks the one-page rule against the segments, and that no
`0x71xxxxxxxx` literal in code bypasses `GA()` without a stated reason. On the author's dump it reports
`all checks passed` (46 code and data addresses, 3 segment checks, 7 literals that are not addresses of things that move).

## Adding a game address

Write the 13.0.4 address and wrap it: `GA(0x71xxxxxxxxull)`. If it is in code, add its 13.0.5 counterpart to
`g_code_13_0_5` (sorted) and run the check above. An address only known on 13.0.5 cannot be written this way; find its
13.0.4 counterpart with the same method, or say so in the pull request.

## Not covered

- `tools/lifter/patches/kinds.txt` (new fighter kinds, off by default) holds instruction patches at 13.0.4 addresses.
  Every patch states the original instruction and `recomp.py` refuses a patch the binary does not match, so on 13.0.5 the
  lift stops instead of patching the wrong instruction; `kinds_scan.py` would have to be run on the 13.0.5 binary.
- `fighter_sdk/tools/`, `tools/audit/` and the scripts under `tests/` still name `binaries/13.0.4` and 13.0.4 constants:
  they are the author's analysis and test drivers, not part of the build.
- A mod DLL that hooks a function of `main` by its address is written for one build.
- Replays: Nintendo's notes say replays from 9.0.0 to 13.0.4 may have compatibility issues in 13.0.5.
