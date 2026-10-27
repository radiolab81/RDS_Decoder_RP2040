#include "rds_parser.h"
#include <stdio.h>
#include <string.h>

/* RDS-Zeichensatz G0, obere Haelfte (0x80..0xFF) als UTF-8 */
static const char *const g0_hi[128] = {
 "á","à","é","è","í","ì","ó","ò","ú","ù","Ñ","Ç","Ş","ß","¡","Ĳ",
 "â","ä","ê","ë","î","ï","ô","ö","û","ü","ñ","ç","ş","ǧ","ı","ĳ",
 "ª","α","©","‰","Ǧ","ě","ň","ő","π","€","£","$","←","↑","→","↓",
 "º","¹","²","³","±","İ","ń","ű","µ","¿","÷","°","¼","½","¾","§",
 "Á","À","É","È","Í","Ì","Ó","Ò","Ú","Ù","Ř","Č","Š","Ž","Ð","Ŀ",
 "Â","Ä","Ê","Ë","Î","Ï","Ô","Ö","Û","Ü","ř","č","š","ž","đ","ŀ",
 "Ã","Å","Æ","Œ","ŷ","Ý","Õ","Ø","Þ","Ŋ","Ŕ","Ć","Ś","Ź","Ŧ","ð",
 "ã","å","æ","œ","ŵ","ý","õ","ø","þ","ŋ","ŕ","ć","ś","ź","ŧ"," " };

static const char *const pty_names[32] = {
 "keine","Nachrichten","Aktuelles","Information","Sport","Bildung","Hoerspiel","Kultur",
 "Wissenschaft","Vermischtes","Pop","Rock","Easy Listening","Leichte Klassik","Ernste Klassik",
 "Sonstige Musik","Wetter","Wirtschaft","Kinder","Soziales","Religion","Anruf-Sendung","Reisen",
 "Freizeit","Jazz","Country","Nationale Musik","Oldies","Folk","Dokumentation","Alarmtest","Alarm" };

/* Ein RDS-Zeichen als UTF-8 in out anhaengen, Rueckgabe: neue Position */
static size_t put_char(char *out, size_t pos, size_t max, uint8_t c)
{
    const char *s; char tmp[2] = {0, 0};
    if (c >= 0x80) s = g0_hi[c - 0x80];
    else if (c < 0x20 || c == 0x7F) s = " ";
    else { tmp[0] = (char)c; s = tmp; }
    size_t l = strlen(s);
    if (pos + l >= max) return pos;
    memcpy(out + pos, s, l);
    return pos + l;
}

static void to_utf8(const uint8_t *src, size_t start, size_t len, char *out, size_t max)
{
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) pos = put_char(out, pos, max, src[start + i]);
    while (pos && out[pos - 1] == ' ') pos--;
    out[pos] = 0;
}

void rds_parser_init(rds_parser_t *p)
{
    memset(p, 0, sizeof *p);
    memset(p->ps, ' ', 8); memset(p->rt, ' ', 64);
    p->rt_end = 0xFF;
}

static void reset_station(rds_parser_t *p, uint16_t pi)
{
    uint32_t seen = p->groups_seen;
    rds_parser_init(p);
    p->groups_seen = seen;
    p->pi = pi; p->pi_valid = 1;
}

static void update_pi(rds_parser_t *p, uint16_t pi)
{
    if (p->pi_valid && pi == p->pi) { p->pi_cnt = 0; return; }
    if (!p->pi_valid) { reset_station(p, pi); return; }
    /* Wechsel: erst nach 3 gleichen Kandidaten uebernehmen */
    if (p->pi_cnt && pi == p->pi_cand) { if (++p->pi_cnt >= 3) reset_station(p, pi); }
    else { p->pi_cand = pi; p->pi_cnt = 1; }
}

static void rt_try_complete(rds_parser_t *p)
{
    unsigned seglen = p->rt_b ? 2 : 4, nseg = 64 / seglen;
    if (p->rt_b) nseg = 16;
    unsigned need;
    if (p->rt_end != 0xFF) need = (1u << (p->rt_end / seglen + 1)) - 1u;
    else need = (nseg >= 16) ? 0xFFFFu : ((1u << nseg) - 1u);
    if ((p->rt_mask & need) != need) return;
    unsigned len = (p->rt_end != 0xFF) ? p->rt_end : (p->rt_b ? 32u : 64u);
    memcpy(p->rt_shown, p->rt, 64);
    p->rt_shown_len = (uint8_t)len; p->rt_complete = true;
}

static void parse_rtplus(rds_parser_t *p, const rds_group_t *g)
{
    uint16_t B = g->blk[1], C = g->blk[2], D = g->blk[3];
    uint8_t tog = (B >> 4) & 1, run = (B >> 3) & 1;
    if (tog != p->rtp_toggle || run != p->rtp_running) {
        memset(p->tag, 0, sizeof p->tag);
        p->rtp_toggle = tog; p->rtp_running = run;
    }
    if (g->st[2]) {
        p->tag[0].type  = (uint8_t)(((B & 7) << 3) | (C >> 13));
        p->tag[0].start = (C >> 7) & 63;
        p->tag[0].len   = (uint8_t)(((C >> 1) & 63) + 1);
        p->tag[0].valid = 1;
        if (g->st[3]) {
            p->tag[1].type  = (uint8_t)(((C & 1) << 5) | (D >> 11));
            p->tag[1].start = (D >> 5) & 63;
            p->tag[1].len   = (uint8_t)((D & 31) + 1);
            p->tag[1].valid = 1;
        }
    }
}

void rds_parser_feed(rds_parser_t *p, const rds_group_t *g, uint32_t now_ms)
{
    p->groups_seen++;
    if (g->st[0]) update_pi(p, g->blk[0]);
    else if (g->st[1] && g->ver_b && g->st[2]) update_pi(p, g->blk[2]);
    if (!g->st[1]) return;

    uint16_t B = g->blk[1], C = g->blk[2], D = g->blk[3];
    unsigned type = B >> 12, ver = (B >> 11) & 1;
    p->groups_by_type[type * 2 + ver]++;
    p->tp = (B >> 10) & 1; p->pty = (B >> 5) & 31;

    switch (type) {
    case 0:
        p->ta = (B >> 4) & 1; p->ms = (B >> 3) & 1;
        if (g->st[3]) {
            unsigned seg = B & 3;
            p->ps[seg * 2] = D >> 8; p->ps[seg * 2 + 1] = D & 0xFF;
            p->ps_mask |= 1u << seg;
            if (p->ps_mask == 0xF) { memcpy(p->ps_shown, p->ps, 8); p->ps_complete = true; }
        }
        break;
    case 1:
        if (!ver && g->st[2] && ((C >> 12) & 7) == 0) { p->ecc = C & 0xFF; p->ecc_valid = 1; }
        break;
    case 2: {
        uint8_t ab = (B >> 4) & 1; unsigned seg = B & 15;
        if (!p->rt_ab_valid || ab != p->rt_ab) {
            memset(p->rt, ' ', 64); p->rt_mask = 0; p->rt_end = 0xFF;
            p->rt_ab = ab; p->rt_ab_valid = 1;
        }
        p->rt_b = ver;
        unsigned pos; uint8_t ch[4]; unsigned n = 0;
        if (!ver) {
            if (!g->st[2] || !g->st[3]) break;
            pos = seg * 4; ch[0] = C >> 8; ch[1] = C & 255; ch[2] = D >> 8; ch[3] = D & 255; n = 4;
        } else {
            if (!g->st[3]) break;
            pos = seg * 2; ch[0] = D >> 8; ch[1] = D & 255; n = 2;
        }
        for (unsigned i = 0; i < n; i++) {
            p->rt[pos + i] = ch[i];
            if (ch[i] == 0x0D && pos + i < p->rt_end) p->rt_end = (uint8_t)(pos + i);
        }
        p->rt_mask |= 1u << seg;
        rt_try_complete(p);
        break; }
    case 3:
        if (!ver && g->st[2] && g->st[3] && D == 0x4BD7) { p->rtp_gtype = B & 31; p->rtp_known = 1; }
        break;
    case 4:
        if (!ver && g->st[2] && g->st[3]) {
            p->ct_mjd = ((uint32_t)(B & 3) << 15) | (C >> 1);
            p->ct_h = (uint8_t)(((C & 1) << 4) | (D >> 12));
            p->ct_m = (D >> 6) & 63;
            int off = D & 31; p->ct_off_half = (D & 32) ? -off : off;
            p->ct_rx_ms = now_ms;
            p->ct_valid = (p->ct_h < 24 && p->ct_m < 60);
        }
        break;
    default: break;
    }
    if (p->rtp_known && type == (unsigned)(p->rtp_gtype >> 1) && ver == (unsigned)(p->rtp_gtype & 1)
        && type != 3 && type != 4 && type != 2 && type != 0)
        parse_rtplus(p, g);
}

void rds_print_group(const rds_group_t *g)
{
    char b[4][8];
    for (int i = 0; i < 4; i++) {
        if (g->st[i]) snprintf(b[i], sizeof b[i], "%04X%c", g->blk[i], g->st[i] == 2 ? '*' : ' ');
        else strcpy(b[i], "----  ");
    }
    if (g->st[1]) printf("RDS %s %s %s %s  %uA%c\n", b[0], b[1], b[2], b[3],
                         g->blk[1] >> 12, ((g->blk[1] >> 11) & 1) ? 'B' : 'A');
    else printf("RDS %s %s %s %s  ??\n", b[0], b[1], b[2], b[3]);
}

/* MJD -> Datum (Hinnant) */
static void mjd_to_date(int32_t mjd, int *y, int *m, int *d)
{
    int32_t z = mjd - 40587 + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t yy = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

static const char *country(const rds_parser_t *p)
{
    if (!p->ecc_valid || !p->pi_valid) return "";
    unsigned c = p->pi >> 12;
    if (p->ecc == 0xE0 && (c == 0xD || c == 0x1)) return " [DE]";
    if (p->ecc == 0xE0 && c == 0xA) return " [AT]";
    if (p->ecc == 0xE1 && c == 0x4) return " [CH]";
    return "";
}

void rds_print_status(const rds_parser_t *p, const rds_link_stats_t *l, uint32_t now_ms)
{
    char buf[256];
    uint32_t tot = l->blocks_ok + l->blocks_fixed + l->blocks_bad;
    unsigned pct = tot ? (100u * (l->blocks_ok + l->blocks_fixed)) / tot : 0;
    printf("\n=== RDS-Status  t=%lus ==============================\n", (unsigned long)(now_ms / 1000));
    printf("Empfang : %s  Traeger %+.1f Hz  Lock %.2f  Pegel %u..%u (DC %.0f)  Symbol-Takt %+.0f ppm\n",
           l->synced ? "SYNC" : "suche", (double)l->dsp.carrier_offset_hz, (double)l->dsp.lock,
           l->dsp.adc_min, l->dsp.adc_max, (double)l->dsp.dc_counts, (double)l->dsp.timing_ppm);
    printf("          Bloecke ok %lu (repariert %lu, fehlerhaft %lu = %u%% gut)  Gruppen %lu  Sync-Verluste %lu\n",
           (unsigned long)l->blocks_ok, (unsigned long)l->blocks_fixed, (unsigned long)l->blocks_bad,
           pct, (unsigned long)l->groups, (unsigned long)l->sync_losses);
    if (!p->pi_valid) { printf("Sender  : (noch kein PI empfangen)\n"); return; }

    char ps[40];
    uint8_t tmp[8]; memcpy(tmp, p->ps_complete ? p->ps_shown : p->ps, 8);
    if (!p->ps_complete) for (int i = 0; i < 8; i++) if (!(p->ps_mask & (1u << (i / 2)))) tmp[i] = '_';
    to_utf8(tmp, 0, 8, ps, sizeof ps);
    printf("Sender  : PI %04X%s  PS \"%-8s\"%s  PTY %u %s  TP%c TA%c %s\n", p->pi, country(p), ps,
           p->ps_complete ? "" : " (unvollst.)", p->pty, pty_names[p->pty & 31],
           p->tp ? '+' : '-', p->ta ? '+' : '-', p->ms ? "Musik" : "Sprache");

    if (p->ct_valid) {
        int y, m, d;
        int32_t lm = (int32_t)(p->ct_h * 60 + p->ct_m) + p->ct_off_half * 30
                   + (int32_t)((now_ms - p->ct_rx_ms) / 60000u);
        int32_t dd = lm >= 0 ? lm / 1440 : -((-lm + 1439) / 1440);
        int32_t lmins = lm - dd * 1440;
        mjd_to_date((int32_t)p->ct_mjd + dd, &y, &m, &d);
        int off = p->ct_off_half * 30;
        printf("Uhrzeit : %04d-%02d-%02d %02d:%02d lokal (UTC%+03d:%02d, CT-Gruppe vor %lus)\n", y, m, d,
               (int)(lmins / 60), (int)(lmins % 60), off / 60, (off < 0 ? -off : off) % 60,
               (unsigned long)((now_ms - p->ct_rx_ms) / 1000));
    } else printf("Uhrzeit : (noch keine CT-Gruppe 4A empfangen)\n");

    if (p->rt_complete) {
        to_utf8(p->rt_shown, 0, p->rt_shown_len, buf, sizeof buf);
        printf("Text    : \"%s\"\n", buf);
        const char *lab[2] = {0, 0}; char val[2][100];
        for (int t = 0; t < 2; t++) {
            if (!p->tag[t].valid || p->tag[t].start + p->tag[t].len > p->rt_shown_len) continue;
            uint8_t ty = p->tag[t].type;
            if (ty == 1) lab[t] = "Titel   ";
            else if (ty == 4) lab[t] = "Interpret";
            else if (ty == 2) lab[t] = "Album   ";
            else continue;
            to_utf8(p->rt_shown, p->tag[t].start, p->tag[t].len, val[t], sizeof val[t]);
        }
        for (int t = 0; t < 2; t++) if (lab[t]) printf("%s: %s\n", lab[t], val[t]);
    } else printf("Text    : (RadioText noch unvollstaendig, %u/16 Segmente)\n",
                  (unsigned)__builtin_popcount(p->rt_mask));
}
