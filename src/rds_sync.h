#pragma once
/* Bitstrom -> RDS-Bloecke (26 Bit, CRC-Syndrom) -> Gruppen (4 Bloecke) */
#include <stdint.h>
#include "rds_config.h"

typedef struct {
    uint16_t blk[4];   /* A, B, C/C', D */
    uint8_t  st[4];    /* 0 = Fehler/fehlt, 1 = ok, 2 = per Burstkorrektur repariert */
    uint8_t  ver_b;    /* Gruppenversion B (Block 3 = C') */
} rds_group_t;

typedef void (*rds_group_cb)(void *ctx, const rds_group_t *g);

typedef struct {
    uint32_t reg, nbit, last_pos;
    int8_t   last_type;
    uint8_t  synced, blk, cnt, consec_bad;
    rds_group_t grp;
    rds_group_cb cb; void *ctx;
    /* Statistik */
    uint32_t blocks_ok, blocks_fixed, blocks_bad, groups, sync_losses;
    uint8_t  hist[50]; uint8_t hpos; uint8_t hbad;   /* letzte 50 Bloecke */
} rds_sync_t;

void rds_sync_init(rds_sync_t *s, rds_group_cb cb, void *ctx);
void rds_sync_bit(rds_sync_t *s, uint8_t bit);
