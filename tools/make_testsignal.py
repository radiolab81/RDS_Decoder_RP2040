#!/usr/bin/env python3
"""Macht aus der MP3 (RDS-Basisband, Traeger ~2190 Hz @ 44,1 kHz) ein ADC-artiges Testsignal:
57-kHz-Traeger, 228 kS/s, 12 Bit unsigned mit Offset (Mitte+DC-Fehler), Rauschen, optional
Pilotrest.  Ausgabe: rohe uint16 little endian."""
import numpy as np, scipy.signal as sg, subprocess, argparse
from scipy.interpolate import CubicSpline
ap=argparse.ArgumentParser()
ap.add_argument("mp3"); ap.add_argument("out")
ap.add_argument("--amp",type=float,default=150,help="RDS-Spitzenamplitude in ADC-Counts")
ap.add_argument("--dc",type=float,default=2048+57,help="DC-Offset in Counts (Bauteiltoleranz)")
ap.add_argument("--noise",type=float,default=6,help="Rausch-RMS in Counts")
ap.add_argument("--pilot",type=float,default=0,help="19-kHz-Pilotrest in Counts")
ap.add_argument("--foff",type=float,default=0,help="zusaetzlicher Traegerversatz Hz")
ap.add_argument("--ppm",type=float,default=0,help="ADC-Takt-Fehler ppm")
ap.add_argument("--fc",type=float,default=2190.0,help="Traegerfrequenz in der MP3")
ap.add_argument("--fs",type=float,default=228000.0)
a=ap.parse_args()
raw=subprocess.run(["ffmpeg","-v","error","-i",a.mp3,"-f","f32le","-ac","1","-"],capture_output=True).stdout
x=np.frombuffer(raw,dtype="<f4").astype(np.float64)
z=sg.hilbert(x)                                   # analytisches Signal @44.1k
z=sg.resample_poly(z,760,147)                     # 44,1 kHz -> 228 kHz (exakt: 228000/44100 = 760/147)
fs0=228000.0
t0=np.arange(len(z))/fs0
cs=CubicSpline(t0,z)
fsa=a.fs*(1+a.ppm*1e-6)                           # tatsaechliche ADC-Rate
n=int((t0[-1]-0.01)*fsa)
t=np.arange(n)/fsa+0.005
zz=cs(t)
df=57000.0-a.fc+a.foff
s=np.real(zz*np.exp(2j*np.pi*df*t))
s*=a.amp/np.max(np.abs(s))*1.0
rng=np.random.default_rng(1)
s+=rng.normal(0,a.noise,len(s))
if a.pilot: s+=a.pilot*np.sin(2*np.pi*19000*t)
s+=a.dc
adc=np.clip(np.round(s),0,4095).astype("<u2")
adc.tofile(a.out)
print("samples",len(adc),"dauer %.2fs"%(len(adc)/a.fs),"mean %.1f std %.1f"%(adc.mean(),adc.std()))
