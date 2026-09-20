"""Canonical typed XML manifests for Workshop packages; one-way legacy migration."""
from pathlib import Path
import json, math, xml.etree.ElementTree as ET

def write_manifest(folder, value):
    folder=Path(folder); folder.mkdir(parents=True,exist_ok=True)
    def encode(node, value):
        if isinstance(value, dict):
            node.set("type","object")
            for name,v in value.items(): encode(ET.SubElement(node,"field",name=name),v)
        elif isinstance(value,list):
            node.set("type","array")
            for v in value: encode(ET.SubElement(node,"item"),v)
        elif isinstance(value,bool): node.set("type","bool");node.text="true" if value else "false"
        elif isinstance(value,int): node.set("type","int");node.text=str(value)
        elif isinstance(value,float):
            if not math.isfinite(value): raise ValueError("Non-finite manifest value")
            node.set("type","float");node.text=repr(value)
        elif value is None: node.set("type","null")
        else: node.set("type","string");node.text=str(value)
    root=ET.Element("melee-package",schema="1");encode(root,value);ET.indent(root)
    path=folder/"manifest.xml";temp=folder/"manifest.xml.tmp"
    ET.ElementTree(root).write(temp,encoding="utf-8",xml_declaration=True);temp.replace(path)
    return value

def read_manifest(folder):
    folder=Path(folder);path=folder/"manifest.xml"
    if not path.exists():
        return write_manifest(folder,json.loads((folder/"manifest.json").read_text()))
    raw=path.read_bytes()
    if len(raw)>1024*1024 or b"<!DOCTYPE" in raw or b"<!ENTITY" in raw:raise ValueError("Unsupported manifest XML")
    root=ET.fromstring(raw)
    if root.tag!="melee-package" or root.get("schema")!="1":raise ValueError("Unsupported package schema")
    def decode(node):
        t=node.get("type");text=node.text or ""
        if t=="object":return {n.attrib["name"]:decode(n) for n in node}
        if t=="array":return [decode(n) for n in node]
        if t=="bool":return text=="true"
        if t=="int":return int(text)
        if t=="float":
            v=float(text)
            if not math.isfinite(v):raise ValueError("Non-finite manifest value")
            return v
        if t=="null":return None
        if t=="string":return text
        raise ValueError("Invalid XML value type")
    return decode(root)

def package_folders(mods):
    return sorted({p.parent for pattern in ("*/manifest.xml","*/manifest.json") for p in Path(mods).glob(pattern)})
