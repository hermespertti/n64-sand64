/* sand64 — N64 falling-sand simulator.
 * v1: CPU physics (deterministic), RSP ucode comes next.
 *
 * Grid 160x120 cells, rendered 4x to the 640x480 framebuffer.
 * Determinism contract (CI): fixed initial terrain, xorshift32 RNG with a
 * build-time seed, frame-parity scan alternation. Two headless runs must
 * produce byte-identical probe lines.
 *
 * Physics notes (vectorization-ready for the later RSP/CPU-SIMD port):
 *  - movement is only into EMPTY (swap) or EMPTY-on-top (fall); one move
 *    per cell per frame, bottom-up scan means "moved" flags are implicit —
 *    a cell we reach from below already fell, and re-testing a destination
 *    we just vacated can only re-pick the same cell (idempotent).
 *  - per-cell fire/plant timers live in a parallel byte plane (TL).
 */
#include <libdragon.h>


#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GW   160
#define GH   116   // 116*2 + TOP 8 = 240 exactly
#define GS   (GW * GH)
#define SCALE 2
#define FBW  320
#define FBH  240
#define TOP  8   /* HUD strip height in framebuffer rows */

#ifndef PHASE_MASK
#define PHASE_MASK 7
#endif
#ifdef USE_RSP
#include <rsp.h>
DEFINE_RSP_UCODE(rsp_sand64);
/* slab buffer: nsim rows + 1 halo, u16 cells */
#define NSIM 8
static uint16_t slab_h[48] __attribute__((aligned(16)));    /* dmem hdr mirror */
static uint16_t mblk[96] __attribute__((aligned(16)));  /* boundary-mask tables @0xD00 */
static uint16_t slab_buf[(NSIM + 1) * GW] __attribute__((aligned(16)));
static uint16_t slab_out[(NSIM + 1) * GW] __attribute__((aligned(16)));
static long rsp_chunks = 0;
#endif

/* materials (single byte per cell — plane-parallel friendly) */
enum { M_AIR, M_WALL, M_SAND, M_WATER, M_SEED, M_PLANT, M_FIRE, M_SMOKE, NMAT };

static const char *mname[NMAT] = { "AIR", "WALL", "SAND", "WATER",
                                   "SEED", "PLANT", "FIRE", "SMOKE" };
/* ares VI reads framebuffer words MSB-first: word = R<<24|G<<16|B<<8|A.
   (empirically: R<<24 packing renders true colors; little-endian packing
   byte-shifts everything) */
static uint32_t mkc(int r, int g, int b) { return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFFu; }
static uint32_t mcol[NMAT] = {   /* NOT const: filled at boot (gcc folds const reads to their static init and skips the memcpy) */
    [M_AIR]   = 0,                 /* filled at boot with true black */
    [M_WALL]  = 0,
    [M_SAND]  = 0,
    [M_WATER] = 0,
    [M_SEED]  = 0,
    [M_PLANT] = 0,
    [M_FIRE]  = 0,
    [M_SMOKE] = 0,
};
static int init_colors_done = 0;
static void init_colors(void)
{
    uint32_t c[NMAT];
    c[M_AIR]   = mkc(10, 10, 16);
    c[M_WALL]  = mkc(90, 84, 96);
    c[M_SAND]  = mkc(196, 148, 74);
    c[M_WATER] = mkc(40, 96, 200);
    c[M_SEED]  = mkc(120, 88, 40);
    c[M_PLANT] = mkc(46, 160, 60);
    c[M_FIRE]  = mkc(240, 120, 30);
    c[M_SMOKE] = mkc(70, 70, 78);
    memcpy((void *)mcol, c, sizeof c);
    init_colors_done = 1;
}

static int flammable(uint8_t m) { return m == M_SEED || m == M_PLANT; }
static int is_gas(uint8_t m)   { return m == M_AIR || m == M_WATER; }

static uint16_t G [GS] __attribute__((aligned(16)));  /* material plane: u16 cell => 1 RSP lane = 1 cell */
static uint8_t TL [GS] __attribute__((aligned(16)));   /* fire/plant timers */
static uint8_t BRX[4096] __attribute__((aligned(16))); /* brush paint buffer */

/* deterministic RNG */
static uint32_t rng_s = 0xC0FFEE42u;
#ifndef RNGSEED
#define RNGSEED rng_s
#endif
static inline uint32_t xrnd(void)
{
    uint32_t x = RNGSEED;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    RNGSEED = x;
    return x;
}

/* conservation tracking (headless CI assertions) */
static long spawn_sand = 0, spawn_water = 0, spawn_seed = 0, spawn_fire = 0, spawn_plant = 0;
static long erase_sand = 0, erase_water = 0;
static long cnt[NMAT];

static int frame_no = 0;
static long long us_sim_total = 0, us_paint_total = 0;
static long fps1000 = 0;

/* ---------- world init ---------- */
static void set_cell(int x, int y, uint8_t m)
{
    if (x < 0 || x >= GW || y < 0 || y >= GH) return;
    uint8_t old = G[y * GW + x];
    if (old == M_SAND && m != M_SAND) erase_sand++;
    if (old == M_WATER && m != M_WATER) erase_water++;
    if (m == M_SAND && old != M_SAND) spawn_sand++;
    if (m == M_WATER && old != M_WATER) spawn_water++;
    if (m == M_SEED && old != M_SEED) spawn_seed++;
    G[y * GW + x] = m;
    TL[y * GW + x] = (m == M_FIRE) ? (uint8_t)(10 + (xrnd() & 7)) : 0;
}

static void build_terrain(void)
{
    memset(G, M_AIR, GS * 2);
    memset(TL, 0, GS);
    /* bottom + side walls */
    for (int x = 0; x < GW; x++) set_cell(x, GH - 1, M_WALL);
    for (int y = 0; y < GH; y++) { set_cell(0, y, M_WALL); set_cell(GW - 1, y, M_WALL); }
    /* two slanted shelves forming a funnel into a bottom pool */
    for (int i = 0; i < 56; i++) {
        set_cell(14 + i, 44 + i / 3, M_WALL);
        set_cell(GW - 15 - i, 44 + i / 3, M_WALL);
    }
    /* a little catch-basin near the bottom middle */
    for (int i = 0; i < 28; i++) {
        set_cell(GW / 2 - 14 + i, 104, M_WALL);
    }
    set_cell(GW / 2 - 15, 96, M_WALL); set_cell(GW / 2 + 14, 96, M_WALL);
    /* pre-grown bushes so fire has fuel on frame 1 */
    for (int x = 34; x < 52; x++) for (int y = 88; y < 96; y++)
        if (((x * 7 + y * 13) % 5) != 0) set_cell(x, y, M_PLANT);
    for (int x = 110; x < 126; x++) for (int y = 70; y < 78; y++)
        if (((x * 3 + y * 11) % 4) != 0) set_cell(x, y, M_PLANT);
}

/* ---------- brush painting ---------- */
#define BRUSH 3
static void paint(int cx, int cy, uint8_t m)
{
    for (int dy = -BRUSH; dy <= BRUSH; dy++)
        for (int dx = -BRUSH; dx <= BRUSH; dx++) {
            if (dx * dx + dy * dy > BRUSH * BRUSH + 1) continue;
            int x = cx + dx, y = cy + dy;
            if (x < 1 || x >= GW - 1 || y < 1 || y >= GH - 1) continue;
            int i = y * GW + x;
            uint8_t old = G[i];
            if (old == M_SAND && m != M_SAND) erase_sand++;
            if (old == M_WATER && m != M_WATER) erase_water++;
            if (m == M_SAND && old != M_SAND) spawn_sand++;
            if (m == M_WATER && old != M_WATER) spawn_water++;
            if (m == M_SEED && old != M_SEED) spawn_seed++;
            G[i] = m;
            TL[i] = (m == M_FIRE) ? (uint8_t)(10 + (xrnd() & 7)) : 0;
        }
}

#ifdef AUTOTEST
static int water_cnt(void) { int n = 0; for (int i = 0; i < GS; i++) if (G[i] == M_WATER) n++; return n; }
static long spawn_water_last = 0, erase_water_last = 0;
static int g_wA; static long g_eA, g_sA;
#endif
/* ---------- physics ---------- */
static inline int idx(int x, int y) { return y * GW + x; }

/* ---------- v3 step: three vector-portable row phases (RSP design v3) ----------
 * PHASE ORDER per frame: V (down) -> D (diagonal) -> H (level) -> smoke-up
 * -> transforms -> fire. Uniform shift direction per frame (parity) makes
 * D and H conflict-free: each destination cell has exactly ONE claimant,
 * so the whole phase is compare+select — exactly what the RSP ucode does.
 * Movement semantics: a grain may move once per PHASE (falls, slides and
 * spreads within one frame — Noita-like flow). Bottom-up row order; dest
 * rows already finalized for the phase; writes land only where checked AIR.
 * AIR==0 makes eligibility pure zero-compare. Determinism contract: CPU
 * mirror must equal RSP output byte-for-byte (mism counter in probe).
 */
static uint16_t HROW[GW] __attribute__((aligned(16)));
static uint16_t HOUT[GW] __attribute__((aligned(16)));

static int movable(uint8_t m) { return m == M_SAND || m == M_SEED || m == M_WATER; }
static int dense(uint8_t m)   { return m == M_SAND || m == M_SEED; }

/* V: straight down into AIR; sand/seed swap-sink through WATER (water up) */
static void phase_V(void)
{
    for (int y = GH - 2; y >= 1; y--) {
        for (int x = 1; x < GW - 1; x++) {
            int i = idx(x, y), b = idx(x, y + 1);
            uint8_t m = G[i];
            if (!movable(m)) continue;
            if (G[b] == M_AIR) { G[b] = m; G[i] = M_AIR; continue; }
            if (dense(m) && G[b] == M_WATER) { G[b] = m; G[i] = M_WATER; continue; }
        }
    }
}

/* D: diagonal down (uniform dir by parity). Destination unique per source:
   dest(z) claimant is exactly x = z - d. */
static void phase_D(void)
{
    int d = (frame_no & 1) ? -1 : 1;
    /* RSP-parity: scan x AGAINST slide dir so dest checks see pre-phase
     * state (parallel-hardware semantics). Dest is row y+1 (already
     * finalized), so any order is actually equivalent; keep it explicit. */
    for (int y = GH - 2; y >= 1; y--) {
        for (int xs = 1; xs < GW - 1; xs++) {
            int x = (d > 0) ? xs : (GW - 1 - xs);
            uint8_t m = G[idx(x, y)];
            if (!movable(m)) continue;
            int nx = x + d;
            if (nx < 1 || nx >= GW - 1) continue;
            int b = idx(nx, y + 1);
            if (G[b] == M_AIR) { G[b] = m; G[idx(x, y)] = M_AIR; }
            else if (dense(m) && G[b] == M_WATER) { G[b] = m; G[idx(x, y)] = M_WATER; }
        }
    }
}

/* H: level one cell sideways (uniform dir, opposite of D this frame).
   Reads use pre-phase row snapshot (HROW), writes land in G. A cell can be
   both source and gained-dest only if pre-state contradictory => no clash. */
static void phase_H(void)
{
    int h = (frame_no & 1) ? 1 : -1;
    for (int y = GH - 2; y >= 1; y--) {
        memcpy(HROW, &G[y * GW], GW * 2);
        for (int x = 1; x < GW - 1; x++) {
            uint16_t m = HROW[x];
            if (!movable(m)) continue;
            int nx = x + h;
            if (nx < 1 || nx >= GW - 1) continue;
            if (HROW[nx] == M_AIR) { G[idx(nx, y)] = m; G[idx(x, y)] = M_AIR; }
        }
    }
}

/* FUSED per-row contract (v4): for y = GH-2..1: V(y); D(y); H(y).
 * Exact per-chunk replay on RSP: dest/source claims are unique per phase,
 * and every read a cell needs happened before its row-finalizing write,
 * so vectorized snapshot reads == CPU live scans. All builds (CPU+RSP)
 * run THIS contract so folds match by construction. */
static void fused_move(void)
{
    int d = (frame_no & 1) ? -1 : 1;
    int h = -d;
    for (int y = GH - 2; y >= 1; y--) {
        /* V */
        if (PHASE_MASK & 1)
        for (int x = 1; x < GW - 1; x++) {
            int i = idx(x, y), b = idx(x, y + 1);
            uint8_t m = G[i];
            if (!movable(m)) continue;
            if (G[b] == M_AIR) { G[b] = m; G[i] = M_AIR; continue; }
            if (dense(m) && G[b] == M_WATER) { G[b] = m; G[i] = M_WATER; continue; }
        }
        /* D */
        if (PHASE_MASK & 2)
        for (int x = 1; x < GW - 1; x++) {
            uint8_t m = G[idx(x, y)];
            if (!movable(m)) continue;
            int nx = x + d;
            if (nx < 1 || nx >= GW - 1) continue;
            int b = idx(nx, y + 1);
            if (G[b] == M_AIR) { G[b] = m; G[idx(x, y)] = M_AIR; }
            else if (dense(m) && G[b] == M_WATER) { G[b] = m; G[idx(x, y)] = M_WATER; }
        }
        /* H: strict parallel snapshot (two buffers, matches ucode):
         * nb/movS/curAIR all read the pre-H snapshot; writes go to HOUT.
         * A grain vacating x forbids x+1 chasing into x the same step. */
        if (!(PHASE_MASK & 4)) continue;
        memcpy(HROW, &G[y * GW], GW * 2);
        memcpy(HOUT, HROW, GW * 2);
        for (int x = 1; x < GW - 1; x++) {
            uint16_t m = HROW[x];
            if (!movable(m)) continue;
            int nx = x + h;
            if (nx < 1 || nx >= GW - 1) continue;
            if (HROW[nx] == M_AIR) { HOUT[nx] = m; HOUT[x] = M_AIR; }
        }
        memcpy(&G[y * GW], HOUT, GW * 2);
    }
}

/* smoke rises straight up, one cell/frame (bottom-up scan; V for smoke) */
static void phase_S(void)
{
    for (int y = GH - 2; y >= 1; y--) {
        for (int x = 1; x < GW - 1; x++) {
            int i = idx(x, y);
            if (G[i] != M_SMOKE) continue;
            if (--TL[i] == 0) { G[i] = M_AIR; continue; }
            int u = idx(x, y - 1);
            if (G[u] == M_AIR) { G[u] = M_SMOKE; G[i] = M_AIR; TL[u] = TL[i]; }
            else if (G[u] != M_SMOKE) {   /* blocked: sideway drift (parity) */
                int dr = ((frame_no ^ y) & 1) ? 1 : -1;
                int a = idx(x + dr, y - 1);
                if (x + dr >= 1 && x + dr < GW - 1 && G[a] == M_AIR)
                    { G[a] = M_SMOKE; G[i] = M_AIR; TL[a] = TL[i]; }
            }
        }
    }
}

static int wet3(int x, int y)
{
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (G[idx(x + dx, y + dy)] == M_WATER) return 1;
    return 0;
}

static void transform_phase(void)
{
    /* seed on wet ground sprouts (consumes xrnd) */
    for (int x = 1; x < GW - 1; x++)
        for (int y = GH - 2; y >= 1; y--) {
            int i = idx(x, y);
            if (G[i] == M_SEED && TL[i] == 0 && wet3(x, y)) {
                G[i] = M_PLANT; TL[i] = (uint8_t)(40 + (xrnd() & 31)); spawn_plant++;
            }
        }
    /* plant grows up through gas, spends its timer chain (consumes xrnd) */
    for (int x = 1; x < GW - 1; x++)
        for (int y = GH - 2; y >= 1; y--) {
            int i = idx(x, y);
            if (G[i] != M_PLANT || TL[i] == 0) continue;
            if (!wet3(x, y)) continue;
            int u = idx(x, y - 1);
            if (is_gas(G[u])) {
                if (G[u] == M_WATER) erase_water++;
                G[u] = M_PLANT;
                TL[u] = (uint8_t)(30 + (xrnd() & 15));
                spawn_plant++;
                if (--TL[i] == 0) TL[i] = 1;
            }
        }
}

static void fire_phase(void)
{
    for (int x = 1; x < GW - 1; x++)
        for (int y = GH - 2; y >= 1; y--) {
            int i = idx(x, y);
            if (G[i] != M_FIRE) continue;
            if (wet3(x, y)) {                      /* quenched -> smoke, eats one water */
                G[i] = M_SMOKE; TL[i] = 60;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int j = idx(x + dx, y + dy);
                        if (G[j] == M_WATER) { G[j] = M_AIR; erase_water++; dy = 2; break; }
                    }
                continue;
            }
            for (int dy = -1; dy <= 1; dy++)       /* ignite ~35% per flammable nbr */
                for (int dx = -1; dx <= 1; dx++) {
                    int j = idx(x + dx, y + dy);
                    if (flammable(G[j]) && ((xrnd() >> 8) & 255) < 90) {
                        G[j] = M_FIRE; TL[j] = (uint8_t)(14 + (xrnd() & 15));
                        spawn_fire++;
                    }
                }
            if (--TL[i] == 0) {
                G[i] = ((xrnd() >> 20) & 1) ? M_SMOKE : M_AIR;
                if (G[i] == M_SMOKE) TL[i] = 90;
            }
        }
}

#ifdef USE_RSP
/* V phase on RSP: chunks bottom-up, slab = rows ya..yb + halo row yb+1.
   Sequential chaining: halo row is finalized in DRAM by the previous
   (lower) chunk's writeback — exactly CPU bottom-up order. */
static void probe(const char *line);

#ifdef RSP_VERIFY
static uint16_t Gcpu[GS] __attribute__((aligned(16)));
static void phase_V_on(uint16_t *g)
{
    for (int y = GH - 2; y >= 1; y--)
        for (int x = 1; x < GW - 1; x++) {
            int i = y * GW + x, b = (y + 1) * GW + x;
            uint16_t m = g[i];
            if (m != M_SAND && m != M_SEED && m != M_WATER) continue;
            if (g[b] == M_AIR) { g[b] = m; g[i] = M_AIR; continue; }
            if ((m == M_SAND || m == M_SEED) && g[b] == M_WATER) { g[b] = m; g[i] = M_WATER; continue; }
        }
}
static long mism_total = 0;
static void rsp_verify(const char *tag)
{
    long mm = 0; int fx = -1, fy = -1; uint16_t fa = 0, fb2 = 0;
    for (int i = 0; i < GS; i++)
        if (G[i] != Gcpu[i]) {
            mm++;
            if (fx < 0) { fx = i % GW; fy = i / GW; fa = G[i]; fb2 = Gcpu[i]; }
        }
    mism_total += mm;
    if (mm) {
        char ln[300];
        int n2 = snprintf(ln, sizeof ln, "[mism] f=%d %s n=%ld @(%d,%d) rsp=%u cpu=%u tot=%ld\n",
                 frame_no, tag, mm, fx, fy, fa, fb2, mism_total);
        if (n2 > 0) probe(ln);
        int r = fy * GW + fx;
        int b0 = r - 4, b1 = r + 4; if (b0 < 0) b0 = 0; if (b1 >= GS) b1 = GS - 1;
        int o = snprintf(ln, sizeof ln, "[win] f=%d rsp:", frame_no);
        for (int i = b0; i <= b1 && o < 250; i++) o += snprintf(ln + o, sizeof ln - o, " %u", G[i]);
        probe(ln);
        o = snprintf(ln, sizeof ln, "[win] f=%d cpu:", frame_no);
        for (int i = b0; i <= b1 && o < 250; i++) o += snprintf(ln + o, sizeof ln - o, " %u", Gcpu[i]);
        probe(ln);
    }
}
#endif

#ifdef RSP_PROF
static uint64_t rsp_prof_ld, rsp_prof_run, rsp_prof_t;
#endif
static void rsp_v_phase(void)
{
    const int w3 = 3, s2 = 2, s4 = 4;
    for (int yb = GH - 2; yb >= 0; yb -= NSIM) {
        int ya = yb - NSIM + 1; if (ya < 0) ya = 0;
        int nsim = yb - ya + 1;
        { uint8_t *h = (uint8_t *)slab_h;   /* nsim as BIG-endian u16: RSP scalar lhu reads BE */
          h[0] = 0; h[1] = (uint8_t)nsim;
          h[2] = (uint8_t)(frame_no & 1);   /* dirflag: 1 => D d=-1, H h=+1 (matches phase_D/H) */
          h[4] = PHASE_MASK;
          h[5] = (uint8_t)(ya == 0 ? 1 : 0); }
        /* source-validity masks @DMEM 0xD00 (mblk), slots of 8 u16:
         * [+0] TBL all-FF (interior vectors), [+16] D v0, [+32] D v19,
         * [+48] H v0, [+64] H v19. d=+1: x158 blocked (D v19 j6,7);
         * d=-1: x1 blocked (D v0 j1). h=-d flips to H tables. */
        {
            int dd = (frame_no & 1) ? -1 : 1;
            int hh = -dd;
            for (int i = 0; i < 96; i++) mblk[i] = 0xFFFF;
            uint16_t *d0 = mblk + 8, *d19 = mblk + 16, *h0 = mblk + 24, *h19 = mblk + 32;
            if (dd > 0) d19[6] = 0; else d0[1] = 0;
            if (hh > 0) h19[6] = 0; else h0[1] = 0;
            (void)dd; (void)hh;
        }
        for (int i = 0; i < 8; i++) {
            slab_h[8 + i] = 1;    /* ONE   @0x10 */
            slab_h[16 + i] = 2;   /* TWO   @0x20 */
            slab_h[24 + i] = 3;   /* THREE @0x30 */
            slab_h[32 + i] = 4;   /* FOUR  @0x40 */
            slab_h[40 + i] = 5;   /* FIVE  @0x50 */
        }
        (void)w3; (void)s2; (void)s4;
        /* slab rows ya..yb + halo (yb+1) */
        memcpy(slab_buf, &G[ya * GW], (nsim + 1) * GW * 2);
#ifdef RSP_VERIFY
        if (rsp_chunks == 0) {
            char ln[220]; int o = snprintf(ln, sizeof ln, "[vin ]");
            for (int i = 0; i < 20 && o < 180; i++) o += snprintf(ln + o, sizeof ln - o, " %u", slab_buf[i]);
            probe(ln);
        }
#endif
        data_cache_hit_writeback_invalidate(slab_h, sizeof slab_h);
        data_cache_hit_writeback_invalidate(slab_buf, (nsim + 1) * GW * 2);
#ifdef RSP_PROF
        uint64_t tA = TIMER_MICROS_LL(timer_ticks());
#endif
        rsp_load_data(slab_h, 96, 0x0000);
        rsp_load_data(slab_buf, (nsim + 1) * GW * 2, 0x0080);
        data_cache_hit_writeback_invalidate(mblk, sizeof mblk);
        rsp_load_data(mblk, sizeof mblk, 0x0D00);
#ifdef RSP_TRACE
        if (rsp_chunks == 0 && frame_no == 0) {
            static uint16_t pre[24] __attribute__((aligned(16)));
            rsp_read_data(pre, 48, 0x0080);
            data_cache_hit_writeback_invalidate(pre, 48);
            char ln[220]; int o = snprintf(ln, sizeof ln, "[pre f0]");
            for (int i = 0; i < 24 && o < 200; i++) o += snprintf(ln + o, sizeof ln - o, " %u", pre[i]);
            probe(ln);
        }
#endif
#ifdef RSP_PROF
        uint64_t tB = TIMER_MICROS_LL(timer_ticks());
#endif
        rsp_run();
#ifdef RSP_VERIFY
        if (rsp_chunks == 0 && frame_no <= 2) {
            uint16_t co[16];
            rsp_read_data(co, 32, 0x0080);       /* slab row k=0 cells 0..15 */
            data_cache_hit_writeback_invalidate(co, 32);
            char ln[220]; int o = snprintf(ln, sizeof ln, "[c0o f%d]", frame_no);
            for (int i = 0; i < 16 && o < 200; i++) o += snprintf(ln + o, sizeof ln - o, " %u", co[i]);
            probe(ln);
            o = snprintf(ln, sizeof ln, "[c0w f%d]", frame_no);
            for (int i = 0; i < 16 && o < 200; i++) o += snprintf(ln + o, sizeof ln - o, " %u", slab_buf[i]);
            probe(ln);
            rsp_read_data(co, 32, 0x0080 + 320);  /* k=1 */
            data_cache_hit_writeback_invalidate(co, 32);
            o = snprintf(ln, sizeof ln, "[c1o f%d]", frame_no);
            for (int i = 0; i < 16 && o < 200; i++) o += snprintf(ln + o, sizeof ln - o, " %u", co[i]);
            probe(ln);
        }
#endif
#ifdef RSP_PROF
        uint64_t tC = TIMER_MICROS_LL(timer_ticks());
        rsp_prof_ld += tB - tA; rsp_prof_run += tC - tB;
#endif
#ifdef RSP_TRACE
        { uint32_t hv; static uint8_t hb[8] __attribute__((aligned(16))); rsp_read_data(hb, 8, 0); rsp_read_data(&hv, 8, 0);
          data_cache_hit_writeback_invalidate(&hv, 8);
          char ln[96]; snprintf(ln, sizeof ln, "[hdr ] cpu=%d dmem0=%lu dir=%u echo=%u r0=%u",
                                 nsim, (unsigned long)hv, hb[2], hb[6], hb[5]); probe(ln); }
#endif
        rsp_read_data(slab_out, (nsim + 1) * GW * 2, 0x0080);
        if (rsp_chunks == 0 && frame_no == 0) {
            static uint16_t s1d[160] __attribute__((aligned(16)));
            rsp_read_data(s1d, 320, 0x0D80);
            data_cache_hit_writeback_invalidate(s1d, 320);
            char ln[220];
            static uint16_t pr[16] __attribute__((aligned(16)));
            rsp_read_data(pr, 32, 0x0060);
            data_cache_hit_writeback_invalidate(pr, 32);
            int o2 = snprintf(ln, sizeof ln, "[hprb f0]");
            for (int i = 0; i < 16 && o2 < 200; i++) o2 += snprintf(ln + o2, sizeof ln - o2, " %u", pr[i]);
            probe(ln);
            int o = snprintf(ln, sizeof ln, "[S1x f0]");
            for (int i = 0; i < 24 && o < 200; i++) o += snprintf(ln + o, sizeof ln - o, " %u", s1d[i]);
            probe(ln);
        }
        data_cache_hit_writeback_invalidate(slab_out, (nsim + 1) * GW * 2);
#ifdef RSP_PROF
        rsp_prof_t += TIMER_MICROS_LL(timer_ticks()) - tC;
        if ((rsp_chunks % 11) == 0) {
            char ln[120];
            snprintf(ln, sizeof ln, "[rsp prof] ld=%lu run=%lu rd=%lu per11chunks",
                     (unsigned long)rsp_prof_ld, (unsigned long)rsp_prof_run, (unsigned long)(rsp_prof_t - rsp_prof_ld - rsp_prof_run));
            probe(ln);
            rsp_prof_ld = rsp_prof_run = rsp_prof_t = 0;
        }
#endif
#ifdef RSP_TRACE
        if (rsp_chunks == 1) {
            static uint8_t tr[96] __attribute__((aligned(16)));
            rsp_read_data(tr, 96, 0x1800);
            data_cache_hit_writeback_invalidate(tr, 96);
            const char *nm[6] = {"cur","bel","movd","cnew","bnew","ONE"};
            char ln[200];
            for (int m = 0; m < 6; m++) {
                int o = snprintf(ln, sizeof ln, "[trc] %s:", nm[m]);
                for (int i = m * 16; i < m * 16 + 16; i++)
                    o += snprintf(ln + o, sizeof ln - o, " %u", (tr[i] << 8) | tr[i + 1]);
                probe(ln);
            }
        }
#endif
#ifdef RSP_VERIFY
#ifdef RSP_TRACE
#endif
        if (rsp_chunks == 0) {
            char ln[220]; int o = snprintf(ln, sizeof ln, "[vout]");
            for (int i = 0; i < 20 && o < 180; i++) o += snprintf(ln + o, sizeof ln - o, " %u", slab_out[i]);
            probe(ln);
        }
#endif
#ifdef RSP_VERIFY
        { /* fused oracle: apply V(row),D(row),H(row) bottom-up on slab copy, match ucode */
            static uint16_t want[(NSIM + 1) * GW];
            static uint16_t snap[GW];
            int ddir = (frame_no & 1) ? -1 : 1;
            int hdir = -ddir;
            memcpy(want, slab_buf, (nsim + 1) * GW * 2);
            int r0lo = (ya == 0) ? 1 : 0;   /* CPU fused skips grid row 0 */
            for (int y = nsim - 1; y >= r0lo; y--) {   /* rows relative to window top == grid ya..ya+nsim-1 */
                /* V */
                if (!(PHASE_MASK & 1)) goto noV;
                for (int x = 1; x < GW - 1; x++) {
                    int i = y * GW + x, b = i + GW;
                    uint16_t mm2 = want[i];
                    if (mm2 != M_SAND && mm2 != M_SEED && mm2 != M_WATER) continue;
                    if (want[b] == M_AIR) { want[b] = mm2; want[i] = M_AIR; continue; }
                    if ((mm2 == M_SAND || mm2 == M_SEED) && want[b] == M_WATER) { want[b] = mm2; want[i] = M_WATER; }
                }
                /* D: scan against dir (source j-d untouched pre-write; equals ucode snapshot) */
                if (!(PHASE_MASK & 2)) goto noD;
                for (int xs = 1; xs < GW - 1; xs++) {
                    int x = (ddir > 0) ? xs : (GW - 1 - xs);
                    uint16_t mm2 = want[y * GW + x];
                    if (mm2 != M_SAND && mm2 != M_SEED && mm2 != M_WATER) continue;
                    int nx = x + ddir;
                    if (nx < 1 || nx >= GW - 1) continue;
                    int b = (y + 1) * GW + nx;
                    if (want[b] == M_AIR) { want[b] = mm2; want[y * GW + x] = M_AIR; }
                    else if ((mm2 == M_SAND || mm2 == M_SEED) && want[b] == M_WATER) { want[b] = mm2; want[y * GW + x] = M_WATER; }
                }
                /* H: snapshot row, direct writes (matches ucode scratch-merge) */
                if (!(PHASE_MASK & 4)) goto noH;
                memcpy(snap, &want[y * GW], GW * 2);
                for (int x = 1; x < GW - 1; x++) {
                    uint16_t mm2 = snap[x];
                    if (mm2 != M_SAND && mm2 != M_SEED && mm2 != M_WATER) continue;
                    int nx = x + hdir;
                    if (nx < 1 || nx >= GW - 1) continue;
                    if (snap[nx] == M_AIR) { want[y * GW + nx] = mm2; want[y * GW + x] = M_AIR; }
                }
            noH:;
            noD:;
            noV:;
            }
            long mm = 0; int fx = -1;
            for (int i = 0; i < nsim * GW; i++)          /* source rows k=0..nsim-1 (halo k=nsim is pre-move on RSP side) */
                if (slab_out[i] != want[i]) { mm++; if (fx < 0) fx = i; }
            mism_total += mm;
            static int dumpbudget = 10;
            if (mm && dumpbudget > 0) {

                dumpbudget--;
                char ln[220];
                int r = fx / GW, xc = fx % GW, base = r * GW + (xc < 16 ? 0 : xc - 16);
                int o = snprintf(ln, sizeof ln, "[WANT] ya=%d d=%d r=%d x0=%d:", ya, ddir, r, base - r * GW);
                for (int i = base; i < base + 32 && o < 212; i++) o += snprintf(ln + o, sizeof ln - o, "%u", want[i]);
                probe(ln);
                o = snprintf(ln, sizeof ln, "[WOUT]");
                for (int i = base; i < base + 32 && o < 212; i++) o += snprintf(ln + o, sizeof ln - o, "%u", slab_out[i]);
                probe(ln);
                o = snprintf(ln, sizeof ln, "[WIN ]");
                for (int i = base; i < base + 32 && o < 212; i++) o += snprintf(ln + o, sizeof ln - o, "%u", slab_buf[i]);
                probe(ln);
                int nb = base + GW;   /* row below in slab */
                o = snprintf(ln, sizeof ln, "[WNBW]");
                for (int i = nb; i < nb + 32 && o < 212; i++) o += snprintf(ln + o, sizeof ln - o, "%u", want[i]);
                probe(ln);
                o = snprintf(ln, sizeof ln, "[WNBI]");
                for (int i = nb; i < nb + 32 && o < 212; i++) o += snprintf(ln + o, sizeof ln - o, "%u", slab_buf[i]);
                probe(ln);
            }
            if (mm && rsp_chunks < 6) {
                char ln[300]; int o = snprintf(ln, sizeof ln,
                    "[fvf] ya=%d nsim=%d d=%d n=%ld @(x=%d,row=%d) rsp:", ya, nsim, ddir, mm, fx % GW, fx / GW);
                for (int i = fx - 4; i <= fx + 4 && o < 250; i++) o += snprintf(ln + o, sizeof ln - o, " %u", slab_out[i]);
                probe(ln);
                o = snprintf(ln, sizeof ln, "[fvw] want:");
                for (int i = fx - 4; i <= fx + 4 && o < 250; i++) o += snprintf(ln + o, sizeof ln - o, " %u", want[i]);
                probe(ln);
            }
            if (mm && mm <= 40 && frame_no >= 15) {
                char ln[220];
                int base = (fx / GW) * GW + (fx % GW) - 16;
                int o = snprintf(ln, sizeof ln, "[zou] f=%d y%d x%d:", frame_no, fx / GW, base - (fx/GW)*GW);
                for (int i = base; i < base + 32 && o < 210; i++) o += snprintf(ln + o, sizeof ln - o, "%u", slab_out[i]);
                probe(ln);
                o = snprintf(ln, sizeof ln, "[zwt]");
                for (int i = base; i < base + 32 && o < 210; i++) o += snprintf(ln + o, sizeof ln - o, "%u", want[i]);
                probe(ln);
                o = snprintf(ln, sizeof ln, "[zin]");
                for (int i = base; i < base + 32 && o < 210; i++) o += snprintf(ln + o, sizeof ln - o, "%u", slab_buf[i]);
                probe(ln);
                int bb = base + GW;
                o = snprintf(ln, sizeof ln, "[zbw]");
                for (int i = bb; i < bb + 32 && o < 210; i++) o += snprintf(ln + o, sizeof ln - o, "%u", want[i]);
                probe(ln);
                o = snprintf(ln, sizeof ln, "[zbi]");
                for (int i = bb; i < bb + 32 && o < 210; i++) o += snprintf(ln + o, sizeof ln - o, "%u", slab_buf[i]);
                probe(ln);
            }
            if (mm && frame_no <= 3 && rsp_chunks == 1) {
                char ln[200];
                int base = (fx / GW) * GW;
                for (int seg = 0; seg < 4; seg++) {
                    int s0 = base + seg * 40;
                    int o = snprintf(ln, sizeof ln, "[xout] seg%d:", seg);
                    for (int i = s0; i < s0 + 40 && o < 190; i++) o += snprintf(ln + o, sizeof ln - o, "%u", slab_out[i]);
                    probe(ln);
                    o = snprintf(ln, sizeof ln, "[xwnt] seg%d:", seg);
                    for (int i = s0; i < s0 + 40 && o < 190; i++) o += snprintf(ln + o, sizeof ln - o, "%u", want[i]);
                    probe(ln);
                    o = snprintf(ln, sizeof ln, "[xin ] seg%d:", seg);
                    for (int i = s0; i < s0 + 40 && o < 190; i++) o += snprintf(ln + o, sizeof ln - o, "%u", slab_buf[i]);
                    probe(ln);
                }
            }
        }
#endif
        memcpy(&G[ya * GW], slab_out, (nsim + 1) * GW * 2);
        rsp_chunks++;
    }
}
#endif

static void probe(const char *line);
#ifdef PHASE_PROF
static uint64_t us_V;
static uint64_t us_D, us_H, us_S, us_T, us_F;
#endif

static void step(void)
{
    static uint16_t Gpre[GS] __attribute__((aligned(16)));
    memcpy(Gpre, G, sizeof Gpre);
#ifdef USE_RSP
#ifdef RSP_VERIFY
    /* CPU oracle branch must not perturb shared state that later feeds the
       RSP branch: xrnd stream, TL plane (fire lifetimes) all diverge otherwise. */
    static uint8_t TLpre[GS] __attribute__((aligned(16)));
    uint32_t rng_pre = RNGSEED;
    long ew_pre = erase_water, sw_pre = spawn_water;
    long es_pre = erase_sand, ss_pre = spawn_sand, sp_pre = spawn_seed;
    memcpy(TLpre, TL, sizeof TLpre);
    memcpy(Gcpu, Gpre, sizeof Gcpu);
    {
        fused_move();                          /* G = CPU-fused(Gpre) */
        phase_S();
        transform_phase();
        fire_phase();
        memcpy(Gcpu, G, sizeof Gcpu);          /* full CPU step */
        memcpy(G, Gpre, sizeof Gpre);          /* restore grid, TL, rng for RSP */
        memcpy(TL, TLpre, sizeof TLpre);
        RNGSEED = rng_pre;
    }
    memcpy(TL, TLpre, sizeof TLpre);           /* re-seed: transform/fire must
        see pre-step TL+rng exactly as the oracle branch did */
    RNGSEED = rng_pre;
    erase_water = ew_pre; spawn_water = sw_pre;
    erase_sand = es_pre; spawn_sand = ss_pre; spawn_seed = sp_pre;
    rsp_v_phase();                             /* G = RSP-fused(Gpre) */
    phase_S();
    transform_phase();
    fire_phase();
    rsp_verify("VDH");                         /* compare AFTER identical post-move phases */
#else
    rsp_v_phase();
    phase_S();
    transform_phase();
    fire_phase();
#endif
#elif defined(PHASE_PROF)
    uint64_t t;
    t = TIMER_MICROS_LL(timer_ticks()); fused_move(); us_D += TIMER_MICROS_LL(timer_ticks()) - t;
    t = TIMER_MICROS_LL(timer_ticks()); phase_S(); us_S += TIMER_MICROS_LL(timer_ticks()) - t;
    t = TIMER_MICROS_LL(timer_ticks()); transform_phase(); us_T += TIMER_MICROS_LL(timer_ticks()) - t;
    t = TIMER_MICROS_LL(timer_ticks()); fire_phase(); us_F += TIMER_MICROS_LL(timer_ticks()) - t;
    if (frame_no > 0 && (frame_no % 60) == 60 - 1) {
        char pl[200];
        snprintf(pl, sizeof pl, "[ph] VDH=%lu S=%lu T=%lu F=%lu usavg",
            (unsigned long)(us_D/60), (unsigned long)(us_S/60), (unsigned long)(us_T/60), (unsigned long)(us_F/60));
        us_D = us_S = us_T = us_F = 0; probe(pl);
    }
#else
#ifdef AUTOTEST
    { int w0, wa, wb, wc, wd;
      long e0 = erase_water, s0 = spawn_water;
      w0 = water_cnt(); fused_move();    wa = water_cnt();
      phase_S();                          wb = water_cnt();
      long e1 = erase_water, s1 = spawn_water;
      transform_phase();                  wc = water_cnt();
      long e2 = erase_water, s2 = spawn_water;
      fire_phase();                       wd = water_cnt();
      long e3 = erase_water, s3 = spawn_water;
      int u_mov = (wa - w0);                       /* should be 0 */
      int u_smk = (wb - wa) + (int)(e1 - e0) - (int)(s1 - s0);
      int u_trn = (wc - wb) + (int)(e2 - e1) - (int)(s2 - s1);
      int u_fir = (wd - wc) + (int)(e3 - e2) - (int)(s3 - s2);
      if (u_mov || u_smk || u_trn || u_fir) {
          char pl[160]; snprintf(pl, sizeof pl,
              "[wdbg f%d] mov=%d smoke=%d trans=%d fire=%d (w:%d->%d)",
              frame_no, u_mov, u_smk, u_trn, u_fir, w0, wd);
          probe(pl);
      }
    }
#else
    fused_move();
    phase_S();
    transform_phase();
    fire_phase();
#endif
#endif
}

static uint64_t grid_fold(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)G;
    for (long i = 0; i < GS * 2; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    for (long i = 0; i < GS; i++)     { h ^= TL[i]; h *= 0x100000001b3ull; }
    return h;
}


static void recount(void)
{
    memset(cnt, 0, sizeof cnt);
    for (int i = 0; i < GS; i++) cnt[G[i]]++;
}

/* ---------- ISViewer probe (word-wise writes, per libdragon/ares) ---------- */
static void probe(const char *line)
{
    int n = (int)strlen(line);
    static uint8_t isvbuf[320] __attribute__((aligned(8)));
    for (int i = 0; i < n; i++) isvbuf[i] = (uint8_t)line[i];
    for (int i = 0; i < n; i += 4) {
        uint32_t v = 0;
        for (int j = 0; j < 4; j++) v |= (uint32_t)isvbuf[i + j] << (24 - 8 * j);
        io_write(0x13FF0020 + i, v);
    }
    io_write(0x13FF0014, (uint32_t)n);
}

/* ---------- rendering ---------- */
static void render(uint32_t *pix, int cx, int cy, int mat)
{
    /* background strip */
    for (int y = 0; y < FBH; y++) {
        uint32_t *row = pix + y * FBW;
        if (y < TOP) { uint32_t c = mkc(20, 20, 30); for (int x = 0; x < FBW; x++) row[x] = c; }
    }
    /* cells 4x */
    for (int gy = 0; gy < GH; gy++) {
        uint32_t *row0 = pix + (TOP + gy * SCALE) * FBW;
        const uint16_t *grow = G + gy * GW;
        for (int gx = 0; gx < GW; gx++) {
            uint8_t m = (uint8_t)grow[gx];
            uint32_t c = mcol[m];
            if ((m == M_SAND || m == M_WATER) && ((gx * 31 + gy * 17 + gx * gy) & 8))
                c = c ^ ((m == M_SAND) ? 0x080800 : 0x000808); /* mottling */
            if (m == M_FIRE) {
                int l = TL[gy * GW + gx];
                c = mkc((140 + l * 7) & 255, 40 + ((l * 13) & 127), 20);
            }
            uint32_t *p = row0 + gx * SCALE;
            for (int sy = 0; sy < SCALE; sy++)
                for (int sx = 0; sx < SCALE; sx++)
                    p[sy * FBW + sx] = c;
        }
    }
    /* cursor: hollow box, white — clamped hard to FB bounds */
    uint32_t wc = mkc(255, 255, 255);
    int bx = cx * SCALE, by = TOP + cy * SCALE, s = (BRUSH + 1) * SCALE;
    for (int d = 0; d < s; d++) {
        int xx0 = bx + d, yy1 = by + s - 1;
        if (by < FBH && xx0 < FBW) pix[by * FBW + xx0] = wc;
        if (yy1 < FBH && xx0 < FBW) pix[yy1 * FBW + xx0] = wc;
        int yy = by + d;
        if (yy < FBH && bx < FBW) pix[yy * FBW + bx] = wc;
        if (yy < FBH && bx + s - 1 < FBW) pix[yy * FBW + bx + s - 1] = wc;
    }
}

/* HUD digits: 3x5 micro-font packed as row bits (no rdpq — it owns the RSP) */
static const uint8_t fonts[10][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},
};
static const uint8_t *glyph(char c)
{
    if (c >= '0' && c <= '9') return fonts[c - '0'];
    return fonts[0];
}
/* simpler: draw decimal numbers only — that's all the HUD needs */
static void draw_num(uint32_t *pix, int x, int y, long v)
{
    char s[16]; int n = snprintf(s, sizeof s, "%ld", v < 0 ? -v : v);
    for (int c = 0; c < n; c++) {
        const uint8_t *g = glyph(s[c]);
        for (int r = 0; r < 5; r++)
            for (int b = 0; b < 3; b++)
                if (g[r] & (4 >> b)) {
                    uint32_t *p = pix + (y + r) * FBW + x + c * 5 + b;
                    p[0] = mkc(200, 220, 255); p[FBW] = mkc(200, 220, 255);
                }
    }
}

int main(void)
{
    debug_init_emulog();
    printf("[n64] pre-init\n");
    init_colors();
    display_init(RESOLUTION_320x240, DEPTH_32_BPP, 1, GAMMA_NONE, FILTERS_DISABLED);
    timer_init();
    joypad_init();

#ifdef USE_RSP
    rsp_init();
    rsp_load(&rsp_sand64);
#endif
    build_terrain();

    int cx = GW / 2, cy = GH / 2, mat = M_SAND;
    const int mats[] = { M_SAND, M_WATER, M_SEED, M_FIRE, M_WALL, M_AIR };

    printf("[n64] sand64 boot: grid %dx%d scale %d seed %08lX autotest %d\n",
           GW, GH, SCALE, RNGSEED,
#ifdef AUTOTEST
           1
#else
           0
#endif
    );

    long long t_fps0 = TIMER_MICROS_LL(timer_ticks());
    int fps_n = 0;

    while (1) {
        surface_t *fb = display_get();
        uint32_t *pix = (uint32_t *)fb->buffer;

        long long t0 = TIMER_MICROS_LL(timer_ticks());

        /* ---- input ---- */
        joypad_inputs_t jin = joypad_get_inputs(JOYPAD_PORT_1);
        int bx = jin.btn.raw;
        const int NM = (int)(sizeof mats / sizeof mats[0]); (void)NM;
#ifdef DICK
        /* dick mode: pixel-cock carved in wall, sand gushes from the tip */
        {
            int t = frame_no;
            if (t == 2) {
                /* shaft: horizontal capsule x 34..94, cy 40..54 */
                for (int y = 40; y <= 54; y++)
                    for (int x = 40; x <= 88; x++) set_cell(x, y, M_WALL);
                /* glans: rounded dome left */
                for (int y = 38; y <= 56; y++)
                    for (int x = 30; x <= 44; x++) {
                        int dx = x - 42, dy = y - 47;
                        if (dx * dx + dy * dy <= 64 && x <= 42) set_cell(x, y, M_WALL);
                    }
                /* tip rim + meatus right */
                for (int y = 42; y <= 52; y++) set_cell(89, y, M_WALL);
                set_cell(90, 46, M_WALL); set_cell(90, 47, M_WALL);
                set_cell(90, 48, M_WALL); set_cell(90, 49, M_WALL);
                /* balls */
                for (int y = 56; y <= 68; y++)
                    for (int x = 46; x <= 58; x++) {
                        int dx = x - 52, dy = y - 62;
                        if (dx * dx + dy * dy <= 42) set_cell(x, y, M_WALL);
                    }
                for (int y = 56; y <= 68; y++)
                    for (int x = 64; x <= 76; x++) {
                        int dx = x - 70, dy = y - 62;
                        if (dx * dx + dy * dy <= 42) set_cell(x, y, M_WALL);
                    }
                probe("[dick] drawn");
            }
            /* sand gush from the tip from f=10 onward, 3 streams with jitter */
            if (t >= 10) {
                for (int k = 0; k < 4; k++) {
                    int rx = 91 + (int)(xrnd() % 5);
                    set_cell(rx, 47 + (int)(xrnd() % 3), M_SAND);
                }
            }
            /* stop gushing + reveal full pile near the end */
        }
#endif
#ifdef DICK
        /* scripted cursor off in dick mode */
#endif
#if defined(AUTOTEST) && !defined(DICK)
        /* scripted: bouncing cursor painting; occasional material switch + rain */
        {
            int t = frame_no;
#ifdef AUTOTEST
            g_wA = water_cnt(); g_eA = erase_water; g_sA = spawn_water;
#endif
            cx = 24 + ((t * 3) % (GW - 48));
            cy = 20 + ((t * 5 + (t / (GW - 48)) * 7) % (GH - 40));
            if (((t / 137) % 4) == 0) paint(cx, cy, M_FIRE);
            else paint(cx, cy, ((t / 61) & 1) ? M_SAND : M_WATER);
            if ((t % 240) < 40) {
                int rx = 1 + (int)(xrnd() % (GW - 2));
                set_cell(rx, 2, M_WATER);
            }
            if ((t % 500) == 1) { int rx = 20 + (int)(xrnd() % (GW - 40)); paint(rx, 30, M_SEED); }
        }
#ifdef AUTOTEST
        { int wB = water_cnt();
          int u = (wB - g_wA) + (int)(erase_water - g_eA) - (int)(spawn_water - g_sA);
          if (u) { char pl[140]; snprintf(pl, sizeof pl,
              "[pdbg f%d] paint_unbal=%d (w %d->%d sp=%ld er=%ld)",
              frame_no, u, g_wA, wB, spawn_water - g_sA, erase_water - g_eA); probe(pl); }
        }
#endif
#endif
#if !defined(AUTOTEST) && !defined(DICK)
        {
            int sx = joypad_get_axis(JOYPAD_AXIS_STICK_X);
            int sy = joypad_get_axis(JOYPAD_AXIS_STICK_Y);
            if (sx > 2000 || sx < -2000) cx += sx > 0 ? 2 : -2;
            if (sy > 2000 || sy < -2000) cy += sy > 0 ? 2 : -2;
            if (bx & BUTTON_D_LEFT) cx -= 2;
            if (bx & BUTTON_D_RIGHT) cx += 2;
            if (bx & BUTTON_D_UP) cy -= 2;
            if (bx & BUTTON_D_DOWN) cy += 2;
            if (cx < 1) cx = 1; if (cx > GW - 2) cx = GW - 2;
            if (cy < 1) cy = 1; if (cy > GH - 2) cy = GH - 2;
            if (bx & BUTTON_A) paint(cx, cy, mat);
            if (bx & BUTTON_B) paint(cx, cy, M_AIR);
            if (bx & BUTTON_L) paint(cx, cy, M_WALL);
            if (bx & BUTTON_R) paint(cx, cy, M_FIRE);
            if (bx & BUTTON_C_LEFT)  mat = mats[(mat + NM - 1) % NM];
            if (bx & BUTTON_C_RIGHT) mat = mats[(mat + 1) % NM];
        }
#endif
        step();
#ifdef SLOWHOLD
        /* long CPU frame: keeps the FINISHED buffer on the VI for ~15 s so
           headless screenshots can't race a mid-paint rewrite */
        if (frame_no == 400)
            for (int hs = 0; hs < 10000; hs++) step();
#endif

        us_sim_total += TIMER_MICROS_LL(timer_ticks()) - t0;

        render(pix, cx, cy, mat);
        /* HUD numbers: frame, sand count, water count */
        {
            uint32_t *p = pix;
            for (int x = 0; x < FBW; x++) p[x] = mkc(20, 20, 30);
            draw_num(pix, 4, 1, frame_no);
            recount();
            draw_num(pix, 60, 1, cnt[M_SAND]);
            draw_num(pix, 130, 1, cnt[M_WATER]);
            draw_num(pix, 200, 1, cnt[M_FIRE]);
        }

        /* probe every 30 frames */
        if ((frame_no % 30) == 0) {
            char line[320];
            long cons_sand  = spawn_sand  - erase_sand  - cnt[M_SAND];
            long cons_water = spawn_water - erase_water - cnt[M_WATER];
            int n = snprintf(line, sizeof line,
                "[probe] f=%d seed=%08lX sand=%ld water=%ld fire=%ld plant=%ld "
                "smoke=%ld spawn_s=%ld spawn_w=%ld cons_s=%ld cons_w=%ld sim_us=%lld fps1000=%ld btn=%04X fold=%016llX rsp=%ld\n",
                frame_no, RNGSEED, cnt[M_SAND], cnt[M_WATER], cnt[M_FIRE],
                cnt[M_PLANT], cnt[M_SMOKE], spawn_sand, spawn_water,
                cons_sand, cons_water, us_sim_total / (frame_no + 1), fps1000, bx, (unsigned long long)grid_fold(),
#ifdef USE_RSP
                rsp_chunks
#else
                0L
#endif
                );
            if (n > 0) probe(line);
        }

        /* debug strip: 8 px at (200,3): bit pattern encodes probe health */
        {
            int ok = (io_read(0x13FF0000) == 0x69737677);
            uint32_t dbg = (ok ? 0xFF0000 : 0) | 0x00FF00; /* R=ISV magic ok, G=alive */
            for (int d = 0; d < 8; d++) pix[3 * FBW + 200 + d] = dbg;
        }

        /* debug strip: 8 swatches at FB (100..107, 3): R, G, B, W, sand,
           water, wall, fire — sample from shot to derive true VI mapping */
        {
            uint32_t sw[8] = {
                mkc(255, 0, 0), mkc(0, 255, 0), mkc(0, 0, 255), mkc(255, 255, 255),
                mcol[M_SAND], mcol[M_WATER], mcol[M_WALL], mcol[M_FIRE]
            };
            for (int d = 0; d < 8; d++) pix[3 * FBW + 100 + d] = sw[d];
        }

        display_show(fb);

        fps_n++;
        long long now = TIMER_MICROS_LL(timer_ticks());
        if (now - t_fps0 > 500000) {
            fps1000 = fps_n * 1000000LL / (now - t_fps0);
            t_fps0 = now; fps_n = 0;
        }
        frame_no++;
    }
}