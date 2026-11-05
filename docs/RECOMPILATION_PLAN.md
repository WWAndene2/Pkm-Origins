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
| 4 | Renderer: the game's graphics library (its draw calls: models, materials, textures) reimplemented on Vulkan, PC and Android; no PICA200 command list is made or read (owner's decision, 4 November: no translation of the 3DS's GPU commands, even as a stopgap); and the Android app compiling the game on the phone (§7) | the title screen draws on the owner's phone |
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
- **Shaders.** Ours, written for the game's materials (step 4 reimplements its graphics library: no PICA200 shader is
  translated); every pipeline the game's materials need built before play and kept in the driver's pipeline cache: no
  compile stutter in play.
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

### 8.1 GPU study: what each family needs (4 November; read, not measured)

Marks: **read** (the source named), **seen** (Pomegrade's Eden study, measured on the 13T), **inferred**, **unknown**.
Sources: Khronos Vulkan-Samples' performance samples, written by Arm, their numbers mostly on Mali-G72 / G76
(github.com/KhronosGroup/Vulkan-Samples, samples/performance/*/README.adoc); Arm's mobile FAQ and memory limits
(github.com/ARM-software/vulkan_best_practice_for_mobile_developers, docs/faq.md, docs/memory_limits.md); Mesa's
Adreno driver documentation (docs.mesa3d.org/drivers/freedreno.html and freedreno/hw/lrz.html); Android's frame pacing
page (developer.android.com/topic/performance/frame-pacing). Arm's own Mali guide (developer.arm.com 101897) and
Qualcomm's Adreno guide could not be read (pages drawn by script, or gone): not used.

**Every tiler (Mali, Adreno in binned mode, PowerVR):** read, Vulkan-Samples.
- Load / store: CLEAR or DONT_CARE, never LOAD unless the previous image is needed; DONT_CARE for what is not kept
  (depth). Measured there: about 600 MiB/s of reads and 555 MiB/s of writes saved at 2220 x 1080. Clears in loadOp, not
  vkCmdClear* inside the pass.
- Depth and MSAA images TRANSIENT with LAZILY_ALLOCATED memory: they live in the tile only. MSAA 4x resolved at the
  pass's end (pResolveAttachments): about 3 % more bandwidth on Mali-G76, against about 5 GB/s and 500 mW for a separate
  resolve. So MSAA 4x is the anti-aliasing for phones; more than 4x only after measuring.
- Effects that read the frame back: subpasses in one render pass when the reads are per pixel (Mali: about 55 % fewer
  tiles written, G76; merge limits 128 bits a pixel of colour, 8 attachments, same depth and samples).
- Barriers as narrow as possible (COLOR_ATTACHMENT_OUTPUT to FRAGMENT_SHADER between passes; never ALL_COMMANDS):
  Mali runs the vertex work of one pass beside the fragment work of the one before (13 % frame time, measured there).
- Swapchain pre-rotated (preTransform = currentTransform, our matrix turned): without it, memory stalls doubled on a
  Mali-G72 phone. FIFO with three images (two dropped a G72 to 30 fps there); never vkDeviceWaitIdle in a frame (fences:
  72 to 56 ms).
- 16-bit vertex outputs and fragment inputs where exact enough (VK_KHR_16bit_storage), positions relative to the camera;
  fp16 arithmetic only where measured (the sample itself says a gain is hard to guarantee).
- Pipelines all made ahead, VkPipelineCache saved between runs (G76: 50 to 24 ms a re-creation); specialization
  constants rather than branches on uniforms (Mali G76: 4.4 %).
- Descriptor sets cached and reused, one pool per frame reset whole, one large buffer per frame with dynamic offsets
  (38 % CPU frame time on a 2019 phone); command pools reset whole.

**Mali (Arm):** read, unless marked.
- Bandwidth is the limit (shared with the CPU): the rules above first. AFBC (Mali's own framebuffer compression, 33 %
  less bandwidth measured) stays on only if images avoid STORAGE usage, MUTABLE_FORMAT and ALIAS flags, and copies that
  undo it; driver r16p0+, Mali-G51+ (the 13T: r54p1, G610).
- Constant data: static descriptor sets preferred on Mali, push constants about 128 bytes at most; no dynamic indexing
  by gl_InstanceIndex (it turns off Mali's pre-run of shader constants).
- Geometry: a fixed 180 MB region holds all vertex output a frame; past it, device lost. Index ranges matter (memory is
  set aside for every index between a draw's min and max). The 3DS's scenes are far below (inferred).
- No geometry streams on Mali (seen: Eden fault on the G610); no geometry or tessellation shaders in our renderer.
- What the 5th generation (G720 and later: vertex shading deferred) changes: unknown, no source read.

**Adreno (Qualcomm):** read, Mesa.
- Draws either by tiles in on-chip GMEM (128 KB to 1 MB) or straight to memory (sysmem); the driver picks per render
  pass (Mesa estimates the bandwidth of each; small passes go to memory). Qualcomm's own driver's rule: unknown.
- LRZ (its early depth reject, like a free depth prepass): lost for a draw that needs late depth, for a draw whose
  shader discards (in binning), and, before Adreno 650, for the rest of the pass once the depth comparison changes
  direction. So: one depth direction per pass, few discarding materials, opaque before transparent (inferred).

**PowerVR, Xclipse (Samsung, AMD design), desktop GPUs:** nothing read yet; the general rules hold (inferred). Desktop
GPUs draw to memory directly: the tile rules cost them nothing, so one renderer serves both (inferred).

**Frame pacing on Android:** read. Swappy (Android's frame pacing library) presents each frame at its time
(VK_GOOGLE_display_timing) and holds the CPU back with fences so frames do not queue (the source of irregular frames);
starting setup: fixed interval at the display's rate, no auto modes. Game and render threads apart; the device's
thermal status watched.

**To measure on the 13T once something draws:** each rule's gain (Arm's malioc for shaders, Perfetto and the GPU
counters for bandwidth), heat over 30 minutes, and every pipeline built before play (none compiled in a frame).

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
   **The whole .code translated (4 November)**: every instruction the two games' code uses has its C (the multiplies,
   saturating and packing instructions, the codec's parallel ones, clz, rev, msr / mrs of the flags, computed jumps as
   switches, the import veneers, shared tails as entries of their own); an instruction with no C (data the walk took
   for code, nearly always) is a stop (fault 10), never a guess. Omega Ruby: **26,214 translated (all 26,109 listed and
   105 entries), 19,506 matched, 0 mismatched**, 6,708 with no valid trial; X **25,449 (all 25,350 and 99), 18,633
   matched, 0 mismatched**, 6,816 (100 trials each). **It compiles** (tools/build_game.py, outside the repository: the C
   holds the game's constants): Omega Ruby's 45 MB library, X's 43 MB, no stub; the only symbols they need are the
   runtime's (hos_memory, hos_svc) and the C library's.
   **The modules translated (4 November)**: their C writes the module's addresses as B_<module> + offset (the base the
   game maps it at, through ldr:ro LoadCRO). All 41,998 functions of Omega Ruby's 145 modules translate (42,189 with
   entries). Checked on a dump with the modules loaded on the field (tools/azahar_trace_preload.c records where each is
   mapped; a loaded module's header reads FIXD, and the loader changed only words its relocation tables list, 452 of
   452 in DllTitle's code), with the .code compiled beside them (--with-code): DllSequence 1,058 matched, DllTitle 411,
   DllBackGround 138, DllFieldDemo 113, **0 mismatched**; every module mapped as its file (--modules all): 9,217 matched,
   0 mismatched, 32,972 with no valid trial (unlinked, their pointers lead nowhere: the modules need linking, ldr:ro's
   work, to be tested whole). The modules' tests found a fault in the translator, fixed: ldr pc, [pc, rN, lsl #2] whose
   table the loader writes was taken for an import veneer (its index ignored).
   Then: the CRO linker (ldr:ro, in C), checked against the modules Azahar loaded, then every module tested linked;
   `src/hos/` from the list above (begun: Memory.c, Kernel.c, Thread.c, Sync.c: the handles, the scheduler, the waits,
   events, mutexes, semaphores, timers and the address arbiter as Azahar has them; tools/svc_abi.py gives each system
   call's registers from Azahar's declarations).
6. **Steps 2 and 3 done (4 November).** The CRO linker in C (`src/hos/Cro.c`, as Azahar's ldr:ro LoadCRO): rebase,
   relocations, imports and exports; checked word for word against the modules Azahar loaded (`tools/cro_check.c`: 0
   words differ), and all modules linked together (`tools/cro_link.c`: 145 in Omega Ruby, 133 in X, 0 failed). Final
   check (200 trials for the .code, 100 for the linked modules; each trial's FPSCR either the kernel's or the game's,
   0x03000000): Omega Ruby .code 26,177 translated, 19,965 matched, **0 mismatched**; X .code 25,422, 19,061, **0**;
   Omega Ruby linked modules 42,169, 25,531, **0**; X's 38,684, 22,913, **0**. Not checked: the functions with no valid
   trial (6,212, 6,361, 16,638, 15,771), mostly ones random inputs never run to the end (inferred, not measured).
   Step 3: `src/hos/` (kernel, threads, loader, IPC, srv, APT, ndm, cfg, ptm, frd, boss, fs (RomFS), hid, gsp with a
   null GPU) runs Omega Ruby's translated code natively (`game <extracted dir> <.3ds>`) to its **first GPU command list**
   (29,824 bytes submitted); its main thread's first 388 system calls equal Azahar's, in order
   (`tools/hos_trace_compare.py`). It then asks for dsp::DSP (step 5). Not done: X run the same way; input; speed not
   measured (it can be once the title screen draws, step 4). Step 4 (the renderer) waits for the owner's discussion.
7. **Step 4 begun (4 November): finding the graphics library.** `tools/gpu_library.py` marks the functions whose literal
   pools hold PICA200 command headers (3 or more of the GPU's registers) and walks their callers up. Omega Ruby's .code:
   37 such writers, then 84, 83 and 45 functions one, two and three calls above; spread over the whole .code
   (0x106000-0x56D000), not one block. Only 6 of them already have a Pomegrade name (`Gfx_BuildCommandList` 0x33899C,
   `Gfx_BuildMaterialCommandLists` 0x397CAC, `Gfx_InitAnimChannelBuffer` 0x3800C0...). Not yet found: the code that
   builds commands from registers or tables (not pool words), the modules' part, and X. Inferred, to check: the models'
   files (BCH) hold their materials and meshes as ready-made command lists, which the reimplemented library would turn
   into Vulkan state once, when a model loads, not in every frame.
   **5 November, the GPU trace** (`build_game.py --gpu-trace`, src/hos/GpuTrace.c: each word of every command list the
   game submits credited to the function that stored it, with its three callers): Omega Ruby now boots to its title
   modules (DllSequence, DllBackGround, DllFieldDemo, DllTitle, as in Azahar) and submits about 300 command lists a
   second (35,840 in 120 s, never stopping; dsp::DSP silent, the system archives, ldr:ro, the save data done for it).
   92.8 % of the words are written by one function, 0x144D28, a memcpy (ldm / stm by 32 bytes), called from
   0x337F6C (41.7 %), 0x300B28 (27.7 %), 0x32430C (10.4 %), 0x3388F0 (5.9 %): the command lists are mostly copied
   from ready-made buffers, which supports the inference above (the models' files hold them). The callers sit in
   0x300000-0x33A000, beside Pomegrade's `Gfx_BuildCommandList` (0x33899C): the library's core. Next: the trace
   to look through the copies (credit the source buffer, not memcpy), and the same on X.
   Through the copies (memcpy 0x144D28 and 0x300B28, HOS_GPU_TRACE_COPY), 60 s at the title: 64.1 % of the words are
   written by code, 28.8 % copied from the program's own initialised data, 7.1 % read from one RomFS file (0x3C4
   bytes at RomFS offset 0x5B71C0F0, a ready-made command list). The largest writer is `Gfx_BuildCommandList`
   (0x33899C, 29.7 %, called by `Gfx_UpdateDrawState` 0x326FA8), with `Gfx_FlushLightAndMatrixCommands` (0x33917C,
   9.6 % written or copied); 16.6 % are copied by 0x1499BC from the data at 0x5A4F4C-0x5B0B48 (templates). So the
   inference above holds only in part at the title: most of the lists are built by code each frame, the models' data
   among the inputs. Not yet looked at: which file the RomFS offset is, what 0x1499BC and 0x14AFA8 draw.
   **X the same way** (5 November; a new save: the owner's X save has no format file): it boots and submits command
   lists without stopping (34,816 in 120 s). Its trace (memcpy 0x144F34, 0x300EF8): 72.5 % written, 24.8 % copied,
   2.8 % from a RomFS file (0x3C4 bytes again); the top writers are the same functions as Omega Ruby's, at X's
   addresses and byte-identical (symbols/shared_functions.tsv): `Gfx_BuildCommandList` (X 0x323420, 33.4 %),
   `Gfx_FlushLightAndMatrixCommands` (0x323C00, 17.2 %), 0x14B984 (Omega Ruby's 0x14AFA8). One graphics library for
   both games, as step 4 hoped: reimplemented once.
   **Built once, or each frame** (5 November: each word's store time kept through the copies, `gpu_map.py`): at
   Omega Ruby's title, 73 % of the words submitted were built earlier and are copied into each frame's lists (all of
   `Gfx_BuildCommandList`'s and `Gfx_FlushLightAndMatrixCommands`' words, entered once in 60 s: their buffers are
   built when the scene loads, as are 0x14AFA8's and the program's templates copied by 0x300B28); 27 % are built
   each frame, by 0x32430C, 0x323D98, 0x144CA8, 0x13EA34, 0x123888, 0x4BE1E0 (called from 0x152888, the main loop),
   0x32403C, 0x323810. For the renderer: the scene's command buffers can be turned into Vulkan objects once, when
   built; only the per-frame part (the functions above: matrices, uniforms, to read next) runs in every frame.
   **What each sets** (the lists read as the GPU reads them, each parameter credited to its word's origin,
   $HOS_GPU_TRACE_REGS; register groups from 3dbrew), Omega Ruby's title, share of the command parameters:
   | Function | Share | Built | GPU registers it sets |
   |---|---|---|---|
   | `Gfx_BuildCommandList` 0x33899C | 36.0 % | once | vertex shader float uniforms 94 % (the materials' constants), textures 5 % |
   | 0x300B28 copies (program data) | 17.7 % | once | vertex attribute layout 62 %, shader code / flags 17 % |
   | RomFS file at 0x5B71C0F0 | 8.9 % | once | vertex shader code 100 %: the shader binary, uploaded as is |
   | 0x14AFA8 | 5.0 % | once | uniform indexes, rasterizer, attributes |
   | `Gfx_FlushLightAndMatrixCommands` 0x33917C | 5.6 % | once | geometry pipeline, draw, uniforms |
   | 0x144CA8, 0x4BE1E0, 0x32403C, 0x4BCD14, 0x15EB38 | 11.2 % | each frame | vertex shader float uniforms: matrices |
   | 0x323D98 | 2.4 % | each frame | textures 93 % |
   | 0x32430C | 3.4 % | each frame | the draw calls (geometry pipeline, draw) and uniforms |
   | 0x13EA34, 0x123888 | 3.0 % | each frame | rasterizer (viewport), output (framebuffer, blending) |
   So the vertex shaders come from the game's files (step 4: translated once to SPIR-V, as §8 planned), the
   materials and vertex layouts are built once per scene, and each frame sets matrices, textures, the framebuffer
   and draws. What the per-frame functions are (read from their code, Ghidra through Pomegrade's
   tools/remake/ghidra/decompile.sh; roles inferred from it, not yet checked against a run):
   - 0x152888 walks a scene tree: each node's virtual function at +0x74, then its children (a list at +0x14), as
     their flags (+0xB3, +0xB5, +0xB6) allow.
   - 0x4BE5D0 draws a mesh node: `Gfx_UpdateDrawState` only when the node is marked changed (that is where
     `Gfx_BuildCommandList` builds its list, once), then 0x323C8C submits the node's built buffer (+0x100) with its
     material (+0x104).
   - 0x4BE1E0 batches sprites (2D quads): per sprite, its 3x4 matrix (0x144CA8: a 48-byte copy) into the batch's
     uniform staging, a texture change (0x323810, 0x323D98) flushing the batch; 0x32430C flushes it: the matrices as
     vertex shader uniforms, then one draw of count x 6 indices (two triangles a sprite).
   So at the title the per-frame work is a scene walk, prebuilt mesh buffers resubmitted, and batched sprites. For
   Vulkan: a mesh node's buffer becomes a pipeline and descriptor set made when it is built; a sprite batch, one
   instanced draw with its matrices in a uniform (or storage) buffer.
   The models are BCH files (Nintendo's H3D format: version 0x21 in Omega Ruby, 0x07 in X; the runtime model class
   `xy_system::H3dShaderModel` in both, Pomegrade's ORAS_ENGINE.md), and Pomegrade already reads them in C++
   (tools/remake/src/Bch.cpp, BchUpgrade.cpp for X's version, PicaCommands.cpp, PicaTexture.cpp: reused, §5). A mesh
   node (+0x104: its material, +0xD4 / +0xFA: its state block and count, +0xFE: changed) is built by
   `Gfx_UpdateDrawState` (a default render state, `Gfx_TextureSetupFromHeader`, the state block applied, then
   `Gfx_BuildCommandList`), once; 0x323C8C draws a node that has a 2D layout part (+0xD8, +0xDC) through 0x337F6C.
   Still to read for the renderer's interface: `Gfx_BuildCommandList`'s inputs field by field (306 lines), the
   material record (+0x104), and how a BCH's meshes, materials and shaders become these nodes when a model loads.
   **The field** (5 November, Omega Ruby from the owner's save, buttons scripted: HOS_INPUT): DllField loaded, about 5
   command lists a frame without stopping (51,729 in 3 minutes). Its lists come from the same functions as the
   title's, in about the same shares: copies of the program's templates (0x300B28, 27.5 %: vertex layouts, shader
   setup), the shader binary from the RomFS (13.8 %), per-frame uniforms by 0x144CA8, 0x32403C, 0x4BE1E0, 0x4BCD14,
   0x15EB38 (27 %: the matrices of every drawn object), textures by 0x323D98, draws by 0x32430C, and the materials'
   buffers built once by `Gfx_BuildCommandList`. So 0x4BE1E0 / 0x32430C are the general draw path, not sprites only:
   each object's matrices sent every frame, its prebuilt buffer resubmitted. New on the field: 0x19B76C and 0x19B6E8
   set geometry shader registers (to read: the PICA200's geometry shader has no Mali equivalent, §8.1).
   Read (5 November): they are generic helpers writing a 4x4 / 3x4 matrix into a uniform upload command (components
   reversed, as the PICA200 takes them), and every matrix goes both to the vertex shader's uniforms (0x2C1) and the
   geometry shader unit's (0x291): the PICA200's fourth unit runs vertex shaders when no geometry shader is on, and is
   given the same uniforms. The control registers' values over 150 s on the field ($HOS_GPU_TRACE_VALUES):
   GPUREG_GEOSTAGE_CONFIG (0x229) 0 in 174,989 settings, 0x100 in 654, 2 (geometry shader on) in 120, set by
   0x184020 alone (an effect, to identify). So nearly every draw needs no geometry shader; the rare one is to be
   rewritten (vertex shader expansion or compute), not emulated, on Mali.
   `Gfx_BuildCommandList` (first reading): its list is a record (render state + 0x44) holding the command buffer
   (+0x10, words used +0x1C), a count of draw items (+4, +6) and the items at +0x28, 0x2C bytes each, each pointing
   (+0x28) at a draw record: its key (+8: the items sharing it are drawn together, the light and matrix commands
   flushed between keys), its vertex buffer (+0xC), its primitive kind (+0x10: 8 and 0xB set one shader setup, other
   values another), and a virtual at +4 then + 100 giving one more register value; command templates are copied from
   the program's data (0x338D84...: 0x60, 0x30 and 0x10 bytes). Each item's own word at +0x38 is compared byte-swapped
   with the last (a sort key). Still to read: the rest of its loop, the draw record in full, and what fills the items.
