import numpy as np

GW, GH = 160, 116
AIR, WALL, SAND, WATER, SEED, PLANT, FIRE, SMOKE = range(8)

def terrain():
    g = np.zeros((GH, GW), np.uint8)
    g[GH-1, :] = WALL
    for y in range(2, GH-1):
        g[y, 0] = WALL; g[y, GW-1] = WALL
    for x in range(20, 60):  g[70 + (x-20)//2, x] = WALL
    for x in range(100,140): g[70 + (139-x)//2, x] = WALL
    for x in range(60, 100): g[104, x] = WALL
    for x in range(4, 12):   g[90, x] = WALL
    return g

class RNG:
    def __init__(s, seed=0x1234abcd): s.s = seed
    def next(s):
        x = s.s
        x ^= (x << 13) & 0xFFFFFFFF; x ^= (x >> 17); x ^= (x << 5) & 0xFFFFFFFF
        s.s = x; return x

def wet(g, y, x):
    return bool((g[max(0,y-1):y+2, max(0,x-1):x+2] == WATER).any())

class Sand:
    def __init__(s):
        s.g = terrain(); s.rng = RNG(); s.f = 0
        s.spawn = {SAND:0, WATER:0, SEED:0, PLANT:0, SMOKE:0}
        s.counts = {m:int((s.g==m).sum()) for m in range(8)}

    def grain_phase(s):
        g = s.g
        flip = (s.f % 20) < 10
        moved = np.zeros((GH, GW), bool)
        for x in range(GW):
            for y in range(GH-2, 0, -1):
                if moved[y, x]: continue
                m = g[y, x]
                if m in (SAND, SEED):
                    b = g[y+1, x]
                    if b == AIR and not moved[y+1, x]:
                        g[y+1, x] = m; g[y, x] = AIR
                        moved[y+1, x] = True; continue
                    if m == SAND and b in (WATER, SEED) and not moved[y+1, x]:
                        g[y, x], g[y+1, x] = b, m
                        moved[y+1, x] = True; continue
                if m == WATER:
                    if g[y+1, x] == AIR and not moved[y+1, x]:
                        g[y+1, x] = WATER; g[y, x] = AIR
                        moved[y+1, x] = True; continue
                if m in (SAND, SEED) and (m == SEED or wet(g, y, x)):
                    nx = None
                    for dx in ((-1, 1) if not flip else (1, -1)):
                        c = x + dx
                        if 1 <= c < GW-1 and not moved[y+1, c]:
                            d = g[y+1, c]
                            if d == AIR: nx = c; break
                            if m == SAND and d in (WATER, SEED): nx = c; break
                    if nx is not None:
                        d = g[y+1, nx]
                        if d == AIR: g[y+1, nx] = m; g[y, x] = AIR
                        else: g[y+1, nx], g[y, x] = m, d
                        moved[y+1, nx] = True; continue
                if m in (SAND, WATER, SEED):
                    step = 1 if not flip else -1
                    c = x + step
                    if 1 <= c < GW-1 and g[y, c] == AIR and not moved[y, c]:
                        g[y, c] = m; g[y, x] = AIR; moved[y, c] = True

    def transforms(s):
        g = s.g
        for y in range(1, GH-1):
            for x in range(1, GW-1):
                if g[y, x] == SEED and wet(g, y, x):
                    g[y, x] = PLANT; s.spawn[PLANT] += 1
        for y in range(1, GH-1):
            for x in range(1, GW-1):
                if g[y, x] == PLANT:
                    if not (g[max(0,y-4):y+5, max(0,x-2):x+3] == WATER).any():
                        g[y, x] = AIR

    def fire_phase(s):
        g = s.g
        for y in range(1, GH-1):
            for x in range(1, GW-1):
                if g[y, x] == FIRE:
                    w = g[y-1:y+2, x-1:x+2]
                    if (w == WATER).any():
                        g[y, x] = SMOKE; s.spawn[SMOKE] += 1
                        w[w == WATER] = AIR
                        continue
                    r = s.rng.next()
                    if r % 6 == 0: g[y, x] = SMOKE; s.spawn[SMOKE] += 1
                    elif r % 3 == 0:
                        for dy in (-1, 0, 1):
                            for dx in (-1, 0, 1):
                                if g[y+dy, x+dx] == PLANT: g[y+dy, x+dx] = FIRE
        for y in range(2, GH):
            for x in range(1, GW-1):
                if g[y, x] == SMOKE:
                    if g[y-1, x] == AIR: g[y-1, x], g[y, x] = SMOKE, AIR
                    elif s.rng.next() % 32 == 0: g[y, x] = AIR

    def rain(s):
        n = s.rng.next()
        for i in range(n % 6):
            x = 3 + (s.rng.next() % (GW-6)); y = 2 + (s.rng.next() % 6)
            if s.g[y, x] == AIR: s.g[y, x] = WATER; s.spawn[WATER] += 1; s.counts[WATER] += 1
        if s.f % 37 == 0:
            x = 3 + (s.rng.next() % (GW-6))
            if s.g[2, x] == AIR: s.g[2, x] = SAND; s.spawn[SAND] += 1; s.counts[SAND] += 1
        if s.f % 53 == 5:
            x = 3 + (s.rng.next() % (GW-6))
            if s.g[2, x] == AIR: s.g[2, x] = SEED; s.spawn[SEED] += 1; s.counts[SEED] += 1

    def audit(s):
        return s.counts[SAND]-s.spawn[SAND], s.counts[WATER]-s.spawn[WATER]

    def fold(s):
        h = 0xcbf29ce484222325
        for b in s.g.reshape(-1):
            h = ((h ^ int(b)) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
        return h

    def step(s):
        s.grain_phase(); s.transforms(); s.fire_phase(); s.rain()
        s.f += 1

if __name__ == "__main__":
    import sys
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    s = Sand(); bad = 0
    for i in range(frames):
        s.step()
        cs, cw = s.audit()
        if cs or cw: bad += 1
        if i % 30 == 0 or i == frames-1:
            print(f"f={i} sand={s.counts[SAND]} water={s.counts[WATER]} "
                  f"plant={s.counts[PLANT]} fire={s.counts[FIRE]} fold={s.fold():016X} cons_s={cs} cons_w={cw}")
    print("cons violations:", bad)
