"""Compare the opening movie PCM with a headless Dolphin DSP dump.

Dolphin may be terminated with unfinalized WAV chunk lengths; use the actual
available data bytes. Compare DSP samples directly, without time stretching.
"""
import argparse,json,struct
from pathlib import Path
import numpy as np
from scipy.signal import correlate,correlation_lags

p=argparse.ArgumentParser()
p.add_argument("native",type=Path);p.add_argument("reference",type=Path)
p.add_argument("--output",type=Path,required=True)
p.add_argument("--native-start",type=float,default=8.0,help="skip startup card prompts and button effects")
p.add_argument("--duration",type=float,default=10.0)
a=p.parse_args()
native=np.fromfile(a.native,dtype="<i2").reshape(-1,2).astype(np.float64)
b=a.reference.read_bytes();pos=12
while b[pos:pos+4]!=b"data":
 size=struct.unpack_from("<I",b,pos+4)[0];pos+=8+size+(size&1)
 if pos+8>len(b):raise ValueError("WAV has no data chunk")
reference=np.frombuffer(b[pos+8:len(b)-(len(b)-pos-8)%4],dtype="<i2").reshape(-1,2).astype(np.float64)
# A shared opening track must overlap for at least five seconds.
x=native[:640000].mean(axis=1)[::8];y=reference[:640000].mean(axis=1)[::8]
c=correlate(x,y,mode="full",method="fft");lags=correlation_lags(len(x),len(y))
lag=int(lags[np.argmax(c)])*8
best=None
for offset in range(lag-8,lag+9):
 ni=max(0,offset,int(a.native_start*32000));ri=ni-offset;length=min(len(native)-ni,len(reference)-ri,int(a.duration*32000))
 if length<160000:continue
 n=native[ni:ni+length];r=reference[ri:ri+length]
 corr=[float(np.corrcoef(n[:,ch],r[:,ch])[0,1]) for ch in range(2)]
 if best is None or sum(corr)>sum(best["stereo_correlation"]):
  best={"native_seconds":len(native)/32000,"offset_samples":offset,"native_start_seconds":ni/32000,"compared_seconds":length/32000,"stereo_correlation":corr,"gain":float(np.sum(n*r)/np.sum(r*r))}
if best is None:raise ValueError("Insufficient shared audio")
a.output.write_text(json.dumps(best,indent=2));print(json.dumps(best,indent=2))
if min(best["stereo_correlation"])<0.98:raise SystemExit("Opening audio correlation below 0.98")
