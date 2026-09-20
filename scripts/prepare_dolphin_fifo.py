"""Convert a Dolphin FIFO recording into ordered GPU commands and RAM updates."""
import argparse,struct,json
from pathlib import Path
def prepare(src,out):
    d=src.read_bytes()
    def u32(o): return struct.unpack_from("<I",d,o)[0]
    def u64(o): return struct.unpack_from("<Q",d,o)[0]
    assert u32(0)==0x0d01f1f0 and u32(4)<=6
    def regs(off,count): return struct.unpack_from("<"+"I"*count,d,off)
    bp=regs(u64(12),u32(20));cp=regs(u64(24),u32(32))
    xf=regs(u64(36),u32(44));xr=regs(u64(48),u32(56))
    frameoff=u64(60);nframes=u32(68)
    init=bytearray()
    for i,v in enumerate(bp):
        if i not in (0x45,0x47,0x48,0x52,0x63,0x65,0x67):
            init+=b"\x61"+struct.pack(">I",(i<<24)|(v&0xffffff))
    for i in [0x30,0x40,0x50,0x60]+list(range(0x70,0x78))+list(range(0x80,0x88))+list(range(0x90,0x98))+list(range(0xa0,0xc0)):
        init+=b"\x08"+bytes([i])+struct.pack(">I",cp[i])
    # Aurora decodes complete SDK matrix/viewport/projection blocks. Dolphin's
    # recorder stores raw XF RAM; restore it in those blocks rather than 16-word
    # chunks which straddle Aurora's matrix objects.
    def load_xf(address, values):
        nonlocal init
        init+=b"\x10"+struct.pack(">HH",len(values)-1,address)+struct.pack(">"+"I"*len(values),*values)
    for i in range(0,0xf0,12): load_xf(i,xf[i:i+12])
    for i in range(0x400,0x45a,9): load_xf(i,xf[i:i+9])
    for i in range(0x500,0x5f0,12): load_xf(i,xf[i:i+12])
    for i in range(0x600,0x680,16): load_xf(i,xf[i:i+16])
    for i,v in enumerate(xr):
        if i==7 or 0x13<=i<=0x17 or 0x1a<=i<=0x26 or 0x27<=i<=0x3e or 0x48<=i<=0x4f: continue
        load_xf(0x1000+i,[v])
    load_xf(0x101a,xr[0x1a:0x20])
    load_xf(0x1020,xr[0x20:0x27])
    events=[(2,0,init)]
    # The first draw may use palettes loaded before recording began. Restore
    # those from Dolphin's initial TMEM snapshot via ordinary LOADTLUT commands.
    seeded=set()
    for slot in range(8):
        bank=0x80+(slot//4)*32+slot%4
        fmt=(bp[bank+8]>>20)&15
        if fmt not in (8,9,10): continue
        palette=bp[bank+24]&1023
        entries={8:16,9:256,10:16384}[fmt]
        if palette in seeded: continue
        seeded.add(palette)
        start=u64(76)+(palette<<9)
        scratch=0x17f0000
        events.append((1,scratch,d[start:start+entries*2]))
        command=b"\x61"+struct.pack(">I",0xfeffffff)
        command+=b"\x61"+struct.pack(">I",0x64000000|(scratch>>5))
        command+=b"\x61"+struct.pack(">I",0x65000000|((entries//16)<<10)|palette)
        events.append((2,0,command))
    updates=0;tmem=0
    for index in range(nframes):
        off=frameoff+index*64
        fifo=d[u64(off):u64(off)+u32(off+8)]
        at=0
        for j in range(u32(off+28)):
            mo=u64(off+20)+j*24
            pos,addr,dataoff,size,kind=struct.unpack_from("<IIQIB",d,mo)
            assert pos<=len(fifo) and pos>=at
            if pos>at: events.append((2,0,fifo[at:pos]))
            at=pos
            # TMEM records contain the RAM source bytes of a palette upload.
            if kind==8: tmem+=1
            assert (addr&0x1ffffff)+size<=0x1800000
            events.append((1,addr&0x1ffffff,d[dataoff:dataoff+size]));updates+=1
        if at<len(fifo): events.append((2,0,fifo[at:]))
        events.append((3,0,b""))
    out.parent.mkdir(parents=True,exist_ok=True)
    with out.open("wb") as f:
        f.write(b"MGXR"+struct.pack("<I",len(events)))
        for kind,addr,data in events:
            f.write(struct.pack("<III",kind,addr,len(data)));f.write(data)
    report={"source":str(src),"frames":nframes,"events":len(events),"ram_updates":updates,"palette_source_updates":tmem}
    out.with_suffix(".json").write_text(json.dumps(report,indent=2));print(report)
if __name__=="__main__":
    p=argparse.ArgumentParser();p.add_argument("source",type=Path);p.add_argument("output",type=Path)
    a=p.parse_args();prepare(a.source,a.output)
