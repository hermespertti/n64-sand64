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
