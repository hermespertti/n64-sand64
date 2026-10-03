# sand64

A deterministic falling-sand simulator for the Nintendo 64, with the heavy
physics running on the **RSP** (Reality Signal Processor) vector unit.

160x116 cellular-automaton grid rendered at 2x on a 320x240 framebuffer.
Sand falls, water levels, fire burns, plants grow, smoke drifts -- and every
simulation step is bit-for-bit reproducible.

## Status

| Stage | What | Status |
|-------|------|--------|
| v1    | CPU engine, truecolor render | done |
| v3    | Row-phase vector-portable engine (V->D->H) | done |
| 2a    | RSP V-phase offload, verified | done |
| 2b    | **Fused per-row V+D+H on RSP** | done: mism=0 / 2550 frames |
| 2c    | Smoke (S) phase fused into the per-row RSP pass | ✅ done |
| --    | DICK mode (`-DDICK`) | done |

Verified against a CPU oracle in the same ROM: **grid mismatches = 0**,
**conservation exact** (`cons_s=0 cons_w=0`), **determinism byte-identical**
across runs.

## How it works

The CPU owns the grid; the RSP owns motion. Per frame the CPU bakes a slab
(8 grid rows + halo, u16 per cell) into RSP DMEM and one ucode pass runs the
three movement phases per row, in lockstep bottom-up:

- **V** -- vertical fall into air, sand/seed sinking through water
- **D** -- diagonal slide (uniform direction per frame, alternating parity)
- **H** -- horizontal leveling (strict parallel two-buffer semantics)

Uniform per-frame diagonal direction is what makes the whole thing
conflict-free: within a phase each destination has exactly one claimer, so
the vector unit computes moves as pure lane-wise compares and shifts with no
read-write hazards.

Movement works entirely on the u16 slab (low byte = material id), using
`lqv`/`sqv` unaligned windows for neighbor reads -- all stitch semantics
pinned empirically against the ares interpreter, not guessed from docs.

Smoke, fire and transforms stay CPU-side for now (timers + RNG + per-cell
branching). Phase S fusion is the next milestone.

## The bugs that mattered

- `sw` into a packed DMEM header clobbered the direction-flag byte next door
  -- the RSP slid one direction *forever* while the CPU oracle alternated.
  One instruction, weeks of ghost mismatches. Fix: `sh`.
- `vsub $vD, $v00, $vX` looks like bitwise NOT, *is* two's-complement
  negation -- zero (AIR) lanes survived as zero and silently erased the
  world. Fix: `vnor`.
- A missing `nop` delay slot after `j` corrupted a loop counter with no
  crash, no exception, just vibes.
- ares's RSP is a per-lane software interpreter: 11.6 ms/chunk-set there is
  *not* 11.6 ms on silicon. Parity first, perf claims on hardware.

## Building

Requires [libdragon](https://github.com/DragonMinded/libdragon) and the
mips64-elf toolchain:

```sh
make                                        # plain ROM
make USE_RSP=1                              # RSP fused physics
make USE_RSP=1 EXTRA_CFLAGS="-DAUTOTEST -DRSP_VERIFY"   # oracle parity run
make USE_RSP=1 EXTRA_CFLAGS="-DAUTOTEST -DDICK"         # :dick:
```

Gotcha: `make` does not rebuild on `EXTRA_CFLAGS` changes alone -- `touch
main.c rsp_sand64.S` between flag-variant builds.

## Verification

Headless CI runs the ROM in
[ares](https://github.com/ares-emulator/ares) (via a JS test runner) with a
CPU oracle compiled alongside the RSP path:

- `mism=0` -- every cell of every chunk matches the oracle, every frame
- `cons_s=0 cons_w=0` -- sand/water conserved exactly (spawns/erasures netted)
- determinism: two full runs, identical probe streams
- phase-mask matrix -- V, D, H alone and in all combinations, all green
- `test/rspmicro/` -- empirical `lqv`/`sqv` unaligned-stitch microtests

Controls (non-AUTOTEST build): stick moves the brush, A paints, B erases,
L walls, R fire, C-left/right switch material.

## Materials

`AIR WALL SAND WATER SEED PLANT FIRE SMOKE`

## License

WTFPL -- do what fuck.
