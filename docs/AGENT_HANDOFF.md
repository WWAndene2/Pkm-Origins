# Hand-off to the next agent

The state of the work and how to carry it on, for an agent starting cold (owner's request, 6 November). Rules first:
`CLAUDE.md` (the game's data never in the repository, faithful first, verify with real runs, short answers to the
owner in French or in the language they write in). The plan and its history: `docs/RECOMPILATION_PLAN.md` (§10 for
what is done, §11 for the renderer). This file says where things stand and what to do next; the plan holds the why.

## Where it stands (6 November)

- **Translation:** Omega Ruby's .code and its 145 CRO modules translate to C and compile (`tools/build_game.py`):
  26,486 functions (369 of them entries), 60 stubs. Proven function by function against the original in Unicorn
  (`tools/match_functions.py`): 0 mismatches on every sample run since step 2. X/Y translates the same way (not run
  this session).
- **Runtime (`src/hos/`):** the game boots, plays its opening cinematic to the end and goes past it. No freeze left
  that is known (the DSP one is fixed: `DspSource.c`).
- **Renderer (`src/platform/`):** Vulkan, headless, frames to PPM. The opening matches Azahar's shots (29 of 36 within
  2.5 / 255). It is the first, correct-but-slow form: one submission per draw, vertex shaders on the CPU.
- **The blocker now:** after the opening, with the owner's save, Azahar shows the title movie; we show a white top
  screen and a black bottom one. The game decodes the movie's frames through `y2r:u` (480 conversions in 300 s) and
  issues no draw at all. Azahar's run makes **no** `y2r:u` request over the same span, 2 WaitSynchronizationN where we
  make 799, and no SleepThread where we make 2,033. So the game takes another path with us. Find why.

## How to work (the loop)

Everything runs in the session's scratch space (`$S` below); the owner's game files are fetched there, never into the
repository. `$S/work/oras` holds the extracted game (code.bin, cro/, save/), `$S/work/dumps/oras.3ds` the ROM,
`$S/build_oras_mod` the build. Pomegrade (the owner's other repository, read only here) holds Azahar and its headless
runner.

| To | Tool |
|---|---|
| Translate, link, run once with frames | `tools/build_and_run.sh $S <seconds> [--no-translate] [--slow]` (translation ~9 min) |
| Check the translation | `tools/match_functions.py <code.bin> symbols/oras_functions.tsv --trials 20 --limit 3000 --jobs 4 --ram $S/work/runs/ram_oras` (and `--only <address>`) |
| Azahar's screenshots of a moment | `tools/azahar_shots.sh <Pomegrade> $S/work <name> <first frame> <count> <step> [save|none]` |
| Our frames against Azahar's | `tools/frame_compare.py <our frames> <shots in time order> --out sheet.png --zoom-out dir`, then **look** at the sheet |
| Our frames by eye | `tools/frame_sheet.py <frames> --out sheet.png --from N --every K [--crop x,y,w,h --zoom 3]` |
| Textures as decoded | `$HOS_TEXTURE_DIR=<dir>` on a run |
| A hang caught | `tools/hang_watch.sh <game> <dir> <rom> <out> [runs] [seconds]` (threads and host stacks) |
| The game's calls, ours and Azahar's | `tools/hos_call_counts.sh` (ours), `tools/azahar_trace_run.sh` (Azahar's), `tools/call_counts_compare.py` |
| Hand-written code | `tools/asm_scan.py <code.bin> hand-calls / refs <address> / literals <functions.tsv>` |
| A thread dump of a running game | `kill -USR1 <pid>` (or `$HOS_DUMP_AFTER=<seconds>`) |

Lessons that cost time, so they are not paid twice:

1. **Look at the frames, zoomed, not only at a score.** A mean difference of 12 / 255 hid every texture upside down.
   `frame_compare.py` now gives the worst 32 x 32 tile and labels its columns; Azahar's column has artefacts of its own.
2. **Match shots in time order** and render slowed (`--slow`): otherwise a frame of another moment is compared.
3. **A hang that comes once in ten runs**: catch it with `hang_watch.sh`, then record what the game sees (a cheap ring
   of the values it reads) rather than every store (a heavy watch made the race vanish).
4. **Fix the class, not the instance.** Hand-written code broke the translation five ways in a row; translating its
   whole range as one body (`symbols/oras_asm_regions.tsv`) ended it. Scan the binary for every case first
   (`asm_scan.py`).
5. **The behaviour is Azahar's, the code ours.** Azahar's source (in Pomegrade, `azahar/src`) is the description of
   the 3DS to follow; where we invented behaviour (our DSP clock) it broke.
6. **Diagnostics stay out of the repository**: build them as a separate binary from a patched tree, then restore it
   (`git checkout`), or keep the patch in the scratch space; commit only what is verified, saying what was checked.
7. **Bound the runs.** Say how many runs and what result ends them before starting (the owner objected to series of
   10, then 15, then 20).

## Next steps, in order

1. **The path after the opening** (the blocker above). Start from `call_counts_compare.py`'s differences: what
   decides that the game decodes through `y2r:u` with us and not in Azahar? Candidates to check, none checked yet:
   what `cfg`, `apt` or `fs` answer about the system (a model or a setting the movie player reads), the timing of
   `y2r:u`'s transfer-end event, an FS request that fails differently (Azahar makes `CreateDirectory`,
   `CreateExtSaveData` that we do not). A record of each thread's calls on both sides
   (`tools/hos_trace_compare.py`) narrows it to the first that differs.
2. **Fog** in the fragment shader (`PicaFragment.c`, Azahar's generator as the reference), then the volcano shot (y9).
3. **A render target written back to memory** when the game reads it as memory (the display transfer from VRAM to
   the linear heap seen at frame 0).
4. **Speed** (owner's question, 6 November: the renderer is not yet what the plan promises): one command buffer per
   frame, the vertex shaders on the GPU (the translator `PicaShaderGlsl.c` exists and is tested, not plugged in), the
   renderer on its own thread (it holds the kernel lock today, which caused the DSP freeze). Measure each on one scene
   and check it against Azahar again.

## Open with the owner

- Nothing. (The `item` command pushed to Pomegrade by mistake, commit `31f59fe0` on its branch `ccr-700c70f5-5dwhza`, is
  now `tools/oras_save_items.py` here; the owner asked to keep that Pomegrade branch: do not delete it.)
