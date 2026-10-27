#pragma once
/* RDS-Gruppen -> Senderdaten (PI, PS, PTY, RT, RT+, CT, ECC) und Textausgabe */
#include <stdint.h>
#include <stdbool.h>
#include "rds_sync.h"
#include "rds_dsp.h"

typedef struct {
    uint16_t pi, pi_cand; uint8_t pi_valid, pi_cnt;
    uint8_t  ps[8]; uint8_t ps_mask; uint8_t ps_shown[8]; bool ps_complete;
    uint8_t  pty, tp, ta, ms, ecc, ecc_valid;
    /* RadioText */
    uint8_t  rt[64]; uint16_t rt_mask; uint8_t rt_ab, rt_ab_valid, rt_end; bool rt_b;
    uint8_t  rt_shown[64]; uint8_t rt_shown_len; bool rt_complete;
    /* RT+ */
    uint8_t  rtp_gtype, rtp_known, rtp_toggle, rtp_running;
    struct { uint8_t type, start, len, valid; } tag[2];
    /* Uhr (UTC + Offset) */
    bool     ct_valid; uint32_t ct_mjd; uint8_t ct_h, ct_m; int8_t ct_off_half; uint32_t ct_rx_ms;
    uint32_t groups_seen, groups_by_type[32];
} rds_parser_t;

void rds_parser_init(rds_parser_t *p);
void rds_parser_feed(rds_parser_t *p, const rds_group_t *g, uint32_t now_ms);

/* Gruppe als Rohbloecke ausgeben (eine Zeile) */
void rds_print_group(const rds_group_t *g);

typedef struct {
    rds_dsp_stats_t dsp; uint8_t synced;
    uint32_t blocks_ok, blocks_fixed, blocks_bad, groups, sync_losses;
} rds_link_stats_t;

/* Statusblock: Empfang, Sender, Uhrzeit, Titel */
void rds_print_status(const rds_parser_t *p, const rds_link_stats_t *l, uint32_t now_ms);
