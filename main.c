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

static uint8_t G  [GS] __attribute__((aligned(16)));   /* material plane */
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
    memset(G, M_AIR, GS);
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

/* ---------- physics ---------- */
static inline int idx(int x, int y) { return y * GW + x; }

/* ---------- v2 step: column-major phases (RSP-portable order) ----------
 * column-major x=1..GW-2 (chunk order), y bottom-up, moved bitmap => every
 * grain moves AT MOST once per frame (v1 let grains slide many cells).
 * Direction randomness is pure hash(x,y,frame) — no xrnd() in the grain
 * phase — so the RSP can recompute it bit-exactly. xrnd() is consumed only
 * by CPU phases (sprout/ignite/burnout/rain), identical in both builds.
 * Phases: grain move -> transform -> fire -> smoke. Fold probe = FNV-1a(G||TL).
 */
static uint8_t MOVED[GS] __attribute__((aligned(16)));

static uint32_t jhash(int x, int y, int salt)
{
    uint32_t h = (uint32_t)x * 2654435761u
               + (uint32_t)y * 2246822519u
               + (uint32_t)frame_no * 3266489917u
               + (uint32_t)salt * 668265263u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return h;
}

static int wet3(int x, int y)
{
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (G[idx(x + dx, y + dy)] == M_WATER) return 1;
    return 0;
}

static void grain_phase(int flip)
{
    memset(MOVED, 0, GS);
    for (int x = 1; x < GW - 1; x++) {
        int d = flip ? 1 : -1;                     /* diagonal/horiz priority */
        for (int y = GH - 2; y >= 1; y--) {
            int i = idx(x, y);
            if (MOVED[i]) continue;
            uint8_t m = G[i];
            if (m == M_AIR || m == M_WALL) continue;

            if (m == M_SAND || m == M_SEED || m == M_WATER) {
                /* down: AIR, or swap with WATER (sand/seed only) */
                int dn = idx(x, y + 1);
                if (G[dn] == M_AIR) {
                    G[dn] = m; G[i] = M_AIR; MOVED[dn] = 1; continue;
                }
                if (m != M_WATER && G[dn] == M_WATER) {
                    G[dn] = m; G[i] = M_WATER; MOVED[dn] = 1; continue;
                }
                /* diagonals: AIR (all) or WATER swap (sand/seed) */
                int a = idx(x + d, y + 1);
                if (G[a] == M_AIR) {
                    G[a] = m; G[i] = M_AIR; MOVED[a] = 1; continue;
                }
                if (m != M_WATER && G[a] == M_WATER) {
                    G[a] = m; G[i] = M_WATER; MOVED[a] = 1; continue;
                }
                int b = idx(x - d, y + 1);
                if (G[b] == M_AIR) {
                    G[b] = m; G[i] = M_AIR; MOVED[b] = 1; continue;
                }
                if (m != M_WATER && G[b] == M_WATER) {
                    G[b] = m; G[i] = M_WATER; MOVED[b] = 1; continue;
                }
                /* horizontal one cell, AIR only (leveling) */
                int h = (jhash(x, y, 7) >> 24) & 1 ? d : -d;
                int c = idx(x + h, y);
                if (G[c] == M_AIR && !MOVED[c]) {
                    G[c] = m; G[i] = M_AIR; MOVED[c] = 1; continue;
                }
            }
        }
    }
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

static void smoke_phase(void)
{
    for (int x = 1; x < GW - 1; x++)
        for (int y = GH - 2; y >= 1; y--) {
            int i = idx(x, y);
            if (G[i] != M_SMOKE) continue;
            if (--TL[i] == 0) { G[i] = M_AIR; continue; }
            if ((frame_no & 1) == (y & 1)) {       /* parity gate = 1 move/frame max */
                int u = idx(x, y - 1);
                if (G[u] == M_AIR) { G[u] = M_SMOKE; G[i] = M_AIR; TL[u] = TL[i]; continue; }
                int h = (jhash(x, y, 11) >> 16) & 1 ? 1 : -1;
                int a = idx(x + h, y - 1);
                if (G[a] == M_AIR) { G[a] = M_SMOKE; G[i] = M_AIR; TL[a] = TL[i]; }
            }
        }
}

static void step(void)
{
    int flip = frame_no & 1;
    grain_phase(flip);
    transform_phase();
    fire_phase();
    smoke_phase();
}


static uint64_t grid_fold(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (long i = 0; i < GS; i++)  { h ^= G[i];  h *= 0x100000001b3ull; }
    for (long i = 0; i < GS; i++)  { h ^= TL[i]; h *= 0x100000001b3ull; }
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
        const uint8_t *grow = G + gy * GW;
        for (int gx = 0; gx < GW; gx++) {
            uint8_t m = grow[gx];
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
#ifdef AUTOTEST
        /* scripted: bouncing cursor painting; occasional material switch + rain */
        {
            int t = frame_no;
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
#else
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
                "smoke=%ld spawn_s=%ld spawn_w=%ld cons_s=%ld cons_w=%ld sim_us=%lld fps1000=%ld btn=%04X fold=%016llX\n",
                frame_no, RNGSEED, cnt[M_SAND], cnt[M_WATER], cnt[M_FIRE],
                cnt[M_PLANT], cnt[M_SMOKE], spawn_sand, spawn_water,
                cons_sand, cons_water, us_sim_total / (frame_no + 1), fps1000, bx, (unsigned long long)grid_fold());
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
