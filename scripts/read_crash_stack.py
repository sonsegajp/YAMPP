"""List return addresses from a local WER minidump for native crash diagnosis."""
import struct,sys,ctypes as C,subprocess
from pathlib import Path
p=Path(sys.argv[1]);data=p.read_bytes()
def u32(at):return struct.unpack_from('<I',data,at)[0]
def u64(at):return struct.unpack_from('<Q',data,at)[0]
streams={}
for i in range(u32(8)):
 at=u32(12)+12*i;streams[u32(at)]=(u32(at+4),u32(at+8))
mods=[];at=streams[4][1]
for i in range(u32(at)):
 b=at+4+i*108;name=u32(b+20);mods.append((u64(b),u32(b+8),data[name+4:name+4+u32(name)].decode('utf16')))
e=streams[6][1];tid=u32(e);context=u32(e+164);rsp=u64(context+0x98);rip=u64(context+0xf8)
print('EXCEPTION',hex(u32(e+8)),'thread',tid,'RIP',hex(rip),'RSP',hex(rsp),'params',[hex(u64(e+40+i*8)) for i in range(min(15,u32(e+32)))])
for b,size,name in mods:print('MODULE',hex(b),hex(size),name)
t=streams[3][1];stack=None
for i in range(u32(t)):
 at=t+4+48*i
 if u32(at)==tid:
  start=u64(at+24);size=u32(at+32);rva=u32(at+36);offset=rsp-start
  stack=data[rva+offset:rva+size] if 0<=offset<size else None
if stack is None:raise ValueError('Exception stack not present')
for i in range(0,min(len(stack),4096),8):
 v=struct.unpack_from('<Q',stack,i)[0]
 for base,size,name in mods:
  if base<=v<base+size:
   label=Path(name).name;print('STACK',hex(i),label,hex(v-base),hex(v))
