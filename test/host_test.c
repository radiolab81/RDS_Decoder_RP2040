/* Host-Test: liest rohe uint16-ADC-Samples (228 kS/s), laeuft durch die komplette Kette. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/rds_dsp.h"
#include "../src/rds_sync.h"
#include "../src/rds_parser.h"

static rds_dsp_t dsp; static rds_sync_t sync_; static rds_parser_t par;
static uint32_t now_ms; static int verbose;
static FILE *bitf;

static void on_bit(void *c, uint8_t b) { (void)c; rds_sync_bit(&sync_, b); if (bitf) fputc('0'+b, bitf); }
static void on_group(void *c, const rds_group_t *g) { (void)c; if (verbose) rds_print_group(g); rds_parser_feed(&par, g, now_ms); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s samples.u16 [-v] [-b bits.txt]\n", argv[0]); return 1; }
    for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "-v")) verbose = 1; if (!strcmp(argv[i], "-b") && i + 1 < argc) bitf = fopen(argv[++i], "w"); }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("open"); return 1; }
    rds_dsp_init(&dsp, on_bit, NULL); rds_sync_init(&sync_, on_group, NULL); rds_parser_init(&par);
    static uint16_t buf[RDS_BLOCK_SAMPLES]; size_t n; uint64_t total = 0; uint32_t next = RDS_STATUS_INTERVAL_MS;
    while ((n = fread(buf, 2, RDS_BLOCK_SAMPLES, f)) == RDS_BLOCK_SAMPLES) {
        rds_dsp_process(&dsp, buf, n); total += n;
        now_ms = (uint32_t)(total * 1000ull / RDS_ADC_FS_HZ);
        if (now_ms >= next) {
            next += RDS_STATUS_INTERVAL_MS;
            rds_link_stats_t l = { .synced = sync_.synced, .blocks_ok = sync_.blocks_ok, .blocks_fixed = sync_.blocks_fixed,
                .blocks_bad = sync_.blocks_bad, .groups = sync_.groups, .sync_losses = sync_.sync_losses };
            rds_dsp_get_stats(&dsp, &l.dsp, 1);
            rds_print_status(&par, &l, now_ms);
        }
    }
    rds_link_stats_t l = { .synced = sync_.synced, .blocks_ok = sync_.blocks_ok, .blocks_fixed = sync_.blocks_fixed,
        .blocks_bad = sync_.blocks_bad, .groups = sync_.groups, .sync_losses = sync_.sync_losses };
    rds_dsp_get_stats(&dsp, &l.dsp, 1);
    printf("\n--- ENDE ---"); rds_print_status(&par, &l, now_ms);
    return 0;
}
