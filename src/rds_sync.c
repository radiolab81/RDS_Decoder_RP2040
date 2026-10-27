#include "rds_sync.h"
#include <string.h>

#define POLY 0x5B9u                       /* x^10+x^8+x^7+x^5+x^4+x^3+1 */
static const uint16_t OFFS[5] = { 0x0FC, 0x198, 0x168, 0x350, 0x1B4 }; /* A B C C' D */

static uint32_t fec_pat[1024];            /* Fehlermuster zu Syndrom (0 = keins) */
static uint8_t  fec_ready;

static uint16_t rem26(uint32_t w)
{
    uint32_t r = 0;
    for (int i = 25; i >= 0; i--) {
        r = (r << 1) | ((w >> i) & 1u);
        if (r & 0x400u) r ^= POLY;
    }
    return (uint16_t)r;
}

static void fec_build(void)
{
    memset(fec_pat, 0, sizeof fec_pat);
    static uint8_t amb[1024];
    memset(amb, 0, sizeof amb);
    for (int len = 1; len <= RDS_FEC_MAX_BURST; len++) {
        for (uint32_t mid = 0; mid < (1u << (len > 1 ? len - 2 : 0)); mid++) {
            uint32_t pat = 1u | (len > 1 ? ((mid << 1) | (1u << (len - 1))) : 0u);
            for (int p = 0; p + len <= 26; p++) {
                uint32_t e = pat << p;
                uint16_t s = rem26(e);
                if (fec_pat[s] && fec_pat[s] != e) amb[s] = 1;
                fec_pat[s] = e;
            }
        }
    }
    for (int i = 0; i < 1024; i++) if (amb[i]) fec_pat[i] = 0;
    fec_pat[0] = 0;
    fec_ready = 1;
}

void rds_sync_init(rds_sync_t *s, rds_group_cb cb, void *ctx)
{
    memset(s, 0, sizeof *s);
    s->cb = cb; s->ctx = ctx; s->last_type = -1;
    if (!fec_ready) fec_build();
}

static void hist_push(rds_sync_t *s, uint8_t bad)
{
    s->hbad -= s->hist[s->hpos];
    s->hist[s->hpos] = bad; s->hbad += bad;
    s->hpos = (uint8_t)((s->hpos + 1) % 50);
}

/* Block bei erwarteter Position auswerten. Rueckgabe: Status (0/1/2), setzt *ver_b */
static uint8_t check_block(rds_sync_t *s, uint32_t y, uint16_t *data)
{
    uint16_t sy = rem26(y);
    int pos = s->blk;
    int cand[2], nc = 0;
    if (pos == 2) {
        if (s->grp.st[1]) { cand[nc++] = s->grp.ver_b ? 3 : 2; }
        else { cand[nc++] = 2; cand[nc++] = 3; }
    } else cand[nc++] = (pos == 3) ? 4 : pos;
    for (int k = 0; k < nc; k++) {
        if (sy == OFFS[cand[k]]) {
            *data = (uint16_t)(y >> 10);
            if (pos == 2 && !s->grp.st[1]) s->grp.ver_b = (cand[k] == 3);
            return 1;
        }
    }
#if RDS_FEC_MAX_BURST > 0
    for (int k = 0; k < nc; k++) {
        uint32_t e = fec_pat[sy ^ OFFS[cand[k]]];
        if (e) {
            *data = (uint16_t)((y ^ e) >> 10);
            return 2;
        }
    }
#endif
    *data = (uint16_t)(y >> 10);
    return 0;
}

void rds_sync_bit(rds_sync_t *s, uint8_t bit)
{
    s->reg = ((s->reg << 1) | (bit & 1u)) & 0x3FFFFFFu;
    s->nbit++;
    if (s->nbit < 26) return;

    if (!s->synced) {
        uint16_t sy = rem26(s->reg);
        for (int t = 0; t < 5; t++) {
            if (sy != OFFS[t]) continue;
            int p = (t == 3) ? 2 : (t == 4) ? 3 : t;
            if (s->last_type >= 0 && s->nbit - s->last_pos == 26 && p == ((s->last_type + 1) & 3)) {
                s->synced = 1; s->blk = (uint8_t)((p + 1) & 3); s->cnt = 26;
                s->consec_bad = 0;
                memset(&s->grp, 0, sizeof s->grp);
                memset(s->hist, 0, sizeof s->hist); s->hbad = 0;
            }
            s->last_type = (int8_t)p; s->last_pos = s->nbit;
            break;
        }
        return;
    }

    if (--s->cnt) return;
    s->cnt = 26;
    uint16_t data;
    uint8_t st = check_block(s, s->reg, &data);
    s->grp.blk[s->blk] = data; s->grp.st[s->blk] = st;
    if (s->blk == 1 && st) s->grp.ver_b = (data >> 11) & 1u;
    if (st == 1) s->blocks_ok++; else if (st == 2) s->blocks_fixed++; else s->blocks_bad++;
    hist_push(s, st == 0);
    s->consec_bad = st ? 0 : (uint8_t)(s->consec_bad + 1);

    if (s->blk == 3) {
        s->groups++;
        if (s->cb) s->cb(s->ctx, &s->grp);
        memset(&s->grp, 0, sizeof s->grp);
    }
    s->blk = (uint8_t)((s->blk + 1) & 3);

    if (s->consec_bad >= 10 || s->hbad > 35) {
        s->synced = 0; s->last_type = -1; s->sync_losses++;
        memset(&s->grp, 0, sizeof s->grp);
    }
}
