# Recompilation plan

Owner's request (4 November): a recompilation of X/Y and Omega Ruby/Alpha Sapphire, PC and Android together (§7), as native
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
| 4 | Renderer: PICA200 command lists and shaders to Vulkan (PC and Android), and the Android app compiling the game on the phone (§7) | the title screen draws on the owner's phone |
| 5 | Sound (DSP: the game's own audio lists to SDL audio), input, saves | the field is playable |
| 6 | X/Y the same way (its own translated code; shared libraries shared) | both games run |
| 7 | (merged into 4: Android from the start, owner's decision of 4 November) | |
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
- **Android: compiled on the phone (owner's choice, 4 November).** GitHub Actions builds the Pkm Origins app from our
  code only (runtime, translator, a compiler for ARM64, the interface): no game data, so it may be an artifact, as
  Pomegrade's are. On the phone, the app reads the player's .3ds, translates its code and compiles it there, once
  (minutes on the owner's Xiaomi 13T: estimated, not measured), then runs it: the game's code never leaves the phone,
  no PC needed. Building the whole APK in a session and sending it was the other way, set aside: it would put game code
  in a file sent out (CLAUDE.md §1). Open: which compiler runs inside an Android app (clang from the NDK built for
  Android, or zig, or a smaller C compiler such as TinyCC, slower code): to measure.
- **PC and Android together, Android for the owner's tests (owner's decision, 4 November; the owner has no PC).** One
  code base for both (SDL for window, sound, input; Vulkan for drawing). The checks of the translation (matching) keep
  running in the cloud sessions, on Linux; the first things to see (a first frame, the title screen) are tried on the
  owner's phone; the PC build comes from the same code.
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

## 8. Mali target (owner's request, 4 November; a design, nothing built or measured yet)

The owner's phone, the first test device: Xiaomi 13T (MediaTek Dimensity 8200-Ultra: 4 Cortex-A78 + 4 Cortex-A55;
Mali-G610 MC6, driver r54p1, Vulkan 1.3). Source of what is known about it: Pomegrade's `docs/Eden_Optimisation_Xiaomi.md`
(the Switch core's study on that phone, measured there). What carries over, since here the game is native code and the
renderer ours, with none of an emulator's costs (decoding the console's GPU commands, tracking guest memory):
- **CPU.** The game's C compiled on the phone for its own cores (`-mcpu=cortex-a78`, and the A55 tuning where both run);
  the game's threads on the large cores, Android's performance hints (ADPF) for them: Pomegrade's O1-O3, measured on
  the 13T with Eden (that study, §5.1).
- **A tiler's rules for the renderer.** Mali draws the screen tile by tile in on-chip memory; its limit is memory
  bandwidth (shared with the CPU), not shader power. So: render passes that never load or store what they need not
  (load / store ops: clear or don't-care, transient attachments for depth), the 3DS's two screens and their effects in
  as few passes as possible (subpasses rather than full-screen passes that read the image back), no readback of a frame
  in progress, and never the CPU waiting on the GPU in a frame (Eden's main fault there: `Scheduler::Finish` in hot
  paths, that study §2.4).
- **Shaders.** The PICA200's shaders translated to SPIR-V once, when the game is compiled on the phone, and the
  pipelines built then and cached (with the driver's pipeline cache): no compile stutter in play.
- **Textures.** ASTC is native on Mali: a replacement pack (§7) in ASTC is read as is; the game's own formats are
  converted once.
- **Headroom (estimated, not measured).** The 3DS draws 400 x 240 (top) and 320 x 240 (bottom), with few polygons by
  a phone's standards: even at 4 times the resolution the G610 should have a wide margin. To be measured on the phone
  once the title screen draws (step 4).

**"Mali Boost" profile (owner's request: everything at maximum on the Mali).** An option, the faithful look staying the
default (CLAUDE.md §2), chosen for what a tiler does cheaply:
- internal resolution up to the screen's (the 13T's 2712 x 1220), or a fixed multiple of the 3DS's;
- MSAA 4x: resolved inside the tile on Mali, nearly free in bandwidth (unlike on desktop GPUs), so preferred over post
  anti-aliasing passes;
- anisotropic filtering 16x, higher-detail textures from a replacement pack (ASTC);
- 60 fps with the game's logic kept at its own rate (interpolated animation: `docs/FEATURE_IDEAS.md`, technical ideas);
- dynamic shadows, ambient occlusion, bloom only where they fit inside the tile pass; each effect a separate switch, so
  a slower phone turns off what costs most.
What the 13T sustains in each setting (heat, frame time over 30 minutes, as Pomegrade's study measured Eden) is to be
measured, not assumed.

## 9. True seamless, no lag (owner's request, 4 November: the main goal after faithfulness; a design, nothing measured)

Performance comes first in every design choice, the more so for the seamless co-op and online ideas
(`docs/FEATURE_IDEAS.md`), where a stall on one player's phone stalls the other's game.
- **Tier 1, nothing changed in the game: loads made almost instant.** The 3DS's loads are its cartridge's speed, the
  decompression and its 256 MB of memory. Here the HOS layer's file service (`src/hos/`) answers from memory: the
  archives of the current region read and decompressed once, ahead, on a background thread (the 13T has 8 GB); a file
  the game asks for is then a copy, not a read. Measured once the game runs: the time of every load, before and after.
- **Tier 2, the waits the game imposes removed (mods).** What remains are the game's own fades, doors and transition
  screens that wait a fixed time: code to change, function by function, once those parts are read (Pomegrade's
  `tools/remake/ORAS_ENGINE.md` already covers the zone change).
- **Tier 3, one continuous world.** The next zone loaded in the background while the player walks, a town to a route,
  Kalos to Hoenn, with no cut: deep work on how the game handles its zones (the "background loading" and "Kalos and
  Hoenn linked" ideas, rated 2).
- **No lag: rules for the runtime from the start.** Nothing blocks the frame: file access, decompression and shader /
  pipeline building on background threads, never in the frame (shaders built when the game is compiled on the phone,
  §8); no CPU wait on the GPU; memory set up before play, no allocation spikes in a frame. A frame budget checked by
  measurement, not assumed: frame time and its worst cases (the longest frame of each minute) logged on the phone, as
  Pomegrade's study logged Eden's.
- **Co-op and online.** The same rules on both sides, plus what multiplayer adds: each player's world streamed on their
  own phone, the game's state exchanged rather than waited for (rollback netcode, in the technical ideas), so a slow
  connection never freezes a frame. Designed when the game runs alone without a stall, not before.

## 10. Next actions

1. Repository created and set up (4 November).
2. Step 1 (loader): done. Step 2's hard part measured (§6).
3. Step 3, the translator's first slice (4 November). The function list (`tools/function_list.py`, `symbols/`): starts
   from calls in walked code, pointers, the modules' imports, then a flow pass (each function followed to its end, its
   pools and switch tables read, ARM and Thumb told apart and each start checked to decode as code). 26,109 functions
   in Omega Ruby's .code (509 Thumb), 25,350 in X's; the modules' lists (`tools/cro_functions.py`): 41,998 and 38,567.
   Checked: all 3,831 functions Pomegrade names are starts; against Ghidra's own analysis (`tools/ghidra_compare.py`,
   Pomegrade's session_setup.sh project), Ghidra has 18,354 in Omega Ruby's .code and 29,396 in its modules, all but 37
   and 11 also ours, and each of those explained: nop entries a reference reaches (ours open on the nop), Ghidra
   cutting a function at a branch target of it, two rodata words Ghidra read as code, one cut in running code (X the
   same: 34 and 9); every function of ours only (8,254 in the .code: mostly methods reached through vtables Ghidra did
   not make functions) decodes whole as valid ARMv6K, but for trap functions (udf) and data placed in the code after an
   unconditional branch. The comparison found and fixed: switch cases split off as functions (441), Thumb read as ARM
   and back, data tables taken for functions, the modules' relocated code pointers, a module known by its file name
   (DllEshop.cro is DllEShop).
   `tools/arm2c.py` translates leaf functions (no call, no stack, integer only) to C; `tools/match_leaf.py` (now `tools/match_functions.py`) ran each
   against the original instructions in Unicorn on random registers and memory, 20 trials each. Omega Ruby: 4,596
   translated, **4,000 matched, 0 mismatched**, 596 untestable (they read fixed global addresses outside the test memory).
   X: 4,378 translated, **3,875 matched, 0 mismatched**, 503 untestable. (A mismatch on the way was the matcher's:
   Unicorn stopped by its instruction count inside a long loop; such a trial now counts as no result.)
   Not translated, first reason: push (a stack frame: 15,599 in Omega Ruby), tail calls (1,346), writeback addressing
   (983), Thumb (507), no unconditional return (501), loads to pc (418), VFP.
4. Step 4, the code the two games share (owner's request, 4 November: both games are recompiled to be merged; what is
   identical is kept once, what differs stays each game's, Pomegrade's rule of 22 October), done 4 November.
   `tools/shared_functions.py` pairs the two games' functions, .code and modules in one pool (addresses, calls and
   relocated words set aside, import veneers left out), into `symbols/shared_functions.tsv`: identical bodies; copies
   of a repeated body and changed functions paired by their callers' votes ("repeated", "call": bodies at least half
   alike); near pairs (difflib >= 0.9) only where no call decides.
   - Omega Ruby's .code, 25,750 functions (veneers aside): 12,155 identical, 1,513 repeated, 2,178 by call, 826 near;
     3 cut differently by the two lists; 78 % of its bytes paired.
   - Its modules, 23,075 functions: 5,270 identical, 175 repeated, 1,057 by call, 1,505 near, 1 cut differently; 48 % paired (35 of its
     145 modules are its own: contests, Delta Episode, Secret Bases...; X has 23 of its own, 110 are in both).
   - 9,792 copies of a body no caller tells apart, not paired; of X's 46,479 functions, 21,796 have no Omega Ruby copy.
   Calls checked, every pair, slot by slot: 98,828 lead to the paired function (16 of them to another copy of the same
   body: Omega Ruby's 003CA31C calls the .code copy, X's the DllField one), 2,324 to a function with no copy in the
   other game, 16 contradict: real differences, the same body calling different functions (Omega Ruby's 00355EB8 calls
   001397B8, X's copy a function of another body), marked "-body" (13 pairs). Pairs resting on likeness or one call
   whose calls contradicted were removed (106), so every pair left agrees with the others.
   `tools/carry_names.py` carries Pomegrade's names (read where they are, not copied) to X through the pairs:
   `symbols/x_names.tsv`, 1,769 names (1,227 through identical pairs; the kind is kept); 2,062 have no pair.
5. Step 5, begun 4 November: the stack and calls. `tools/arm2c.py` now also translates push / pop, ldm / stm,
   writeback addressing, ldrd / strd, direct calls and tail calls to translated functions, indirect calls and jumps
   (blx / bx rN) through `call_indirect` (a lookup of the translated functions by address: the table of §6), switches
   (ldr pc, [pc, rN] as C switches), shifts by a register; a function falling into the next one calls it; a stack
   switch (sp loaded from a register, as a saved-context restore) is refused. Each function gets the address it returns
   to, so a pop {pc} loading another one is caught. `tools/match_functions.py` (was match_leaf.py) runs the same stack
   and return address on both sides and compares sp too; a function counts only with all its callees translated.
   Without the game's memory: Omega Ruby **11,358 of 26,109 translated (4,155 calling others), 6,320 matched, 0
   mismatched**, 5,038 with no valid trial; X 11,180 of 25,350, 6,044 matched, 0 mismatched, 5,136 with none.
   With it (`--ram`: RAM dumps of each game on the field from the owner's saves, Pomegrade's headless Azahar, its core
   built in this session's work folder in 20 min; writes logged on both sides, compared, undone; registers may point into
   the game's heap) and the words the module loader rewrites read when run (`--cro`): Omega Ruby **8,311 matched, 0
   mismatched**, 3,047 with no valid trial; X **8,226 matched, 0 mismatched**, 2,954 (32 min each, both at once).
   The game's memory caught three faults of the translator, fixed: two instructions taken for data (a pool word that
   read as a pc-relative ldrd: data words now only from code), the module loader's rewritten words baked in as constants
   (367 in Omega Ruby's text), and a return to lr not checked (a shared epilogue popping lr: every return and tail call
   now faults when lr is not the address the function got). The trials still with no result mostly call indirectly a
   function not translated yet. Not translated, first reason: callees not translated (7,867 in Omega Ruby), VFP
   (vpush 1,219, vldr 1,016, vldmia), blx to Thumb, mrc (cp15: the thread-local pointer), svc (system calls: the HOS
   layer, step 3 of §3), ldrex / strex, mla.
   VFP added (4 November), as the game runs it: FPSCR 0x03C00010, the value Azahar's kernel gives every thread (default
   NaN, flush to zero, round toward zero); host float / double rounding toward zero, the flush and the default NaN added
   by the C, multiply-accumulate not fused; the trials also set random VFP registers (with edge values) and compare them
   and FPSCR's flags. Omega Ruby **13,133 translated, 10,836 matched, 0 mismatched**, 2,297 with no valid trial; X
   **12,898 translated, 10,633 matched, 0 mismatched**, 2,265. match_functions.py now draws its random memories once
   and runs on every CPU: 10 min a game instead of 32. Not translated now, by the functions each reason holds alone
   (itself and its callers): blx to Thumb 1,184 (Omega Ruby), writes to pc 594, mrc 561, ldrex / strex 520, Thumb 507,
   loads to pc 489, branches out to no start 459, svc 410, pld 403, mla 298.
   Thumb added (4 November): Thumb-1 and its bl / blx pair (ARMv6K has no other Thumb-2), pc read as + 4 (word-aligned
   for literal loads and adr), calls between the two sets with lr's bit 0 as the CPU sets it, call_indirect finding a
   Thumb function by its odd address; with it adc / sbc / rsc and muls. The trials now fault a write to the program's
   text and read-only data (`--exheader`), read-only on the 3DS: X's 00507F50 on a random index wrote its own code,
   which the game cannot do. Omega Ruby **13,880 translated, 11,428 matched, 0 mismatched** (Thumb: 309 of 507
   translated, 265 matched; 480 translate on their own, the others call a function not translated yet), 2,452 with no
   valid trial; X **13,666 translated, 11,252 matched, 0 mismatched** (Thumb 308 of 506, 260 matched), 2,414.
   Now first, by the functions each reason holds alone (Omega Ruby): writes to pc 596, mrc 566, ldrex / strex 538,
   branches out to no start 534, loads to pc 490, pld 454, svc 431, mla 313, umull 297.
   mrc of the thread-local storage pointer (c13, c0, 3: the runtime gives each thread its own), the ARMv6 barriers (mcr
   c7, c10, 4 / 5: nothing for one thread's C) and ldrex / strex / clrex on an exclusive monitor added (4 November): Omega
   Ruby **14,070 translated, 11,565 matched, 0 mismatched**, 2,505 with no valid trial; X **13,854, 11,388, 0
   mismatched**, 2,466. svc now holds 1,526 functions alone (most read the TLS for their IPC buffer, then call the
   kernel), then writes to pc 596, branches out to no start 538, loads to pc 490, pld 454, mla 313, umull 310.
   Tools: match_functions.py --uses REGEX tests only the functions with such an instruction (and their callees: a new
   translation checked in seconds), tools/match_game.sh runs a whole game in one command, arm2c.py --functions prints a
   function's C as the check translates it.
   svc added (4 November): `svc #n` calls `hos_svc(c, n)`, the kernel's call in `src/hos/`; the trials stand it in on
   both sides by the same mixing of n and r0-r7 into r0-r3 (checked sensitive: a stand-in off by one on the C side, 445
   of 523 mismatch). Omega Ruby **15,220 translated, 13,399 matched, 0 mismatched**, 1,821 with no valid trial; X
   **15,004, 13,196, 0 mismatched**, 1,808.
   What `src/hos/` must answer, measured: tools/svc_inventory.py (static: 33 svc numbers in Omega Ruby's code and
   modules, 34 in X, all known to Azahar; 256 IPC headers) and tools/azahar_trace_run.sh + hos_calls.py (every call a
   headless Azahar run makes, counted at Pomegrade's code-trace hooks, none lost). From boot to the field: Omega Ruby
   534,116 calls, X 136,942 (X with Pomegrade's `draw every 15`: full drawing ran it at 1 frame a second), 23 kernel
   calls and 101 service requests between them. Most: WaitSynchronization1, ArbitrateAddress, ClearEvent, SignalEvent,
   GetSystemTick, SendSyncRequest; gsp::Gpu TriggerCmdReqQueue (86,312 in Omega Ruby), file reads (2,263), FlushDataCache.
   Services: srv, APT (A in Omega Ruby, U in X), fs:USER and its file sessions, cfg:u, gsp::Gpu, dsp::DSP, hid:USER (its
   handles only: input comes by shared memory), ldr:ro, frd:u, boss:U, ndm:u, ptm:u; X also y2r:u (its intro video) and
   nwm::UDS (the Player Search System); Omega Ruby's nwm::UDS comes later (a 14-minute run had it).
   Then: `src/hos/` from that list, the kernel's objects first (threads, events, mutexes, the address arbiter: most of
   the calls), then srv, fs, gsp; the jumps through pc; pld, the multiplies.
