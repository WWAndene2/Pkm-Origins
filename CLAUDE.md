# Project Rules — Pkm Origins

Pkm Origins: a recompilation of Pokémon X/Y and Omega Ruby/Alpha Sapphire (Nintendo 3DS) into native programs, PC and
Android together (the owner tests on Android), with no emulator; no game data in the repository (§1). The plan:
`docs/RECOMPILATION_PLAN.md`; the features wanted afterwards: `docs/FEATURE_IDEAS.md`.

Hard rules for any change. They follow Pomegrade's CLAUDE.md (verification, scope, naming, documentation homes), adapted to
a decompilation.

## 1. Game data never enters the repository

- Never commit, upload or put in an artifact the owner's ROMs, code dumps, saves, keys or anything extracted from them
  (models, textures, sounds, texts, tables copied byte for byte). `.gitignore` covers the usual files; check every diff.
- The game's files are read at run time from the player's copy (a path given by the player).
- Function and variable names, structure layouts and format descriptions are ours and may be committed.

## 2. Faithful first, customised after

- A function is first decompiled to behave exactly like the original: same results for the same inputs. Proven by
  comparison against the original running in Azahar (the "matching" check, docs/RECOMPILATION_PLAN.md §4), never assumed.
- What X/Y and ORAS share is one copy; what differs stays each game's own, ported whole (Pomegrade's rule of 22 October:
  100 % identical = merged, not identical = ported, never a substitute).
- Changes to gameplay or look (widescreen, 60 fps, new features) come only after, behind options, on code already matched.

## 3. Verification

- Verify before and after every fix with real execution (a test, a comparison run, measured output). State what was not
  checked.
- Ask before runs longer than 15 minutes; one run at a time.
- Short answers to the owner, in French.

## 4. Structure

| Purpose | Location |
|---|---|
| Decompiled game code (C/C++), one folder per original module (`code/` for the ExeFS .code, `cro/<Dll name>/` for the CRO modules) | `src/game/` |
| The platform layer: window, input, sound, files, the GPU renderer (PICA200 command lists to Vulkan / OpenGL ES) | `src/platform/` |
| The 3DS system services the game calls (FS, GSP, HID, APT, CFG...), reimplemented | `src/hos/` |
| Tools: binary splitting, symbol lists, matching checks | `tools/` |
| Symbol names (address, name, status) for each game version | `symbols/` |
| Tests | `tests/` |
| Documentation | Root: `CLAUDE.md` only (no README, owner's decision of 4 November); the rest in `docs/`: `RECOMPILATION_PLAN.md` (the plan and its progress), `FEATURE_IDEAS.md` (the owner's checklist of features for after the faithful recompilation, in French, edited by the owner), `BALANCE_STUDY.md` (the balance study behind the "Rééquilibré" option: Smogon usage data and game design, in French), `AGENT_HANDOFF.md` (the state of the work and how to carry it on, for the next agent: kept current at each hand-off) |

Names: C/C++ files in PascalCase after their subject (`FieldCamera.cpp`); a decompiled function keeps the name given in
`symbols/` (Pomegrade's `function_names.tsv` style: `Subsystem_Action`). No catch-all `misc/` or `utils/` folder.

Pomegrade (the owner's other project) is a separate project: it may be used (its Azahar as the reference, its headless
runner, its knowledge of the games), but its code and files are not brought into Pkm Origins as a habit; when something
of it is needed here, ask, then make it Pkm Origins' own (owner's rule, 6 November).

## 5. Commits

Commit and push the branch; merge only when the owner asks. Each commit one intent, its message saying why.
