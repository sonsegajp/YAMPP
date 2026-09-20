using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Web.Script.Serialization;
using BrawlLib.SSBB.ResourceNodes;
using BrawlLib.Internal;
using BrawlLib.Internal.Audio;
class Program {
 static string Clean(string s) { foreach(char c in Path.GetInvalidFileNameChars())s=s.Replace(c,'_');return s; }
 static float[] V(Vector3 v)=>new[]{v._x,v._y,v._z};
 static object Material(MDL0MaterialNode m)=>new{name=m.Name,blend=m.EnableBlend,alpha0=m.Comp0.ToString(),alpha1=m.Comp1.ToString(),textures=m.Children.OfType<MDL0MaterialRefNode>().Select(t=>new{name=t.Texture,coords=t.Coordinates.ToString(),uv=t.TextureCoordId,scale=new[]{t.Scale._x,t.Scale._y},rotation=t.Rotation,translation=new[]{t.Translation._x,t.Translation._y},matrix=Enumerable.Range(0,16).Select(i=>t.GetTransform(false)[i]).ToArray()}).ToArray()};
 static JavaScriptSerializer json=new JavaScriptSerializer{MaxJsonLength=int.MaxValue,RecursionLimit=512};
 static HashSet<int> requestedSounds=new HashSet<int>();
 static List<object> inventory=new List<object>();
 static HashSet<ResourceNode> visited=new HashSet<ResourceNode>();
 static void Walk(ResourceNode n,string trail,string output,bool audioOnly) {
  if(!visited.Add(n))return;
  string key=trail.Length==0?n.Name:trail+"/"+n.Name;
  inventory.Add(new{path=key,type=n.GetType().Name,soundId=n is RSARSoundNode snd?snd.InfoIndex:-1});
  if(n is RSARSoundNode sound) {
   if(key.IndexOf("diddy",StringComparison.OrdinalIgnoreCase)>=0 || requestedSounds.Contains(sound.InfoIndex)) {
    if(sound.SoundType!=RSARSoundNode.SndType.WAVE){Console.WriteLine("EXTERNAL AUDIO "+key+" "+sound.SoundFile);return;}
    string file=Path.Combine(output,Clean(key.Replace('/','_'))+".wav");
    using(var stream=sound.CreateStreams()[0]) {
     if(stream==null)throw new Exception("No audio stream: "+key);
     WAV.ToFile(stream,file);
    }
    Console.WriteLine("WAV "+key);
   }
   return;
  }
  if(!audioOnly) {
   string group=Clean(trail.Replace('/','_'));
   if(group.Length>85)group=group.Substring(0,65)+"_"+inventory.Count;
   string dir=Path.Combine(output,group);
   if(n is MDL0Node || n is TEX0Node || n is CHR0Node)Directory.CreateDirectory(dir);
   string name=Clean(n.Name);
   if(n is MDL0Node model){model.Export(Path.Combine(dir,name+".dae"));model.Export(Path.Combine(dir,name+".mdl0"));
    var meshes=model.PolygonList.OfType<MDL0ObjectNode>().Select(o=>new{name=o.Name,draws=o.DrawCalls.Cast<DrawCall>().Select(d=>new{material=d.MaterialNode?.Name,state=d.MaterialNode!=null?Material(d.MaterialNode):null,visibilityBone=d.VisibilityBoneNode?.Name,visible=d.VisibilityBoneNode?.Visible??true}).ToArray()}).ToArray();
    File.WriteAllText(Path.Combine(dir,name+".mesh-metadata.json"),json.Serialize(meshes));
    Console.WriteLine("MODEL "+key);return;}
   if(n is TEX0Node tex){tex.Export(Path.Combine(dir,name+".png"));return;}
   if(n is CHR0Node anim){
    var tracks=anim.Children.OfType<CHR0EntryNode>().Select(b=>new{name=b.Name,keys=Enumerable.Range(0,9).Select(c=>Enumerable.Range(0,b.FrameCount).Select(i=>b.GetKeyframe(c,i)).Where(k=>k!=null).Select(k=>new{frame=k._index,value=k._value,tangent=k._tangent}).ToArray()).ToArray(),frames=Enumerable.Range(0,anim.FrameCount).Select(i=>{var f=b.GetAnimFrame(i);return new{translation=V(f.Translation),rotation=V(f.Rotation),scale=V(f.Scale)};}).ToArray()}).ToArray();
    File.WriteAllText(Path.Combine(dir,name+".animation.json"),json.Serialize(new{name=anim.Name,frames=anim.FrameCount,loop=anim.Loop,bones=tracks}));
    anim.Export(Path.Combine(dir,name+".chr0"));return;
   }
  }
  foreach(var c in n.Children)Walk(c,key,output,audioOnly);
 }
 [STAThread] static int Main(string[] args){try{
  if(args.Length<3)throw new Exception("BrawlBridge <export|audio|tree> input output-directory");
  if(args.Length>3)requestedSounds=new HashSet<int>(json.Deserialize<int[]>(File.ReadAllText(args[3])));
  Directory.CreateDirectory(args[2]);
  using(var root=NodeFactory.FromFile(null,args[1])) {
   if(root==null)throw new Exception("Unsupported archive: "+args[1]);
   Walk(root,"",args[2],args[0]!="export");
  }
  File.WriteAllText(Path.Combine(args[2],"inventory.json"),json.Serialize(inventory));
  Console.WriteLine("Resource nodes: "+inventory.Count);return 0;
 }catch(Exception e){Console.Error.WriteLine(e);return 1;}}
}
