#pragma once
/* Zentrale Einstellungen des RDS-Decoders */

#define RDS_ADC_FS_HZ        228000u   /* 4 x 57 kHz */
#define RDS_DEC_MIX          4         /* 228 k -> 57 k  (Mischer mit 1,0,-1,0)  */
#define RDS_DEC_FIR          6         /* 57 k  -> 9,5 k (FIR-Tiefpass)          */
#define RDS_BB_FS_HZ         9500u     /* Basisband-Rate = 8 x 1187,5 Baud       */
#define RDS_SPS              8         /* Abtastwerte pro Symbol                  */

#define RDS_BLOCK_SAMPLES    1536u     /* DMA-Blockgroesse (Vielfaches von 24)    */

/* Costas-Schleife: Rauschbandbreiten in Hz */
#define RDS_COSTAS_BL_ACQ    35.0f     /* Einfangen */
#define RDS_COSTAS_BL_TRK    8.0f      /* Nachfuehren */
#define RDS_COSTAS_MAX_HZ    120.0f    /* max. Frequenzablage der Schleife */

/* Fehlerkorrektur im Blockdecoder: max. Buerstlaenge (0 = aus, 1..5) */
#define RDS_FEC_MAX_BURST    2

/* Statusausgabe */
#define RDS_STATUS_INTERVAL_MS 5000u
