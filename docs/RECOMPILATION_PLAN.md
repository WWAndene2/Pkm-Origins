# Recompilation plan

Owner's request (4 November): a recompilation of X/Y and Omega Ruby/Alpha Sapphire, PC first, then Android, as native
programs separate from Pomegrade (no emulator, no launcher in Punica). This plan states what is known, what is estimated
and what is not checked; estimates are marked so.

## 1. The size of the job (measured in Pomegrade's sessions, Omega Ruby 1.4)

- The ExeFS code (`.code`, ARM11, loaded at 0x100000): 5.4 MB decompressed; X's 5.2 MB.
- 146 CRO modules beside it in Omega Ruby (DllField, DllBattle, DllBag...), loaded and linked at run time.
- Ghidra counts about 47,750 functions in Omega Ruby's .code alone (`functions.tsv`, Pomegrade's session tools); about 3,850
  have names so far (Pomegrade `tools/remake/ghidra/function_names.tsv`, most marked "guess").
- Much of it is shared libraries (Nintendo's SDK, the PICA200 graphics library, the C++ runtime): those are replaced by
  our platform layer rather than decompiled (§3).

Estimate (not measured): a full decompilation is years of work for one person even with tooling; the plan is therefore
built so that something runs early and grows (§2).

## 2. The approach: a static recompilation that grows into a decompilation

1. **Static recompilation first.** A tool translates every ARM function of the .code and the CROs into C, mechanically
   (instruction by instruction, the registers as variables), as the N64 recompilations do. The result is ugly but
   complete and runs natively. This gives a running game early, without understanding every function.
2. **The system replaced, not emulated.** The game's calls to the 3DS system (services: files, graphics, input, sound) go
   to our own implementations (`src/hos/`); the GPU's command lists are translated to Vulkan / OpenGL ES (`src/platform/`).
3. **Decompilation by need.** A function someone wants to change or understand is rewritten as readable C, then checked
   to behave as the translated one (§4) before it replaces it. The readable share grows over time: the camera, the field,
   the battle, the menus first (what customising needs).
4. **Customising last**, on matched code, behind options (widescreen, 60 fps, the merge of the two games).

## 3. Components and their order

| Step | What | Done when |
|---|---|---|
| 0 | Repository, rules, this plan | the owner approves it |
| 1 | Loader (`tools/n3ds_extract.py`, done 4 November): reads the player's decrypted .3ds, writes the .code decompressed, the exheader and the CROs outside the repository | done: both games' code byte-identical to Pomegrade's extraction |
| 2 | Translator: ARM11 (ARMv6K + VFPv2) to C for the .code; CRO relocations and imports resolved | the translated .code compiles |
| 3 | HOS layer: the services the game calls at boot (srv, APT, FS, CFG, HID, GSP, DSP stubs) | the translated game reaches its first frame of GPU commands |
| 4 | Renderer: PICA200 command lists and shaders to Vulkan (PC) | the title screen draws |
| 5 | Sound (DSP: the game's own audio lists to SDL audio), input, saves | the field is playable on PC |
| 6 | X/Y the same way (its own translated code; shared libraries shared) | both games run |
| 7 | Android build (same code, ARM64 or x86-64, SDL, Vulkan / GLES) | runs on the owner's Xiaomi 13T |
| 8 | Decompiled modules by need, first the field camera (widescreen) | each matched (§4) |

## 4. How correctness is proven ("matching")

- The reference is the original game running in Azahar, headless (Pomegrade's rig, `tools/remake/headless/`), from the
  same save and the same inputs.
- For a translated or rewritten function: the same inputs give the same outputs and memory writes, compared by a test
  harness that runs both (the original under Azahar's CPU, ours natively).
- For the whole game: frame captures and memory dumps compared at fixed points (the field loaded, a battle started).
- A function is "matched" only after such a comparison; its status is kept in `symbols/` (unknown / named / translated /
  matched).

## 5. What is reused from Pomegrade (linked, not copied twice)

- Function names and their notes: `tools/remake/ghidra/function_names.tsv` (Omega Ruby), to be moved here once this
  project uses them, with Pomegrade then pointing here (one copy only).
- The formats already read (GARC, BCH, CRO, text, scripts): `tools/remake/src/` and `ORAS_ENGINE.md`.
- The headless rig and its tools, for the comparisons of §4.

## 6. Risks and unknowns

- Legal: the repository must stay free of game data (CLAUDE.md §1); the player supplies the games. Keeping the repository
  private until the owner decides otherwise.
- Translating ARM to C statically needs every jump target known. Measured 4 November (`tools/indirect_census.py`, upper
  bounds: data decoding as instructions counts too), Omega Ruby / X .code: 66,121 / 62,592 direct calls; 9,395 / 9,283
  indirect calls (`blx rN`: C++ virtual calls, function pointers); 1,883 / 1,843 indirect jumps (`bx rN` 1,161 in both,
  `ldr pc` 360 / 323, `mov pc` 213, `add pc` 140: switch tables, pointer tail calls). The calls are handled the way the
  N64 recompilations do it: every function entry (Ghidra finds about 47,750 in Omega Ruby) in a lookup table, an indirect
  call looking its target up there (inferred to suffice, not built yet). The jumps need each switch table's bounds read
  (the `add pc` / `ldr pc` ones) or the same lookup. The CROs' imports are resolved at load, as the game's loader does;
  not measured yet. The identical counts of `bx rN`, `mov pc` and `add pc` in the two games suggest shared library code
  (inferred).
- The GPU: PICA200 shaders must be translated; Azahar's renderer is the reference (GPL-2.0: reading it to understand is
  fine, copying code brings its licence; to decide with the owner before any copy).
- Performance on mid-range phones: not measured.

## 7. Distribution and modding (owner's questions, 4 November; a design, nothing built yet)

- **What is distributed: the tool that makes the program, not the program.** The C the translator writes is the game's
  code instruction by instruction: game data (CLAUDE.md §1), never distributed. Distributed: the translator
  (`tools/arm2c.py`, grown), the runtime (`src/hos/`, `src/platform/`: our code) and a bundled compiler (clang or zig,
  one package each). The player gives the path of their decrypted .3ds once; the tool extracts the code, translates it to
  C and compiles it into a program for that copy; the game's assets are read from the copy at run time. C is the
  intermediate format so that one translation serves PC (x86-64) and Android (ARM64).
- **Android without Android Studio.** We build an APK shell once (manifest, a small launcher activity, the runtime as an
  ARM64 `.so`: our code, distributable). The tool compiles the game's C into `libgame.so` with the bundled compiler
  targeting Android (the NDK's clang, or zig), puts it into the shell (an APK is a zip), aligns and signs it with a key
  made on the player's computer: one command gives an APK to install. Open: `apksigner` needs Java (bundle a small
  runtime, or sign in our own tool); not measured.
- **Assets: their own formats, plus a replacement folder.** The program reads the game's GARC / BCH as they are
  (Pomegrade's `tools/remake/` reads them). Mods go in a folder read first, in open formats (PNG textures, glTF models,
  WAV / OGG sounds), like Pomegrade's HD texture packs; edited with ordinary tools (Blender, an image editor), with a
  BCH to glTF converter and back to write. No engine (Godot or other) in the program; a visual editor (maps, placing
  characters) could come later as a separate tool, if wanted.
- **Mods (owner's discussion, 4 November).** Every change to the game (the ideas of `docs/FEATURE_IDEAS.md`, the
  rebalance of `docs/BALANCE_STUDY.md`) is delivered as a mod: a file holding only our changes, no game code. A mod says
  where in the game to act and what to do there: call our function before / after / instead of the instruction at an
  address, change a constant, or replace a whole function once it is decompiled into readable C; the translator applies
  them while it writes the C, so one instruction can be changed without copying anything. Our mods ship inside the tool
  as options the player ticks; others can add their own. Privately, the owner may edit the generated C of their own copy
  freely; the rules only concern what is published.
- Not checked: how the N64 recompilation projects ship their releases; the above follows from CLAUDE.md §1, not from
  their example.

## 8. Next actions

1. Repository created and set up (4 November).
2. Step 1 (loader): done. Step 2's hard part measured (§6).
3. Step 3, the translator's first slice (4 November): our own function list (`tools/function_list.py`, `symbols/`).
   Its flow pass (the code followed from each known start to its return and literal pool; what follows starts the next
   function) brought it to 27,396 entries in Omega Ruby's .code and 26,485 in X's (from 15,617 / 15,072 by calls and
   pointers alone); 1,737 of the 1,743 .code functions Pomegrade names are among them (1,367 before). Ghidra's 47,750 is
   not comparable as is: it counts the modules too (inferred from Pomegrade's linked program), not checked here.
   `tools/arm2c.py` translates leaf functions (no call, no stack, integer only) to C; `tools/match_leaf.py` runs each
   against the original instructions in Unicorn on random registers and memory, 20 trials each. Omega Ruby: 4,746
   translated, **4,156 matched, 0 mismatched**, 590 untestable (they read fixed global addresses outside the test memory).
   X: 4,535 translated, **4,028 matched, 0 mismatched**, 507 untestable. (One mismatch on the way was the matcher's: Unicorn
   stopped by its instruction count inside a long loop and was compared anyway; such a trial now counts as no result.)
   Not translated, first reason: push (a stack frame: 15,452 in Omega Ruby), tail calls (1,814), writeback addressing
   (1,104), no unconditional return (441), loads to pc (423), VFP.
4. Step 4, the code the two games share (owner's request, 4 November: both games are recompiled to be merged; what is
   identical is kept once, what differs stays each game's, Pomegrade's rule of 22 October), done 4 November.
   `tools/cro_functions.py` lists the modules' functions (`symbols/<game>_cro_functions.tsv`: 41,651 in Omega Ruby, of
   which 18,923 import veneers; 2,064 of the 2,088 module functions Pomegrade names found). `tools/shared_functions.py`
   pairs the two games' functions, .code and modules in one pool, calls, literal-pool addresses and relocated words set
   aside, into `symbols/shared_functions.tsv`:
   - Omega Ruby's .code, 27,037 functions (veneers aside): 11,867 identical, 2,414 repeated (the same content the same
     number of times in both games), 501 cut differently, 1,351 near (difflib >= 0.9); 62 % of its bytes paired; 67 have
     their X copy in a module.
   - Omega Ruby's modules, 22,728 functions: 5,226 identical, 1,173 repeated, 1,931 near; 44 % of their bytes paired (35
     of its 145 modules are its own: contests, Delta Episode, Secret Bases...; X has 23 of its own, 110 are in both).
   - 7,082 ambiguous (the same content a different number of times in the two games), not paired; of X's 47,238
     functions, 22,773 have no Omega Ruby copy.
   Checked: samples of each kind open with the same instructions; no X function paired twice; a first run paired 1-word
   functions as "near" across a .code / module boundary, fixed (near needs 4 words, a gap inside one place). Not checked:
   whether an identical pair calls the same functions in both games (call targets are set aside).
   `tools/carry_names.py` carries Pomegrade's names (read where they are, not copied) to X through the pairs:
   `symbols/x_names.tsv`, 1,557 names (1,183 through identical pairs; the kind is kept so a name carried through a near
   pair can be told apart); 2,274 have no pair (Omega Ruby's own code, or changed).
5. Then: the stack (push/pop, writeback, ldm/stm) and calls between translated functions (the lookup table of §6), which
   opens most of the code; then VFP; the untestable functions given the game's real memory (a RAM dump from Azahar).
