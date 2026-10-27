#pragma once
/* RDS-Demodulator: ADC-Samples (228 kS/s, 12 Bit) -> differentiell dekodierte Bits.
 * Kette: DC-Tracking -> Mischer 57 kHz (1,0,-1,0) ->  /4 -> FIR /6 -> Costas (BPSK)
 *        -> Manchester-Korrelator -> Zeittaktregelung (Early/Late) -> Diff.-Dekoder */
#include <stdint.h>
#include <stddef.h>
#include "rds_config.h"
#include "rds_fir.h"

typedef void (*rds_bit_cb)(void *ctx, uint8_t bit);

typedef struct {
    /* Stufe 1: DC */
    int32_t  dc_acc;            /* Q16 */
    uint8_t  dc_init, dc_shift;
    uint32_t dc_count;
    uint16_t adc_min, adc_max;  /* laufende Min/Max (Intervall) */
    /* Stufe 2: FIR */
    int32_t  hi[2 * RDS_FIR_TAPS], hq[2 * RDS_FIR_TAPS];
    uint8_t  fpos, fcnt;
    /* Stufe 3: Costas */
    float    nco_ph, nco_f;     /* Zyklen bzw. Zyklen/Sample */
    float    pow, lock;
    float    kp_acq, ki_acq, kp_trk, ki_trk;
    /* Stufe 4: Symboltakt */
    float    zh[8]; uint8_t zhp;
    float    c0, c1, c2, c3;
    float    tph, tper, tfreq, asym;
    uint8_t  prev_pos;
    /* Grobsynchronisation: Energie |c| je Abstand zum letzten Strobe (gegen Falschrasten) */
    float    bins[8];
    uint8_t  phc, nchk;
    /* Ausgabe */
    rds_bit_cb cb; void *ctx;
    uint32_t bits;
} rds_dsp_t;

typedef struct {
    float carrier_offset_hz;  /* Costas-Frequenz relativ zu 57 kHz */
    float lock;               /* ~1 = gerastet, ~0 = nicht */
    float sym_amp;            /* mittlere Symbolamplitude */
    float dc_counts;          /* geschaetzter DC-Anteil (ADC-Counts) */
    float timing_ppm;         /* Symboltakt-Abweichung */
    uint16_t adc_min, adc_max;
    uint32_t bits;
} rds_dsp_stats_t;

void rds_dsp_init(rds_dsp_t *d, rds_bit_cb cb, void *ctx);
/* n muss ein Vielfaches von 4 sein (Blockgroesse: Vielfaches von 24 empfohlen) */
void rds_dsp_process(rds_dsp_t *d, const uint16_t *s, size_t n);
void rds_dsp_get_stats(rds_dsp_t *d, rds_dsp_stats_t *st, int reset_minmax);
