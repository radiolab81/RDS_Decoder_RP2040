#!/usr/bin/env python3
"""Synthetisches MPX-Testsignal mit echtem RDS-Basisband (aus MP3) + Audio/Pilot/Stereo-Stoerern,
analogem Frontend-Modell und ADC-Modell.  Ausgabe: uint16-LE @ 228 kS/s
Parameter von make(): amp (RDS-Spitze in Counts nach dem Frontend), noise (ADC-Rauschen, Counts RMS),
fe = none | bp2 | bp4 | bp8 (analoges Frontend: nur Hochpass / Bandpass 2., 4., 8. Ordnung um 57 kHz),
audio, pilot (Pegel von L+R / L-R bzw. Pilot, relativ), foff (Traegerversatz Hz), ppm (ADC-Taktfehler),
dc (DC-Offset in Counts), inl (kubische ADC-Nichtlinearitaet), seed, secs."""
import numpy as np, scipy.signal as sg, subprocess
from scipy.interpolate import CubicSpline
FS0=228000.0
def load_bb(mp3, fc=2190.0):
    raw=subprocess.run(["ffmpeg","-v","error","-i",mp3,"-f","f32le","-ac","1","-"],capture_output=True).stdout
    x=np.frombuffer(raw,dtype="<f4").astype(np.float64)
    z=sg.resample_poly(sg.hilbert(x),760,147)
    return z,fc
def pink(n,fmin,fmax,rng,fs=FS0):
    w=np.fft.rfft(rng.standard_normal(n)); f=np.fft.rfftfreq(n,1/fs)
    g=np.zeros_like(f); m=(f>=fmin)&(f<=fmax); g[m]=1/np.sqrt(f[m]); w*=g
    x=np.fft.irfft(w,n); return x/np.std(x)
def frontend(kind):
    if kind=="none": return sg.butter(1,10,"highpass",fs=FS0,output="sos")
    if kind=="bp2":  return sg.butter(1,[54000,60000],"bandpass",fs=FS0,output="sos")   # 2. Ordnung
    if kind=="bp4":  return sg.butter(2,[54000,60000],"bandpass",fs=FS0,output="sos")   # 4. Ordnung
    if kind=="bp8":  return sg.butter(4,[54000,60000],"bandpass",fs=FS0,output="sos")   # 8. Ordnung
    raise ValueError(kind)
_cache={}
def make(mp3,out,amp=100,noise=6,fe="bp4",audio=1.0,pilot=1.0,foff=0.0,ppm=0.0,dc=2048+57,inl=0.0,seed=1,fc=2190.0,secs=None):
    key=("bb",mp3)
    if key not in _cache: _cache[key]=load_bb(mp3,fc)
    z,_=_cache[key]
    fsa=FS0*(1+ppm*1e-6)
    t0=np.arange(len(z))/FS0
    n=int((t0[-1]-0.01)*fsa)
    if secs: n=min(n,int(secs*fsa))
    t=np.arange(n)/fsa+0.005
    zz=CubicSpline(t0,z)(t)
    rds=np.real(zz*np.exp(2j*np.pi*(57000-fc+foff)*t)); rds/=np.max(np.abs(rds))
    rng=np.random.default_rng(seed)
    r=0.04                                           # RDS-Spitze = 4 % des MPX-Vollhubs
    lpr=0.12*pink(n,30,15000,rng)*audio
    lmr=0.08*pink(n,30,15000,rng)*audio
    mpx=r*rds + lpr + lmr*np.cos(2*np.pi*38000*t) + 0.09*pilot*np.cos(2*np.pi*19000*t+0.7)
    sos=frontend(fe)
    xf=sg.sosfilt(sos,mpx); rf=sg.sosfilt(sos,r*rds)
    g=amp/np.max(np.abs(rf[len(rf)//10:]))
    pk=np.max(np.abs(xf[len(xf)//10:]))*g
    if pk>2000: g*=2000/pk                            # nicht uebersteuern
    x=xf*g
    rds_pk=np.max(np.abs(rf[len(rf)//10:]))*g
    x=x+noise*rng.standard_normal(n)
    if inl: x=x+inl*(x/2048)**3*2048              # kubische ADC-Nichtlinearitaet (inl = Anteil bei Vollaussteuerung)
    adc=np.clip(np.round(x+dc),0,4095).astype("<u2")
    adc.tofile(out)
    return dict(rds_peak=float(rds_pk),comp_peak=float(np.max(np.abs(x))),n=n,fs=fsa)
if __name__=="__main__":
    import sys, json
    # Aufruf: make_mpx_test.py <rds.mp3> <out.u16> [json-Parameter, z.B. '{"fe":"bp4","noise":40,"audio":3}']
    kw=json.loads(sys.argv[3]) if len(sys.argv)>3 else {}
    print(make(sys.argv[1],sys.argv[2],**kw))
