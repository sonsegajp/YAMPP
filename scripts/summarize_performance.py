"""Summarize native render cadence over a selected host-frame interval."""
import argparse,csv,json
from pathlib import Path
import numpy as np
p=argparse.ArgumentParser();p.add_argument("csv",type=Path)
p.add_argument("--start",type=int,default=2400);p.add_argument("--end",type=int,default=4200)
p.add_argument("--output",type=Path,required=True);a=p.parse_args()
rows=[r for r in csv.DictReader(a.csv.open()) if None not in r.values() and a.start<=int(r["host"])<=a.end]
draws=[r for r in rows if r["rendered"]=="1"]
t=np.array([float(r["seconds"])+float(r["begin_ms"])/1000+float(r["translate_ms"])/1000+float(r["end_ms"])/1000 for r in draws])
dt=np.diff(t)*1000
report={"source":str(a.csv),"host_frame_range":[a.start,a.end],"sample_seconds":float(rows[-1]["seconds"])-float(rows[0]["seconds"]),"rendered_frames":len(draws),"render_fps":(len(t)-1)/(t[-1]-t[0]),"frame_interval_ms":{"median":float(np.median(dt)),"p95":float(np.percentile(dt,95)),"max":float(dt.max())}}
report["average_work_ms"]={k:float(np.mean([float(r[k]) for r in draws])) for k in ["begin_ms","translate_ms","end_ms","ready_age_ms"]}
a.output.write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
