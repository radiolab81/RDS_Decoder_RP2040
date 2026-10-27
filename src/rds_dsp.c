#include "rds_dsp.h"
#include <math.h>
#include <string.h>
#include <stdbool.h>

#define NCO_N 1024
#define TWO_PI_F 6.2831853f

static float sin_tbl[NCO_N];
static bool  tbl_ready;

static void loop_gains(float bl_hz, float *kp, float *ki)
{
    const float zeta = 0.707f;
    float th = (bl_hz / (float)RDS_BB_FS_HZ) / (zeta + 0.25f / zeta);
    float den = 1.0f + 2.0f * zeta * th + th * th;
    *kp = 4.0f * zeta * th / den / TWO_PI_F;      /* in Zyklen (Fehler in rad) */
    *ki = 4.0f * th * th / den / TWO_PI_F;
}

void rds_dsp_init(rds_dsp_t *d, rds_bit_cb cb, void *ctx)
{
    memset(d, 0, sizeof *d);
    if (!tbl_ready) {
        for (int i = 0; i < NCO_N; i++) sin_tbl[i] = sinf(TWO_PI_F * (float)i / (float)NCO_N);
        tbl_ready = true;
    }
    d->cb = cb; d->ctx = ctx;
    d->dc_shift = 8;
    d->adc_min = 0xFFFF; d->adc_max = 0;
    loop_gains(RDS_COSTAS_BL_ACQ, &d->kp_acq, &d->ki_acq);
    loop_gains(RDS_COSTAS_BL_TRK, &d->kp_trk, &d->ki_trk);
    d->tper = (float)RDS_SPS;
    d->asym = 1.0f;
}

/* Matched-Filter-Gewichte fuer ein Biphase-Symbol (aelteste zuerst), ~Halbsinus */
static const float mf_w[8] = { 0.38f, 0.92f, 0.92f, 0.38f, -0.38f, -0.92f, -0.92f, -0.38f };

static void bb_step(rds_dsp_t *d, float i, float q)
{
    /* --- Costas-Schleife (BPSK) --- */
    unsigned idx = (unsigned)(d->nco_ph * (float)NCO_N) & (NCO_N - 1);
    float sn = sin_tbl[idx];
    float cs = sin_tbl[(idx + NCO_N / 4) & (NCO_N - 1)];
    float zi = i * cs + q * sn;
    float zq = q * cs - i * sn;

    float pw = zi * zi + zq * zq;
    d->pow += (pw - d->pow) * (1.0f / 512.0f);
    float den = d->pow + 0.25f;
    float err = (zi * zq) / den;                   /* ~ Phasenfehler in rad */
    if (err > 0.7f) err = 0.7f; else if (err < -0.7f) err = -0.7f;

    bool tracking = d->lock > 0.5f;
    float kp = tracking ? d->kp_trk : d->kp_acq;
    float ki = tracking ? d->ki_trk : d->ki_acq;
    d->nco_f += ki * err;
    const float fmax = RDS_COSTAS_MAX_HZ / (float)RDS_BB_FS_HZ;
    if (d->nco_f > fmax) d->nco_f = fmax; else if (d->nco_f < -fmax) d->nco_f = -fmax;
    d->nco_ph += d->nco_f + kp * err;
    if (d->nco_ph >= 1.0f) d->nco_ph -= 1.0f; else if (d->nco_ph < 0.0f) d->nco_ph += 1.0f;
    if (d->nco_ph >= 1.0f || d->nco_ph < 0.0f) d->nco_ph = 0.0f;
    d->lock += (((zi * zi - zq * zq) / den) - d->lock) * (1.0f / 256.0f);

    /* --- Manchester-Korrelator ueber 1 Symbol --- */
    d->zh[d->zhp] = zi;
    d->zhp = (d->zhp + 1) & 7;                     /* aeltester Wert liegt bei zhp */
    float c = 0.0f;
    for (int k = 0; k < 8; k++) c += mf_w[k] * d->zh[(d->zhp + k) & 7];
    d->c3 = d->c2; d->c2 = d->c1; d->c1 = d->c0; d->c0 = c;

    /* Energieprofil |c| ueber dem Abstand zum letzten Strobe (Bin 0..7) */
    d->phc++;
    {
        float *b = &d->bins[d->phc & 7];
        *b += (fabsf(c) - *b) * (1.0f / 32.0f);
    }

    /* --- Symboltakt: Early/Late mit linearer Interpolation --- */
    d->tph += 1.0f;
    if (d->tph >= d->tper) {
        float mu = d->tph - d->tper;
        if (mu > 1.0f) mu = 1.0f;
        float f = 1.0f - mu;
        float on    = d->c2 + f * (d->c1 - d->c2);
        float early = d->c3 + f * (d->c2 - d->c3);
        float late  = d->c1 + f * (d->c0 - d->c1);
        float aon = fabsf(on);
        d->asym += (aon - d->asym) * (1.0f / 32.0f);
        float te = (fabsf(late) - fabsf(early)) / (d->asym + 0.05f);
        if (te > 1.0f) te = 1.0f; else if (te < -1.0f) te = -1.0f;
        d->tfreq += 0.0004f * te;
        if (d->tfreq > 0.2f) d->tfreq = 0.2f; else if (d->tfreq < -0.2f) d->tfreq = -0.2f;
        d->tper = (float)RDS_SPS + d->tfreq;
        d->tph = mu - 0.04f * te;
        d->phc = 0;

        /* Grobsuche: Der Manchester-Korrelator hat neben dem Symbolmaximum ein
         * Nebenmaximum eine halbe Symbolperiode daneben (~0,6-fache Hoehe). Die
         * Early/Late-Schleife kann dort stabil falsch rasten. Alle 64 Symbole wird
         * deshalb das Energieprofil geprueft; liegt das Maximum >= 2 Samples neben
         * dem Entscheidungspunkt (bei Bin 6..7), wird der Strobe dorthin verschoben. */
        if (++d->nchk >= 64) {
            d->nchk = 0;
            unsigned best = 0;
            for (unsigned k = 1; k < 8; k++) if (d->bins[k] > d->bins[best]) best = k;
            float here = d->bins[6] > d->bins[7] ? d->bins[6] : d->bins[7];
            float dd = (float)best - 6.5f;
            if (dd >= 4.0f) dd -= 8.0f; else if (dd < -4.0f) dd += 8.0f;
            if ((dd >= 2.0f || dd <= -2.0f) && d->bins[best] > 1.25f * here) {
                d->tph -= dd;
                memset(d->bins, 0, sizeof d->bins);
                d->nchk = 0;
            }
        }

        uint8_t pos = on > 0.0f;
        uint8_t bit = pos ^ d->prev_pos;           /* differentieller Dekoder */
        d->prev_pos = pos;
        d->bits++;
        if (d->cb) d->cb(d->ctx, bit);
    }
}

static inline void fir_push(rds_dsp_t *d, int32_t i4, int32_t q4)
{
    unsigned p = d->fpos;
    d->hi[p] = d->hi[p + RDS_FIR_TAPS] = i4;
    d->hq[p] = d->hq[p + RDS_FIR_TAPS] = q4;
    if (++p == RDS_FIR_TAPS) p = 0;
    d->fpos = (uint8_t)p;
    if (++d->fcnt < RDS_DEC_FIR) return;
    d->fcnt = 0;
    const int32_t *pi = &d->hi[p], *pq = &d->hq[p];
    int32_t ai = 0, aq = 0;
    for (int k = 0; k < RDS_FIR_TAPS; k++) {
        int32_t h = rds_fir_q15[k];
        ai += h * pi[k];
        aq += h * pq[k];
    }
    bb_step(d, (float)ai * (1.0f / 32768.0f), (float)aq * (1.0f / 32768.0f));
}

void rds_dsp_process(rds_dsp_t *d, const uint16_t *s, size_t n)
{
    int32_t dc = d->dc_acc;
    unsigned sh = d->dc_shift;
    uint16_t mn = d->adc_min, mx = d->adc_max;
    if (!d->dc_init && n) { dc = (int32_t)s[0] << 16; d->dc_init = 1; }

    for (size_t k = 0; k + 4 <= n; k += 4) {
        int32_t x[4];
        for (int j = 0; j < 4; j++) {
            uint16_t v = s[k + j];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            /* mitlaufende DC-Schaetzung (Einpol-Tiefpass), Startphase schneller */
            dc += (((int32_t)v << 16) - dc) >> sh;
            x[j] = (int32_t)v - ((dc + 32768) >> 16);
        }
        if (sh < 12 && ++d->dc_count >= (1u << (sh - 4))) { d->dc_count = 0; sh++; }
        /* Mischer mit cos = (1,0,-1,0), -sin = (0,-1,0,1), dezimiert um 4 */
        fir_push(d, x[0] - x[2], x[3] - x[1]);
    }
    d->dc_acc = dc; d->dc_shift = (uint8_t)sh; d->adc_min = mn; d->adc_max = mx;
}

void rds_dsp_get_stats(rds_dsp_t *d, rds_dsp_stats_t *st, int reset_minmax)
{
    st->carrier_offset_hz = d->nco_f * (float)RDS_BB_FS_HZ;
    st->lock = d->lock;
    st->sym_amp = d->asym;
    st->dc_counts = (float)d->dc_acc / 65536.0f;
    st->timing_ppm = d->tfreq / (float)RDS_SPS * 1e6f;   /* ADC-Takt relativ zum Sender (grober Schaetzwert) */
    st->adc_min = d->adc_min; st->adc_max = d->adc_max;
    st->bits = d->bits;
    if (reset_minmax) { d->adc_min = 0xFFFF; d->adc_max = 0; }
}
