/* RDS-Decoder fuer RP2040 (Raspberry Pi Pico)
 *  - ADC0 (GPIO26) tastet das um 57 kHz bandpassgefilterte MPX-Signal mit 228 kS/s ab (DMA, Ping-Pong)
 *  - Core 1: komplette DSP-Kette + Block-Sync (zeitkritisch)
 *  - Core 0: RDS-Gruppen auswerten, serielle Ausgabe (USB-CDC), Kommandos
 *  Serielle Kommandos: v = Rohgruppen an/aus, s = Status sofort, r = Daten zuruecksetzen, h = Hilfe */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "rds_dsp.h"
#include "rds_sync.h"
#include "rds_parser.h"

#define ADC_GPIO 26                /* ADC0 */
#define SMPS_PS_PIN 23             /* Pico: HIGH = PWM-Modus des Schaltreglers, weniger ADC-Rauschen */

static uint16_t adc_buf[2][RDS_BLOCK_SAMPLES] __attribute__((aligned(4)));
static volatile uint32_t blocks_done;
static volatile uint32_t overruns;
static int dma_ch[2];

static rds_dsp_t dsp;
static rds_sync_t sync_;
static rds_parser_t par;
static queue_t grp_q;

static void on_bit(void *c, uint8_t b) { (void)c; rds_sync_bit(&sync_, b); }
static void on_group(void *c, const rds_group_t *g) { (void)c; queue_try_add(&grp_q, g); }

static void dma_isr(void)
{
    for (int i = 0; i < 2; i++) {
        if (dma_channel_get_irq0_status(dma_ch[i])) {
            dma_channel_acknowledge_irq0(dma_ch[i]);
            dma_channel_set_write_addr(dma_ch[i], adc_buf[i], false);  /* fuer naechsten Durchlauf */
            blocks_done++;
        }
    }
}

static void adc_dma_start(void)
{
    adc_init();
    adc_gpio_init(ADC_GPIO);
    adc_select_input(0);
    adc_fifo_setup(true, true, 1, false, false);          /* 12 Bit, DREQ bei 1 Wert */
    /* Abtastrate = 48 MHz / (1 + div) */
    adc_set_clkdiv(48000000.0f / (float)RDS_ADC_FS_HZ - 1.0f);

    dma_ch[0] = dma_claim_unused_channel(true);
    dma_ch[1] = dma_claim_unused_channel(true);
    for (int i = 0; i < 2; i++) {
        dma_channel_config c = dma_channel_get_default_config(dma_ch[i]);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, DREQ_ADC);
        channel_config_set_chain_to(&c, dma_ch[i ^ 1]);
        dma_channel_configure(dma_ch[i], &c, adc_buf[i], &adc_hw->fifo, RDS_BLOCK_SAMPLES, false);
        dma_channel_set_irq0_enabled(dma_ch[i], true);
    }
    irq_set_exclusive_handler(DMA_IRQ_0, dma_isr);
    irq_set_enabled(DMA_IRQ_0, true);
    dma_channel_start(dma_ch[0]);
    adc_run(true);
}

static void core1_main(void)
{
    adc_dma_start();
    uint32_t done = 0;
    for (;;) {
        while (done == blocks_done) tight_loop_contents();
        if (blocks_done - done >= 2) {                    /* zu langsam: aeltesten Block verlieren */
            overruns++;
            done = blocks_done - 1;
        }
        rds_dsp_process(&dsp, adc_buf[done & 1], RDS_BLOCK_SAMPLES);
        done++;
    }
}

static void print_status(void)
{
    rds_link_stats_t l;
    memset(&l, 0, sizeof l);
    l.synced = sync_.synced; l.blocks_ok = sync_.blocks_ok; l.blocks_fixed = sync_.blocks_fixed;
    l.blocks_bad = sync_.blocks_bad; l.groups = sync_.groups; l.sync_losses = sync_.sync_losses;
    rds_dsp_get_stats(&dsp, &l.dsp, 1);
    rds_print_status(&par, &l, to_ms_since_boot(get_absolute_time()));
    if (overruns) printf("WARNUNG : %lu DSP-Ueberlaeufe (CPU zu langsam)\n", (unsigned long)overruns);
}

int main(void)
{
    stdio_init_all();
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN); gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_init(SMPS_PS_PIN); gpio_set_dir(SMPS_PS_PIN, GPIO_OUT); gpio_put(SMPS_PS_PIN, 1);
#endif
    queue_init(&grp_q, sizeof(rds_group_t), 32);
    rds_parser_init(&par);
    rds_sync_init(&sync_, on_group, NULL);
    rds_dsp_init(&dsp, on_bit, NULL);

    sleep_ms(2000);
    printf("\nRDS-Decoder RP2040 - ADC0/GPIO26, %u kS/s, Kommandos: v=Rohgruppen s=Status r=Reset h=Hilfe\n",
           RDS_ADC_FS_HZ / 1000u);
    multicore_launch_core1(core1_main);

    bool verbose = false;
    uint32_t next = to_ms_since_boot(get_absolute_time()) + RDS_STATUS_INTERVAL_MS;
    for (;;) {
        rds_group_t g;
        while (queue_try_remove(&grp_q, &g)) {
            if (verbose) rds_print_group(&g);
            rds_parser_feed(&par, &g, to_ms_since_boot(get_absolute_time()));
        }
        int ch = getchar_timeout_us(0);
        if (ch == 'v') { verbose = !verbose; printf("Rohgruppen %s\n", verbose ? "an" : "aus"); }
        else if (ch == 's') print_status();
        else if (ch == 'r') { rds_parser_init(&par); printf("RDS-Daten zurueckgesetzt\n"); }
        else if (ch == 'h') printf("v=Rohgruppen an/aus  s=Status  r=Reset\n");
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((int32_t)(now - next) >= 0) { next += RDS_STATUS_INTERVAL_MS; print_status(); }
#ifdef PICO_DEFAULT_LED_PIN
        gpio_put(PICO_DEFAULT_LED_PIN, sync_.synced);
#endif
        sleep_ms(2);
    }
}
