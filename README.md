# RDS-Decoder für den RP2040 (Raspberry Pi Pico)

Ein vollständiger Software-RDS-Decoder: Er nimmt das **MPX-Signal eines UKW-Tuners** an einem ADC-Pin entgegen, demoduliert den 57-kHz-RDS-Träger komplett in Software (Costas-Schleife, Symbol-Takt-Rückgewinnung, Block-Synchronisation) und gibt über die serielle USB-Konsole sowohl die rohen RDS-Blöcke als auch alle paar Sekunden eine lesbare Zusammenfassung aus: **Sendername, Stationskennung, Programmtyp, Uhrzeit, RadioText sowie Titel und Interpret** (sofern der Sender RadioText Plus sendet).

Es wird keine zusätzliche RDS-Hardware (z. B. klassischer SAA6588 oder vergleichbar) benötigt – nur der Pico, ein Bandpass um 57 kHz und zwei Widerstände.

```
=== RDS-Status  t=25s ==============================
Empfang : SYNC  Traeger -15.1 Hz  Lock 0.96  Pegel 1956..2261 (DC 2105)  Symbol-Takt -6830 ppm
          Bloecke ok 1116 (repariert 0, fehlerhaft 0 = 100% gut)  Gruppen 279  Sync-Verluste 0
Sender  : PI 7012  PS "K-98.3 "  PTY 7 Kultur  TP- TA- Musik
Uhrzeit : 2026-10-19 17:02 lokal (UTC+02:00, CT-Gruppe vor 12s)
Text    : "Music That Makes You Feel Good! / Long Island's K-98.3"
```

---

## Inhalt

1. [Was ist RDS?](#was-ist-rds)
2. [Signalkette dieses Decoders](#signalkette-dieses-decoders)
3. [Hardware](#hardware)
4. [Bauen und Flashen](#bauen-und-flashen)
5. [Bedienung und Ausgabe](#bedienung-und-ausgabe)
6. [Testen am PC ohne Hardware](#testen-am-pc-ohne-hardware)
7. [Projektstruktur](#projektstruktur)
8. [Grenzen und mögliche Erweiterungen](#grenzen-und-mögliche-erweiterungen)

---

## Was ist RDS?

Das **Radio Data System** (RDS, in den USA als RBDS bekannt; Norm IEC 62106 / EN 50067) überträgt zusätzlich zum Audio kleine Datenmengen im UKW-Rundfunk. Praktisch jedes Autoradio nutzt es für den Sendernamen, Verkehrsmeldungen oder die automatische Sendersuche.

### Wo steckt RDS im UKW-Signal?

Das demodulierte UKW-Signal (das *MPX-Signal*, „Multiplex") enthält mehrere Anteile übereinander:

| Frequenzbereich | Inhalt |
|---|---|
| 0 – 15 kHz | Mono-Summensignal (L+R) |
| 19 kHz | Stereo-Pilotton |
| 23 – 53 kHz | Stereo-Differenzsignal (L−R), um 38 kHz |
| **57 kHz ± 2,4 kHz** | **RDS** (3. Harmonische des Pilottons) |

Der RDS-Träger liegt bei exakt 57 kHz und ist mit dem Pilotton phasenstarr verkoppelt. Er trägt ein **BPSK-Signal** (Phasenumtastung) mit nur ca. 2–10 % des Gesamthubs – also ein sehr schwaches, schmalbandiges Signal neben starkem Audio.

### Wie sind die Daten codiert?

* **Datenrate:** 1187,5 Bit/s (= 57 kHz / 48).
* **Differenzielle Codierung:** Ein Bit entspricht einem *Wechsel* bzw. *Nicht-Wechsel* des Vorzeichens – dadurch ist die absolute Polarität der Trägerphase egal (wichtig für die Costas-Schleife, die einen 180°-Zweifel hat).
* **Biphase (Manchester)-Symbole:** Jedes Bit besteht aus einer positiven und einer negativen Halbwelle. Das Spektrum hat dadurch *keine* Energie direkt auf dem Träger (kein Trägerrest), die Hauptkeule liegt bei ±1,2 kHz um 57 kHz.
* **Blöcke und Gruppen:** Je 16 Datenbits + 10 Prüfbits = **Block** (26 Bit). Vier Blöcke (A, B, C bzw. C', D) bilden eine **Gruppe** (104 Bit, ca. 11,4 Gruppen/s).
* **Prüfsumme (CRC):** Die 10 Prüfbits werden mit einem **Offset-Wort** (A, B, C, C', D) verknüpft. Dadurch erkennt der Empfänger *ohne* zusätzliche Synchronisationsbits, an welcher Stelle im Bitstrom er sich befindet – und kann sogar kurze Fehlerbündel (Bursts) korrigieren.

### Was steht in den Gruppen?

| Block | Inhalt |
|---|---|
| A | **PI** (Programme Identification, 16 Bit) – eindeutige Senderkennung, enthält u. a. das Land |
| B | Gruppentyp (4 Bit), Version A/B, TP-Flag, **PTY** (Programmtyp, 5 Bit), gruppenspezifische Bits |
| C, D | Nutzdaten je nach Gruppentyp |

Die wichtigsten Gruppentypen (von diesem Decoder ausgewertet):

| Gruppe | Inhalt |
|---|---|
| **0A/0B** | **PS** – Programme Service Name (8 Zeichen, in 4 Segmenten à 2 Zeichen), TA/TP, Musik/Sprache |
| **1A** | Slow Labelling, u. a. **ECC** (Extended Country Code) |
| **2A/2B** | **RadioText** (bis 64 Zeichen, in Segmenten à 4 bzw. 2 Zeichen; A/B-Flag signalisiert neuen Text) |
| **3A** | ODA-Ankündigung („Open Data Application") – hier **RadioText Plus (RT+)**, AID `4BD7` |
| **4A** | **CT** – Clock Time: Datum (Modified Julian Date), UTC-Uhrzeit, lokaler Offset; einmal pro Minute |
| *RT+-Gruppe* | Markierungen („Tags") in den RadioText: welche Zeichen sind **Titel**, **Interpret**, Album usw. |

---

## Signalkette dieses Decoders

Die Abtastrate ist bewusst **das Vierfache des RDS-Trägers (228 kS/s)**. Dadurch ist der Mischer auf 57 kHz mathematisch trivial: Der Träger ist nur die Folge `1, 0, −1, 0` (Kosinus) bzw. `0, −1, 0, 1` (Sinus) – es sind keine Multiplikationen und keine Tabellen nötig.

```
ADC0 (GPIO26) ──DMA──► [DC-Tracking] ─► [Mischer 57 kHz, ÷4] ─► [FIR-Tiefpass, ÷6]
   228 kS/s                228 k              57 k                    9,5 kHz
                                                                         │
 Bits ◄─ [Diff.-Dekoder] ◄─ [Takt-Regelung + Manchester-Korrelator] ◄─ [Costas-Schleife]
   │            1187,5 Bit/s            8 Samples pro Symbol               (BPSK, PI-Regler)
   ▼
[Block-Sync / CRC / Burstkorrektur] ─► Gruppen ─► [Parser] ─► serielle Ausgabe
```

| Stufe | Datei | Beschreibung |
|---|---|---|
| **ADC + DMA** | `main.c` | ADC-Freilauf mit 48 MHz/210,5 ≈ 228 kS/s, zwei DMA-Kanäle im Ping-Pong (je 1536 Samples = 6,7 ms), Interrupt pro Block |
| **DC-Tracking** | `rds_dsp.c` | Einpol-Tiefpass schätzt den DC-Anteil (Bauteiltoleranzen des Spannungsteilers) und zieht ihn **pro Sample mitlaufend** ab. Zeitkonstante anfangs kurz (schnelles Einschwingen), dann ca. 18 ms |
| **Mischer + ÷4** | `rds_dsp.c` | Mit der Folge `1,0,−1,0` entsteht aus je vier Samples ein komplexer Wert: `I = x0 − x2`, `Q = x3 − x1` (Basisbandrate 57 kHz). Die Differenzbildung unterdrückt exakt DC und 114 kHz, **Audio unter ca. 4 kHz klappt aber nur mit −23 … −37 dB in das RDS-Band zurück** – ein guter analoger Bandpass ist daher Pflicht |
| **FIR-Tiefpass ÷6** | `rds_dsp.c`, `rds_fir.h` | 48 Taps (Q15, mit `tools/gen_fir.py` erzeugt), Durchlass bis 2,2 kHz, Sperrbereich ab 5,6 kHz (≥ 44 dB). Ergebnis: 9,5 kHz = **8 Samples pro Symbol**. Pilot (19 kHz) und Stereo-Reste ab ca. 8 kHz Abstand werden ≥ 60 dB unterdrückt; direkt am Träger ist die Flanke flach (52/62 kHz: −23 dB, 53 kHz: −7 dB) |
| **Costas-Schleife** | `rds_dsp.c` | Echte BPSK-Costas-Schleife mit NCO (1024er Sinustabelle). Phasenfehler `I·Q / Leistung` (dadurch pegelunabhängig, kein separates AGC nötig), PI-Regler 2. Ordnung. Rauschbandbreite 35 Hz beim Einfangen, automatisch 8 Hz bei Lock (Lock-Detektor). Die Schleife gleicht Quarzabweichungen und die Differenz zum Sender-Träger aus |
| **Symboltakt** | `rds_dsp.c` | Gleitender Manchester-Korrelator (Halbsinus-Gewichte über 1 Symbol); Early/Late-Detektor mit linearer Interpolation und PI-Schleife (Taktabweichung bis ca. ±500 ppm gut, bei ±2000 ppm bricht der Empfang ein). **Grobsuche** über ein Energieprofil, weil der Korrelator ein Nebenmaximum eine halbe Symbolperiode neben dem Hauptmaximum hat, auf dem die Feinschleife sonst stabil falsch rasten kann |
| **Diff.-Dekoder** | `rds_dsp.c` | Bit = Vorzeichenwechsel zum vorigen Symbol |
| **Block-Sync** | `rds_sync.c` | 26-Bit-Schieberegister, CRC-Syndrom gegen alle Offsets; Sync nach zwei gültigen Blöcken im richtigen Abstand/Reihenfolge; danach blockweise Prüfung. **Burstkorrektur** bis 2 Bit (einstellbar `RDS_FEC_MAX_BURST`); Sync-Verlust bei 10 fehlerhaften Blöcken in Folge oder > 35 von 50 |
| **Parser** | `rds_parser.c` | PS, PTY, TP/TA, ECC, RadioText (A/B-Flag, Terminator `0x0D`, Vollständigkeitsprüfung), RT+-Tags, CT inkl. Datumsberechnung, **RDS-Zeichensatz → UTF-8** (Umlaute, ß …). PI-Wechsel wird erst nach 3 Bestätigungen übernommen |

**Zwei Kerne:** Core 1 führt ADC-Verarbeitung, DSP und Block-Sync aus (zeitkritisch). Core 0 wertet die fertigen Gruppen (über eine Queue) aus und kümmert sich um USB-Ausgabe und Tastaturkommandos. So kann eine langsame USB-Ausgabe die Demodulation nie blockieren.

---

## Hardware

```
MPX vom Tuner ─► Bandpass 57 kHz ─► C (Koppelkondensator) ─┬─► ADC0 (GPIO26)
                                                           │
                                          3V3 ──[R]──┬─────┘
                                                     └──[R]── GND      (R = 2 × gleicher Wert, z. B. 4,7 k … 10 k)
```

* **Gleichspannungsarbeitspunkt:** Zwei gleiche Widerstände legen den ADC-Eingang auf Bereichsmitte (≈ 2048). Bauteiltoleranzen sind unkritisch – die Firmware rechnet den DC-Rest mitlaufend heraus.
* **Pegel:** Das RDS-Signal sollte nach dem Bandpass mindestens einige zehn ADC-Counts Amplitude haben (Status-Zeile „Pegel min..max" prüfen), ggf. vor dem ADC verstärken. Nicht übersteuern (0 bzw. 4095).
* **Bandpass:** Ein Bandpass um 57 kHz (ca. ±3 kHz) ist wichtig: Er muss Mono-Audio (< 15 kHz), Pilotton (19 kHz) und das Stereo-Differenzsignal (bis 53 kHz) deutlich abschwächen, denn die digitale Eingangsstufe dieses Decoders sperrt Audio unter 4 kHz nur schwach.
* **Rauschen:** Auf dem Pico wird GPIO23 (SMPS-PS) auf HIGH gesetzt (PWM-Modus des Schaltreglers → sauberere ADC-Versorgung). Zusätzlich helfen kurze Leitungen und ein gut entkoppeltes 3V3.
* **Abtastrate:** 48 MHz / 210,527 = 227,999 kS/s (−6 ppm, ≈ 0,3 Hz Ablage bei 57 kHz; wird von der Costas-Schleife ausgeglichen). Die Schleife fängt Trägerablagen bis ±20 Hz in < 0,3 s, ±50 Hz in ca. 1,5 s und ±100 Hz erst nach ca. 6 s ein.


---

## Bauen und Flashen

Voraussetzungen: [Pico SDK](https://github.com/raspberrypi/pico-sdk) (inkl. Submodule `tinyusb`), CMake, `arm-none-eabi-gcc`.

```bash
export PICO_SDK_PATH=/pfad/zum/pico-sdk
cmake -S . -B build -DPICO_BOARD=pico
make -C build
```

`build/rds_decoder.uf2` bei gedrückter BOOTSEL-Taste auf das USB-Laufwerk des Pico kopieren. Danach erscheint ein serieller USB-Port (CDC, die Baudrate ist egal) – mit einem beliebigen Terminalprogramm öffnen (UTF-8!).

---

## Bedienung und Ausgabe

Alle 5 Sekunden (`RDS_STATUS_INTERVAL_MS` in `src/rds_config.h`) wird ein Statusblock ausgegeben:

| Zeile | Bedeutung |
|---|---|
| **Empfang** | `SYNC`/`suche`, Trägerablage der Costas-Schleife in Hz, Lock-Wert (≈ 1 = gerastet), ADC-Pegel min..max und DC-Wert, Symboltakt-Abweichung |
| Blöcke | Anzahl guter / reparierter / fehlerhafter Blöcke, Gruppen, Sync-Verluste |
| **Sender** | PI (hex, ggf. Land), PS (Sendername), PTY, TP/TA, Musik/Sprache |
| **Uhrzeit** | lokale Zeit aus der CT-Gruppe, zwischen den Minuten-Gruppen mitgezählt |
| **Text / Titel / Interpret** | RadioText; bei RT+ zusätzlich getrennt Titel, Interpret, Album |

Tastaturkommandos im Terminal:

| Taste | Funktion |
|---|---|
| `v` | Rohgruppen an/aus (`RDS 7012  00E8  E0CD  596F   0A`; `----` = Block fehlerhaft, `*` = per Burstkorrektur repariert) |
| `s` | Status sofort ausgeben |
| `r` | RDS-Daten zurücksetzen (z. B. nach Senderwechsel) |
| `h` | Hilfe |

Die LED des Pico leuchtet, solange der Block-Sync steht.

---

## Testen am PC ohne Hardware

Der gesamte DSP-/Dekoder-Code ist hardwareunabhängig und läuft auch auf dem PC. Als Testsignal dient eine MP3 mit RDS-Basisband (hier ca. 2,2 kHz Träger bei 44,1 kHz); `make_testsignal.py` setzt sie auf 57 kHz / 228 kS/s um und erzeugt ein ADC-ähnliches 12-Bit-Signal mit DC-Offset und Rauschen.

```bash
python3 tools/make_testsignal.py deine.mp3 test/mpx228k.u16
gcc -O2 -Wall -o test/host_test test/host_test.c src/rds_dsp.c src/rds_sync.c src/rds_parser.c -lm
test/host_test test/mpx228k.u16 -v
```

`tools/make_mpx_test.py` erzeugt zusätzlich ein realistischeres Testsignal mit Audio, Pilot und Stereo-Störern, einem analogen Frontend-Modell (Bandpass 2./4./8. Ordnung oder nur Hochpass) und ADC-Rauschen/-Nichtlinearität (Parameter siehe Kopf der Datei).

Optionen des Generators `make_testsignal.py`: `--amp` (RDS-Amplitude in Counts), `--noise`, `--dc`, `--pilot` (19-kHz-Rest), `--foff` (Trägerversatz in Hz), `--ppm` (Takt­fehler), `--fc` (Trägerfrequenz in der MP3).

**Ergebnisse mit dem mitgelieferten Testsignal** (30 s, US-Sender „K-98.3", PI 7012):

| Szenario | Gute Blöcke |
|---|---|
| Ideal (Amplitude 150, Rauschen 6 Counts) | 100 % |
| Träger +25 Hz, +150 ppm, Pilotrest 80 Counts, Rauschen 20 Counts | 99 % |
| Träger −30 Hz, −200 ppm, Rauschen 40 Counts bei Amplitude 80 | 99 % |
| Rauschen 80 Counts bei Amplitude 100 (SNR < 0 dB im ADC-Band) | 99 % |

Zusätzlich läuft der Decoder auf echter Hardware an einem analogen Radio auch bei schwächeren Signalen zuverlässig (Rückmeldung des Entwicklers).

---

## Projektstruktur

```
CMakeLists.txt            Pico-SDK-Projekt (Code läuft aus dem RAM)
pico_sdk_import.cmake
src/
  main.c                  ADC + DMA (Ping-Pong), Core-1-Loop, Core-0-Konsole
  rds_config.h            zentrale Konstanten (Raten, Schleifenbandbreiten, FEC, Status-Intervall)
  rds_dsp.c/.h            DC-Tracking, Mischer, FIR, Costas, Symboltakt, Diff.-Dekoder
  rds_fir.h               FIR-Koeffizienten (erzeugt von tools/gen_fir.py)
  rds_sync.c/.h           Block-Sync, CRC-Syndrome, Burstkorrektur, Gruppenbildung
  rds_parser.c/.h         Gruppenauswertung, Zeichensatz, Statusausgabe
tools/
  gen_fir.py              FIR-Entwurf (scipy) → rds_fir.h
  make_testsignal.py      MP3/Basisband → 57-kHz-ADC-Testsignal
test/
  host_test.c             Decoder auf dem PC (liest rohe uint16-Samples)
```

---

## Grenzen und mögliche Erweiterungen

* **Nicht ausgewertet:** AF-Listen (alternative Frequenzen), TMC/Verkehrsfunk, EON, RDS2. Die Rohgruppen (`v`) liefern die Daten dafür.
* **RT+, CT:** implementiert und gegen die Spezifikation geprüft, aber mangels passender Testdaten nicht mit realen Sendungen validiert – bitte mit eigenen Empfangsdaten testen.
* **Sync nach Bitschlupf:** Fällt der Takt um ein Bit zurück, wird der Sync nach 10 fehlerhaften Blöcken (≈ 0,2 s) verworfen und neu gesucht.
* **Scrollende PS:** Manche Sender (z. B. das Testsignal) schicken den Sendernamen als Laufschrift; das PS-Feld zeigt dann wechselnde Teile, der RadioText ist meist aussagekräftiger.
* **Länderkennung:** Nur DE/AT/CH werden aus ECC + PI-Land-Nibble als Kürzel angezeigt.
* **Ideen:** 19-kHz-Pilot als Träger-/Taktreferenz nutzen (bessere Empfindlichkeit), Soft-Decision-Dekodierung, AF/EON-Anzeige, Ausgabe als JSON, Anbindung an ein Display.

