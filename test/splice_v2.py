import sys
path = '/home/lex/n64/sand64/main.c'
src = open(path).read()
v2 = open('/home/lex/.hermes/cache/scratch/step_v2.c').read()

start = src.index('static void step(void)')
end = src.index('/* ---------- ISViewer probe')
old = src[start:end]
assert 'l2r' in old, "old step not bounded correctly"
src2 = src[:start] + v2 + '\n' + src[end:]

fnv = '''
static uint64_t grid_fold(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (long i = 0; i < GS; i++)  { h ^= G[i];  h *= 0x100000001b3ull; }
    for (long i = 0; i < GS; i++)  { h ^= TL[i]; h *= 0x100000001b3ull; }
    return h;
}

'''
anchor = '/* ---------- ISViewer probe'
assert src2.count(anchor) == 1
src2 = src2.replace(anchor, fnv + anchor, 1)

recount_def = '''
static void recount(void)
{
    memset(cnt, 0, sizeof cnt);
    for (int i = 0; i < GS; i++) cnt[G[i]]++;
}

'''
src2 = src2.replace(anchor, recount_def + anchor, 1)

o1 = '"smoke=%ld spawn_s=%ld spawn_w=%ld cons_s=%ld cons_w=%ld sim_us=%lld fps1000=%ld btn=%04X\\n",'
n1 = '"smoke=%ld spawn_s=%ld spawn_w=%ld cons_s=%ld cons_w=%ld sim_us=%lld fps1000=%ld btn=%04X fold=%016llX\\n",'
assert src2.count(o1) == 1
src2 = src2.replace(o1, n1, 1)

o2 = 'cons_sand, cons_water, us_sim_total / (frame_no + 1), fps1000, bx);'
n2 = 'cons_sand, cons_water, us_sim_total / (frame_no + 1), fps1000, bx, (unsigned long long)grid_fold());'
assert src2.count(o2) == 1
src2 = src2.replace(o2, n2, 1)

open(path, 'w').write(src2)
print("spliced OK, new len", len(src2))
