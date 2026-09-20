"""Public TLS relay lifecycle check; owns and removes only its temporary room."""
import argparse,hashlib,json,socket,ssl,time,urllib.request
from pathlib import Path

class Peer:
    def __init__(self,host,name):
        self.sock=ssl.create_default_context().wrap_socket(socket.create_connection((host,443),10),server_hostname=host)
        self.sock.settimeout(10);self.buffer=b"";self.messages=[]
        self.sock.sendall(("GET /melee/ HTTP/1.1\r\nHost: %s\r\nConnection: Upgrade\r\nUpgrade: melee-netplay\r\n\r\n"%host).encode())
        while b"\r\n\r\n" not in self.buffer:self.buffer+=self.sock.recv(4096)
        headers,self.buffer=self.buffer.split(b"\r\n\r\n",1)
        assert b" 101 " in headers.split(b"\r\n",1)[0],"TLS upgrade failed"
        runtime=hashlib.sha256(b"YAMPP transport probe runtime").digest();game=hashlib.sha256(b"YAMPP transport probe data").digest()
        compatibility={"schema":1,"runtime":runtime.hex(),"game":game.hex(),"fingerprint":hashlib.sha256(b"MeleePC-compatibility-v1\0"+runtime+game).hexdigest()}
        self.send(op="hello",name=name,version=2,sync="rollback-v1",features=["compat-v1","mods-v1"],compatibility=compatibility)
        self.wait("welcome");self.wait("rooms")
    def send(self,**message):self.sock.sendall((json.dumps(message)+"\n").encode())
    def wait(self,op,predicate=lambda x:True):
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            for i,m in enumerate(self.messages):
                if m.get("op")==op and predicate(m):return self.messages.pop(i)
            while b"\n" not in self.buffer:
                data=self.sock.recv(8192)
                if not data:raise RuntimeError("Relay closed the connection")
                self.buffer+=data
            line,self.buffer=self.buffer.split(b"\n",1)
            m=json.loads(line)
            if m.get("op")=="error":raise RuntimeError(m.get("message"))
            self.messages.append(m)
        raise TimeoutError(op)
    def close(self):
        try:self.send(op="leave")
        finally:self.sock.close()

def main():
    p=argparse.ArgumentParser();p.add_argument('--host',default='mmodx.fun');p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    peers=[];report={"hostname":a.host,"transportOnly":True,"nativeGameplay":False}
    try:
        h=Peer(a.host,"YAMPP check host");peers.append(h)
        j=Peer(a.host,"YAMPP check guest");peers.append(j)
        name="YAMPP connection check "+str(int(time.time()))[-6:]
        h.send(op="create",name=name,max=2,required_mods=[],installed_mods=[])
        room=h.wait("room",lambda m:bool(m.get("room")))["room"];ident=room["id"]
        assert room['name']==name
        j.send(op="join",room=ident,installed_mods=[])
        joined=j.wait("room",lambda m:bool(m.get('room')) and len(m['room']['players'])==2)['room']
        assert joined['id']==ident and joined['name']==name
        report.update(tlsValidated=True,upgrade101=True,create=True,joinCorrectRoom=True)
        j.send(op='leave');j.wait('room',lambda m:m.get('room') is None)
        j.send(op='join',room=ident,installed_mods=[]);j.wait('room',lambda m:bool(m.get('room')) and len(m['room']['players'])==2)
        report['rejoin']=True
        h.send(op='ready',ready=True);j.send(op='ready',ready=True)
        h.wait('room',lambda m:bool(m.get('room')) and len(m['room']['players'])==2 and all(x['ready'] for x in m['room']['players']))
        h.send(op='start');host_start=h.wait('start');guest_start=j.wait('start')
        assert host_start['session']==guest_start['session'];report['readyStart']=True
        j.send(op='leave');h.wait('ended');report['disconnectEndsSession']=True
        with urllib.request.urlopen('https://'+a.host+'/',timeout=15) as r:report['siteHTTP200']=r.status==200
        with urllib.request.urlopen('https://'+a.host+'/melee/api/mods',timeout=15) as r:report['catalogHTTP200']=r.status==200
        report['passed']=True
    finally:
        for peer in reversed(peers):peer.close()
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
