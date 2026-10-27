#!/usr/bin/env python3
"""Erzeugt src/rds_fir.h : Tiefpass-FIR fuer die Dezimation 57 kHz -> 9,5 kHz (Faktor 6).
Durchlass bis ~2,2 kHz (RDS-Hauptkeule), Sperrbereich ab 5,5 kHz (Aliasbaender
k*9500 +- 2400 Hz und alles bis 28,5 kHz)."""
import numpy as np, scipy.signal as sg, sys
FS=57000.0; NTAPS=48; PASS=2200.0; STOP=5600.0; BETA=4.0
h=sg.firwin(NTAPS,(PASS+STOP)/2,window=('kaiser',BETA),fs=FS)
h/=h.sum()                       # DC-Verstaerkung 1
q=np.round(h*32768).astype(int)  # Q15
w,H=sg.freqz(q/32768.0,worN=8192,fs=FS)
def at(f): return 20*np.log10(abs(H[np.argmin(abs(w-f))])+1e-12)
print("taps",NTAPS,"sum",q.sum(),"1.2k %.2f dB 2.2k %.2f dB 5.6k %.1f dB max>5.6k %.1f dB"%(
  at(1200),at(2200),at(5600),20*np.log10(abs(H[w>5600]).max())),file=sys.stderr)
with open(sys.argv[1] if len(sys.argv)>1 else "src/rds_fir.h","w") as f:
    f.write("/* automatisch erzeugt von tools/gen_fir.py - nicht von Hand aendern */\n")
    f.write("#pragma once\n#define RDS_FIR_TAPS %d\n"%NTAPS)
    f.write("static const int16_t rds_fir_q15[RDS_FIR_TAPS] = {\n")
    for i in range(0,NTAPS,8): f.write("    "+", ".join("%6d"%v for v in q[i:i+8])+",\n")
    f.write("};\n")
