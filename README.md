# RDS Decoder for the RP2040 (Raspberry Pi Pico)

A complete software RDS decoder: it takes the **MPX signal of an FM tuner** on an ADC pin, demodulates the 57 kHz RDS subcarrier entirely in software (Costas loop, symbol clock recovery, block synchronization), and outputs both the raw RDS blocks and, every few seconds, a human-readable summary over the USB serial console: **station name, station identification, programme type, time of day, RadioText, and title and artist** (if the station transmits RadioText Plus).

No additional RDS hardware (e.g. the classic SAA6588 or similar) is needed – only the Pico, a bandpass filter around 57 kHz, and two resistors.

```
=== RDS-Status  t=25s ==============================
Empfang : SYNC  Traeger -15.1 Hz  Lock 0.96  Pegel 1956..2261 (DC 2105)  Symbol-Takt -6830 ppm
          Bloecke ok 1116 (repariert 0, fehlerhaft 0 = 100% gut)  Gruppen 279  Sync-Verluste 0
Sender  : PI 7012  PS "K-98.3 "  PTY 7 Kultur  TP- TA- Musik
Uhrzeit : 2026-10-19 17:02 lokal (UTC+02:00, CT-Gruppe vor 12s)
Text    : "Music That Makes You Feel Good! / Long Island's K-98.3"
```

*(The firmware's console output is in German, as shown above. The labels are explained in [Operation and output](#operation-and-output).)*

---

## Contents

1. [What is RDS?](#what-is-rds)
2. [Signal chain of this decoder](#signal-chain-of-this-decoder)
3. [Hardware](#hardware)
4. [Building and flashing](#building-and-flashing)
5. [Operation and output](#operation-and-output)
6. [Testing on a PC without hardware](#testing-on-a-pc-without-hardware)
7. [Project structure](#project-structure)
8. [Limitations and possible extensions](#limitations-and-possible-extensions)

---

## What is RDS?

The **Radio Data System** (RDS, known as RBDS in the USA; standard IEC 62106 / EN 50067) transmits small amounts of data in FM broadcasting in addition to the audio. Practically every car radio uses it for the station name, traffic announcements, or automatic station search.

### Where is RDS located in the FM signal?

The demodulated FM signal (the *MPX signal*, "multiplex") contains several components stacked on top of each other:

| Frequency range | Content |
|---|---|
| 0 – 15 kHz | Mono sum signal (L+R) |
| 19 kHz | Stereo pilot tone |
| 23 – 53 kHz | Stereo difference signal (L−R), around 38 kHz |
| **57 kHz ± 2.4 kHz** | **RDS** (3rd harmonic of the pilot tone) |

The RDS subcarrier is at exactly 57 kHz and is phase-locked to the pilot tone. It carries a **BPSK signal** (phase-shift keying) with only about 2–10 % of the total deviation – a very weak, narrowband signal next to strong audio.

### How is the data encoded?

* **Data rate:** 1187.5 bit/s (= 57 kHz / 48).
* **Differential encoding:** A bit corresponds to a *change* or *no change* of the sign – so the absolute polarity of the carrier phase does not matter (important for the Costas loop, which has a 180° ambiguity).
* **Biphase (Manchester) symbols:** Each bit consists of a positive and a negative half-wave. As a result, the spectrum has *no* energy directly at the carrier (no residual carrier); the main lobe lies at ±1.2 kHz around 57 kHz.
* **Blocks and groups:** 16 data bits + 10 check bits = one **block** (26 bits). Four blocks (A, B, C or C', D) form a **group** (104 bits, about 11.4 groups/s).
* **Checksum (CRC):** The 10 check bits are combined with an **offset word** (A, B, C, C', D). This lets the receiver determine its position in the bit stream *without* any additional synchronization bits – and even correct short error bursts.

### What is in the groups?

| Block | Content |
|---|---|
| A | **PI** (Programme Identification, 16 bits) – unique station identifier, containing the country among other things |
| B | Group type (4 bits), version A/B, TP flag, **PTY** (programme type, 5 bits), group-specific bits |
| C, D | Payload depending on group type |

The most important group types (evaluated by this decoder):

| Group | Content |
|---|---|
| **0A/0B** | **PS** – Programme Service name (8 characters, in 4 segments of 2 characters), TA/TP, music/speech |
| **1A** | Slow labelling, including the **ECC** (Extended Country Code) |
| **2A/2B** | **RadioText** (up to 64 characters, in segments of 4 or 2 characters; the A/B flag signals new text) |
| **3A** | ODA announcement ("Open Data Application") – here **RadioText Plus (RT+)**, AID `4BD7` |
| **4A** | **CT** – Clock Time: date (Modified Julian Date), UTC time, local offset; once per minute |
| *RT+ group* | Markers ("tags") in the RadioText: which characters are **title**, **artist**, album, etc. |

---

## Signal chain of this decoder

The sampling rate is deliberately **four times the RDS subcarrier (228 kS/s)**. This makes the mixer at 57 kHz mathematically trivial: the carrier is just the sequence `1, 0, −1, 0` (cosine) or `0, −1, 0, 1` (sine) – no multiplications and no lookup tables are needed.

```
ADC0 (GPIO26) ──DMA──► [DC tracking] ─► [Mixer 57 kHz, ÷4] ─► [FIR low-pass, ÷6]
   228 kS/s                228 k              57 k                    9.5 kHz
                                                                         │
 Bits ◄─ [Diff. decoder] ◄─ [Clock control + Manchester correlator] ◄─ [Costas loop]
   │            1187.5 bit/s            8 samples per symbol               (BPSK, PI controller)
   ▼
[Block sync / CRC / burst correction] ─► Groups ─► [Parser] ─► serial output
```

| Stage | File | Description |
|---|---|---|
| **ADC + DMA** | `main.c` | Free-running ADC at 48 MHz/210.5 ≈ 228 kS/s, two DMA channels in ping-pong mode (1536 samples each = 6.7 ms), one interrupt per block |
| **DC tracking** | `rds_dsp.c` | A single-pole low-pass estimates the DC component (component tolerances of the voltage divider) and subtracts it **continuously, sample by sample**. The time constant is short at first (fast settling), then about 18 ms |
| **Mixer + ÷4** | `rds_dsp.c` | With the sequence `1,0,−1,0`, each four samples yield one complex value: `I = x0 − x2`, `Q = x3 − x1` (baseband rate 57 kHz). The differencing suppresses DC and 114 kHz exactly, **but audio below about 4 kHz aliases back into the RDS band with only −23 … −37 dB** – a good analog bandpass is therefore mandatory |
| **FIR low-pass ÷6** | `rds_dsp.c`, `rds_fir.h` | 48 taps (Q15, generated with `tools/gen_fir.py`), passband up to 2.2 kHz, stopband from 5.6 kHz (≥ 44 dB). Result: 9.5 kHz = **8 samples per symbol**. Pilot (19 kHz) and stereo residuals at about 8 kHz offset and beyond are suppressed by ≥ 60 dB; directly at the carrier the slope is flat (52/62 kHz: −23 dB, 53 kHz: −7 dB) |
| **Costas loop** | `rds_dsp.c` | A true BPSK Costas loop with NCO (1024-entry sine table). Phase error `I·Q / power` (hence level-independent, no separate AGC needed), 2nd-order PI controller. Noise bandwidth 35 Hz during acquisition, automatically 8 Hz at lock (lock detector). The loop compensates crystal deviations and the difference to the transmitter's carrier |
| **Symbol clock** | `rds_dsp.c` | Sliding Manchester correlator (half-sine weights over one symbol); early/late detector with linear interpolation and PI loop (clock deviation up to about ±500 ppm is fine; at ±2000 ppm reception breaks down). **Coarse search** via an energy profile, because the correlator has a side maximum half a symbol period away from the main maximum, on which the fine loop could otherwise lock stably in the wrong place |
| **Diff. decoder** | `rds_dsp.c` | Bit = sign change relative to the previous symbol |
| **Block sync** | `rds_sync.c` | 26-bit shift register, CRC syndrome against all offsets; sync after two valid blocks at the correct spacing/order; block-by-block checking thereafter. **Burst correction** of up to 2 bits (configurable via `RDS_FEC_MAX_BURST`); sync loss after 10 consecutive erroneous blocks or > 35 out of 50 |
| **Parser** | `rds_parser.c` | PS, PTY, TP/TA, ECC, RadioText (A/B flag, terminator `0x0D`, completeness check), RT+ tags, CT including date calculation, **RDS character set → UTF-8** (umlauts, ß, …). A PI change is accepted only after 3 confirmations |

**Two cores:** Core 1 runs the ADC processing, DSP, and block sync (time-critical). Core 0 evaluates the completed groups (via a queue) and handles USB output and keyboard commands. This way, slow USB output can never block the demodulation.

---

## Hardware

```
MPX from tuner ─► Bandpass 57 kHz ─► C (coupling capacitor) ─┬─► ADC0 (GPIO26)
                                                             │
                                            3V3 ──[R]──┬─────┘
                                                       └──[R]── GND      (R = 2 × same value, e.g. 4.7 k … 10 k)
```

* **DC operating point:** Two equal resistors set the ADC input to mid-range (≈ 2048). Component tolerances are not critical – the firmware continuously removes the residual DC.
* **Level:** After the bandpass, the RDS signal should have at least a few tens of ADC counts of amplitude (check "Pegel min..max" in the status line); amplify before the ADC if necessary. Do not overdrive (0 or 4095).
* **Bandpass:** A bandpass around 57 kHz (about ±3 kHz) is important: it must clearly attenuate mono audio (< 15 kHz), pilot tone (19 kHz), and the stereo difference signal (up to 53 kHz), because the digital input stage of this decoder attenuates audio below 4 kHz only weakly.
* **Noise:** On the Pico, GPIO23 (SMPS power save) is set HIGH (PWM mode of the switching regulator → cleaner ADC supply). Short wiring and a well-decoupled 3V3 rail also help.
* **Sampling rate:** 48 MHz / 210.527 = 227.999 kS/s (−6 ppm, ≈ 0.3 Hz offset at 57 kHz; compensated by the Costas loop). The loop acquires carrier offsets up to ±20 Hz in < 0.3 s, ±50 Hz in about 1.5 s, and ±100 Hz only after about 6 s.

---

## Building and flashing

Requirements: [Pico SDK](https://github.com/raspberrypi/pico-sdk) (including the `tinyusb` submodule), CMake, `arm-none-eabi-gcc`.

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -DPICO_BOARD=pico
make -C build
```

Copy `build/rds_decoder.uf2` to the Pico's USB drive while holding the BOOTSEL button. Afterwards a USB serial port (CDC; the baud rate does not matter) appears – open it with any terminal program (UTF-8!).

---

## Operation and output

Every 5 seconds (`RDS_STATUS_INTERVAL_MS` in `src/rds_config.h`) a status block is printed. The firmware prints the line labels in German; their meaning:

| Line (as printed) | Meaning |
|---|---|
| **Empfang** (reception) | `SYNC`/`suche` (searching), carrier offset of the Costas loop in Hz, lock value (≈ 1 = locked), ADC level min..max and DC value, symbol clock deviation |
| Blöcke (blocks) | Number of good / repaired / erroneous blocks, groups, sync losses |
| **Sender** (station) | PI (hex, with country if known), PS (station name), PTY, TP/TA, music/speech |
| **Uhrzeit** (time) | Local time from the CT group, counted on between the once-per-minute groups |
| **Text / Titel / Interpret** (text / title / artist) | RadioText; with RT+, title, artist, and album are additionally shown separately |

Keyboard commands in the terminal:

| Key | Function |
|---|---|
| `v` | Raw groups on/off (`RDS 7012  00E8  E0CD  596F   0A`; `----` = block erroneous, `*` = repaired by burst correction) |
| `s` | Print status immediately |
| `r` | Reset RDS data (e.g. after changing station) |
| `h` | Help |

The Pico's LED is lit as long as block sync is established.

---

## Testing on a PC without hardware

The entire DSP/decoder code is hardware-independent and also runs on a PC. The test signal is an MP3 containing the RDS baseband (here about 2.2 kHz carrier at 44.1 kHz); `make_testsignal.py` converts it to 57 kHz / 228 kS/s and produces an ADC-like 12-bit signal with DC offset and noise.

```bash
python3 tools/make_testsignal.py your.mp3 test/mpx228k.u16
gcc -O2 -Wall -o test/host_test test/host_test.c src/rds_dsp.c src/rds_sync.c src/rds_parser.c -lm
test/host_test test/mpx228k.u16 -v
```

`tools/make_mpx_test.py` additionally generates a more realistic test signal with audio, pilot, and stereo interferers, an analog front-end model (2nd/4th/8th-order bandpass or high-pass only), and ADC noise/nonlinearity (parameters: see the top of the file).

Options of the generator `make_testsignal.py`: `--amp` (RDS amplitude in counts), `--noise`, `--dc`, `--pilot` (19 kHz residual), `--foff` (carrier offset in Hz), `--ppm` (clock error), `--fc` (carrier frequency in the MP3).

**Results with the supplied test signal** (30 s, US station "K-98.3", PI 7012):

| Scenario | Good blocks |
|---|---|
| Ideal (amplitude 150, noise 6 counts) | 100 % |
| Carrier +25 Hz, +150 ppm, pilot residual 80 counts, noise 20 counts | 99 % |
| Carrier −30 Hz, −200 ppm, noise 40 counts at amplitude 80 | 99 % |
| Noise 80 counts at amplitude 100 (SNR < 0 dB in the ADC band) | 99 % |


---

## Project structure

```
CMakeLists.txt            Pico SDK project (code runs from RAM)
pico_sdk_import.cmake
src/
  main.c                  ADC + DMA (ping-pong), core 1 loop, core 0 console
  rds_config.h            central constants (rates, loop bandwidths, FEC, status interval)
  rds_dsp.c/.h            DC tracking, mixer, FIR, Costas, symbol clock, diff. decoder
  rds_fir.h               FIR coefficients (generated by tools/gen_fir.py)
  rds_sync.c/.h           block sync, CRC syndromes, burst correction, group assembly
  rds_parser.c/.h         group evaluation, character set, status output
tools/
  gen_fir.py              FIR design (scipy) → rds_fir.h
  make_testsignal.py      MP3/baseband → 57 kHz ADC test signal
test/
  host_test.c             decoder on the PC (reads raw uint16 samples)
```

---

## Limitations and possible extensions

* **Not evaluated:** AF lists (alternative frequencies), TMC/traffic information, EON, RDS2. The raw groups (`v`) provide the data for these.
* **RT+** implemented and checked against the specification, but not validated with real broadcasts due to a lack of suitable test data – please test with your own reception data.
* **Sync after bit slip:** If the clock slips back by one bit, sync is discarded after 10 erroneous blocks (≈ 0.2 s) and searched anew.
* **Scrolling PS:** Some stations (e.g. the test signal) send the station name as a scrolling text; the PS field then shows changing fragments, and the RadioText is usually more informative.
* **Country code:** Only DE/AT/CH are displayed as abbreviations, derived from ECC + the PI country nibble.
* **Ideas:** use the 19 kHz pilot as carrier/clock reference (better sensitivity), soft-decision decoding, AF/EON display, JSON output, connection to a display.
