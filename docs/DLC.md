# Add-on content (DLC)

The game asks the system which add-on contents are installed and unlocks what they stand for. The runtime answered
"none". It now answers from `config/addon_content.ini`, a list you copy from your own console. Without the file
nothing changes.

## What the game does (measured on 13.0.5; addresses for 13.0.4 from the function list)

- It imports only `nn::aoc::CountAddOnContent` and `nn::aoc::ListAddOnContent`. There is no `MountAddOnContent`: the
  game never reads files of an add-on content. The fighters, their stages and their music are in `data.arc`; the list
  only decides what is unlocked.
- One function asks (a virtual method, `0x710354ca20` in 13.0.4, `0x710354cfd0` in 13.0.5; the same 596-byte function):
  up to 128 indices from offset 0, kept XOR-scrambled with a key taken from the clock. It runs once, early in the boot.
- Six functions answer "is this unlocked", one per kind of id (the tag in the id's top byte), each through a table of
  (id, add-on content index) that the game builds from its data. They are at the same addresses in 13.0.4 and 13.0.5:

| Check function | Tag | Entries | Indices |
|---|---|---|---|
| `0x7100556ad0` | `0xc1` ui_chara (fighters) | 12 | 1 - 12 |
| `0x7100556c40` | `0x69` ui_stage | 11 | 2 - 12 (one stage per fighter pack; none with Piranha Plant) |
| `0x7100556db0` | `0x13` | 59 | 21 - 82 |
| `0x7100556f20` | `0x5a` | 54 | 21 - 82 |
| `0x7100557090` | `0xc4` | 36 | 101 - 136 |
| `0x7100557200` | `0x28` | 18 | 361 - 378 |

The tables were read by hooking the six functions in a diagnostic build (not part of this change). With indices 1 - 12
listed, all twelve fighter ids were answered "unlocked by an add-on content" once the tables were filled; the checks
during the boot, before that, are always "no".

Fighters by index (name hashes `hash40("ui_chara_<name>")` checked for two, the others by position only):

| Index | Title id | Fighter |
|---|---|---|
| 1 | 01006A800016F001 | Piranha Plant (packun) |
| 2 | 01006A800016F002 | Joker (jack) |
| 3 | 01006A800016F003 | Hero (brave) |
| 4 | 01006A800016F004 | Banjo & Kazooie (buddy) |
| 5 | 01006A800016F005 | Terry (dolly) |
| 6 | 01006A800016F006 | Byleth (master) |
| 7 | 01006A800016F007 | Min Min (tantan) |
| 8 | 01006A800016F008 | Steve (pickel) |
| 9 | 01006A800016F009 | Sephiroth (edge) |
| 10 | 01006A800016F00A | Pyra / Mythra (eflame / elight) |
| 11 | 01006A800016F00B | Kazuya (demon) |
| 12 | 01006A800016F00C | Sora (trail) |

Indices 21 and up are the other add-on contents (Mii fighter costumes and the like).

## Using it

`config/addon_content.ini` beside `controllers.ini` (`SSBU_AOC_CONFIG=<file>` names another file; with `SSBU_CONFIG`
set, the one beside the file it names): one title id per line, as your console lists the game's add-on contents (a
dumping tool on the console, nxdumptool for example, shows them with their ids). Text after a space is a note; `;` or
`#` starts a comment. Index = title id - `01006A800016F000`. Lines that are not an add-on content id of this game are
reported and ignored.

```
; add-on contents installed on my console
01006A800016F002 Joker
01006A800016F00A Pyra / Mythra
```

The log says what was read: `nn::aoc: 2 add-on content(s) from ...: 2 10`, or `nn::aoc: no ...: no add-on content`.

List only what your own console has. The runtime cannot check it: like the game data itself, the list is yours to
take from your console.

## Tested

13.0.5 (with the 13.0.5 support change), RTX 5070: with indices 1 - 12 listed, the twelve fighters are selectable, and
Joker, Terry, Sephiroth and Pyra / Mythra were played in matches (Pyra's and Mythra's final smashes show a black box,
a renderer problem of its own). Not tried: the stages and the other add-on contents (indices 21 and up), 13.0.4.
