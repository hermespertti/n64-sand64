/* micro: probe unaligned lqv byte-stitch semantics via ISViewer log */
#include <libdragon.h>
#include <rsp.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

DEFINE_RSP_UCODE(rsp_micro);

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

static uint8_t dmem_in[128] __attribute__((aligned(16)));
static uint8_t dmem_out[256] __attribute__((aligned(16)));

int main(void)
{
    debug_init_emulog();
    display_init(RESOLUTION_320x240, DEPTH_32_BPP, 1, GAMMA_NONE, FILTERS_DISABLED);
    for (int i = 0; i < 128; i++) dmem_in[i] = (uint8_t)(i + 1);
    data_cache_hit_writeback_invalidate(dmem_in, sizeof dmem_in);
    rsp_load(&rsp_micro);
    rsp_load_data(dmem_in, 128, 0x0000);
    { static uint8_t echo[32] __attribute__((aligned(16)));
      rsp_read_data(echo, 16, 0x0000);
      data_cache_hit_writeback_invalidate(echo, 32);
      char el[100]; int o = snprintf(el, sizeof el, "[mic] echo:");
      for (int i = 0; i < 16 && o < 90; i++) o += snprintf(el+o, sizeof el-o, " %u", echo[i]);
      probe(el);
    }
    { static uint8_t pre[96] __attribute__((aligned(16)));
      memset(pre, 0xAA, sizeof pre);
      rsp_load_data(pre, 96, 0x0400);
    }
    rsp_run();
    rsp_read_data(dmem_out, 144, 0x0100);
    { char el[100]; int o = snprintf(el, sizeof el, "[mic] st:");
      for (int i = 192; i < 208 && o < 90; i++) o += snprintf(el+o, sizeof el-o, " %u", dmem_out[i]);
      probe(el);
      o = snprintf(el, sizeof el, "[mic] st2:");
      for (int i = 208; i < 224 && o < 90; i++) o += snprintf(el+o, sizeof el-o, " %u", dmem_out[i]);
      probe(el); }
    data_cache_hit_writeback_invalidate(dmem_out, 144);
    char ln[200];
    int off[] = {0,16,32,48,64,80,96,112};
    const char *nm[] = {"t1","t2","t3","t4","t5","t6","t7","t8"};
    for (int t = 0; t < 8; t++) {
        int o = snprintf(ln, sizeof ln, "[mic] %s:", nm[t]);
        for (int i = 0; i < 16 && o < 180; i++) o += snprintf(ln+o, sizeof ln-o, " %u", dmem_out[off[t]+i]);
        probe(ln);
    }
    { static uint8_t st[96] __attribute__((aligned(16)));
      rsp_read_data(st, 96, 0x0400);
      data_cache_hit_writeback_invalidate(st, 96);
      const char *nm[] = {"sa","sa2","sb","sb2","sc","sc2"};
      int off[] = {0,16,64,80,128-64,144-64}; (void)nm;
      char el[150]; int o = snprintf(el, sizeof el, "[mic] sa:");
      for (int i = 0; i < 32 && o < 140; i++) o += snprintf(el+o, sizeof el-o, " %u", st[i]);
      probe(el);
      o = snprintf(el, sizeof el, "[mic] sb:");
      for (int i = 64; i < 96 && o < 140; i++) o += snprintf(el+o, sizeof el-o, " %u", st[i]);
      probe(el); }
    probe("[mic] done");
    for (;;) {}
    return 0;
}
