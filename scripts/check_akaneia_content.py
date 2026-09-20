"""Audit the selective local Akaneia composition against its source and base disc."""
import argparse, hashlib, json, sys, re, struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream,"sha256").hexdigest() if hasattr(hashlib,"file_digest") else hashlib.sha256(stream.read()).hexdigest()

def data(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))

def verify_stage_files(root,source,audit):
    # Independently discover all named stage dependencies; old incomplete
    # compositions fail here even if they carry a previously passing receipt.
    expected={};queue=[x["FileName"].lstrip("/") for x in audit["stages"]]
    for row in audit["stages"]:
        metadata=data(source/"data/stages"/(str(row["sourceID"]).zfill(3)+".json"))
        queue.extend(x.lstrip("/") for x in metadata.get("additionalFiles",[]))
    while queue:
        name=queue.pop()
        if name in expected:continue
        assert Path(name).name==name and ".." not in name,("Unsafe stage path",name)
        original=(source/"files"/name).read_bytes()
        actual=root/"files"/name
        assert actual.is_file(),("Missing stage dependency",name)
        expected[name]=hashlib.sha256(original).hexdigest()
        assert digest(actual)==expected[name],("Changed stage dependency",name)
        queue.extend(x.decode("ascii") for x in re.findall(rb"(?<![A-Za-z0-9_])Gr[A-Za-z0-9_]+\.(?:dat|usd|csv)(?=\x00)",original))
    assert audit.get("stageFileRevision")==1 and audit.get("stageFiles")==expected,"Stage dependency audit is missing or incomplete"
    # Check the actual exported disc, not just the composition directory.
    with (root/"yampp-content.iso").open("rb") as disc:
        disc.seek(0x424);offset,size=struct.unpack(">2I",disc.read(8));disc.seek(offset);fst=disc.read(size)
        count=struct.unpack_from(">I",fst,8)[0];names=fst[count*12:];present={}
        stack=[(count,"")]
        for i in range(1,count):
            while i>=stack[-1][0]:stack.pop()
            flags,offset,size=struct.unpack_from(">3I",fst,i*12);n=flags&0xffffff
            name=names[n:names.index(b"\0",n)].decode("ascii");relative=stack[-1][1]+name
            if flags>>24:stack.append((size,relative+"/"));continue
            if relative in expected:
                disc.seek(offset);present[relative]=hashlib.sha256(disc.read(size)).hexdigest()
        assert present==expected,"Exported disc is missing or changed stage data"
    return len(expected)

def main():
    ap=argparse.ArgumentParser();ap.add_argument("content",type=Path);ap.add_argument("--source",type=Path,default=ROOT/"build/akaneia-content/source");args=ap.parse_args()
    root=args.content.resolve();source=args.source.resolve();audit=data(root/"content-audit.json")
    assert audit["scope"]==["fighters","stages","music"]
    assert audit["optionalCodes"]==audit["optionalPatches"]==0
    names={"Wolf","Diddy Kong","Charizard","Lucas","Sonic","King Dedede","Tails"}
    assert {x["Name"] for x in audit["fighters"]}==names
    assert len(audit["stages"])==24 # 17 versus arenas and seven fighter target tests.
    assert not any(x["Name"]=="Volleyball" for x in audit["stages"])
    stage_file_count=verify_stage_files(root,source,audit)
    music=data(root/"data/music.json");original=data(source/"data/music.json")
    assert len(music)==139 and len(audit["music"])==41
    for track in audit["music"]:
        expected=source/"files/audio"/original[track["sourceID"]]["fileName"]
        actual=root/"files/audio"/track["FileName"]
        assert digest(actual)==digest(expected)==track["sha256"],track["Name"]
    for name,sha in audit["menuAssets"].items():
        assert digest(root/"files"/name)==sha,name
        assert digest(ROOT/"data/GALE01/files"/name)==sha,name
    for path in (root/"data/stages").glob("*.json"):
        stage=data(path)
        assert all(0<=x["musicID"]<len(music) for x in stage["playlist"]["entries"]),path
    for path in (root/"data/fighters").glob("*.json"):
        fighter=data(path)
        if fighter["name"] in names:
            for key in ["fighterDataPath","animFile","rstAnimFile"]:
                name=fighter["files"][key]
                assert name and (root/"files"/name).is_file(),(fighter["name"],key)
            for key in ["victoryTheme","fighterMusic1","fighterMusic2"]:
                assert 0<=fighter[key]<len(music),(fighter["name"],key)
    report={"passed":True,"stageFilesVerified":stage_file_count,"fighters":7,"versusStages":17,"targetStages":7,"musicTracks":41,"musicBytesMatchUpstream":True,"baseMenuBytesUnchanged":True,"isoSHA256":digest(root/"yampp-content.iso")}
    (root/"verification.json").write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))
if __name__=="__main__":main()
