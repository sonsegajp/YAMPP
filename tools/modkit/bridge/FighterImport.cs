using HSDRaw;
using HSDRaw.Common;
using HSDRaw.Common.Animation;
using HSDRaw.GX;
using HSDRaw.Tools;
using HSDRaw.Melee.Pl;
using HSDRaw.Melee.Cmd;
using System.Text.Json;

static partial class Bridge {
 static HSD_JOBJ[] ImportJoints(JsonElement model) {
  var bones=model.GetProperty("bones").EnumerateArray().ToArray();var joints=bones.Select(_=>Joint()).ToArray();var tails=new HSD_JOBJ[joints.Length];
  for(int i=0;i<bones.Length;i++){
   var b=bones[i];var j=joints[i];var t=Floats(b.GetProperty("translation"));var r=Floats(b.GetProperty("rotation"));var s=Floats(b.GetProperty("scale"));
   j.TX=t[0];j.TY=t[1];j.TZ=t[2];j.RX=r[0];j.RY=r[1];j.RZ=r[2];j.SX=s[0];j.SY=s[1];j.SZ=s[2];
   int parent=b.GetProperty("parent").GetInt32();if(parent>=i)throw new Exception("Bones must be in depth-first order");
   if(parent>=0){if(tails[parent]==null)joints[parent].Child=j;else tails[parent].Next=j;tails[parent]=j;}
   if(b.TryGetProperty("inverseBind",out var inv)) {var v=Floats(inv);var m=new HSD_Matrix4x3();for(int k=0;k<12;k++)m._s.SetFloat(k*4,v[k]);j.InverseWorldTransform=m;}
  }
  return joints;
 }
 static void CompileModel(JsonElement model,string output) {
  var joints=ImportJoints(model);var root=joints[0];HSD_DOBJ tail=null;
  var images=new Dictionary<string,HSD_Image>();
  foreach(var mesh in model.GetProperty("meshes").EnumerateArray()){
   var gen=new POBJ_Generator{UseTriangleStrips=true,CullMode=GenCullMode.None};
   var ps=mesh.GetProperty("positions").EnumerateArray().Select(Floats).ToArray();var ns=mesh.GetProperty("normals").EnumerateArray().Select(Floats).ToArray();var uv=mesh.GetProperty("texcoords").EnumerateArray().Select(Floats).ToArray();
   var skin=mesh.GetProperty("skin").EnumerateArray().Select(a=>a.EnumerateArray().Select(v=>v.GetInt32()).ToArray()).ToArray();var weights=mesh.GetProperty("weights").EnumerateArray().Select(Floats).ToArray();
   var vertices=new List<GX_Vertex>();var binds=new List<HSD_JOBJ[]>();var values=new List<float[]>();
   foreach(int i in mesh.GetProperty("indices").EnumerateArray().Select(v=>v.GetInt32())){
    vertices.Add(new GX_Vertex{POS=new GXVector3(ps[i][0],ps[i][1],ps[i][2]),NRM=new GXVector3(ns[i][0],ns[i][1],ns[i][2]),TEX0=new GXVector2(uv[i][0],uv[i][1])});
    var used=Enumerable.Range(0,weights[i].Length).Where(k=>weights[i][k]>0).ToArray();binds.Add(used.Select(k=>joints[skin[i][k]]).ToArray());values.Add(used.Select(k=>weights[i][k]).ToArray());
   }
   var mat=new HSD_Material{AMB_R=160,AMB_G=160,AMB_B=160,AMB_A=255,DIF_R=255,DIF_G=255,DIF_B=255,DIF_A=255,Alpha=1};
   var mo=new HSD_MOBJ{Material=mat,RenderFlags=RENDER_MODE.DIFFUSE|RENDER_MODE.ALPHA_MAT};
   var attrs=new List<GXAttribName>{GXAttribName.GX_VA_PNMTXIDX,GXAttribName.GX_VA_POS,GXAttribName.GX_VA_NRM};
   if(mesh.TryGetProperty("texture",out var tx)&&tx.ValueKind!=JsonValueKind.Null){
    var to=new HSD_TOBJ{SX=1,SY=1,SZ=1,RepeatS=1,RepeatT=1,Flags=TOBJ_FLAGS.LIGHTMAP_DIFFUSE|TOBJ_FLAGS.COLORMAP_MODULATE|TOBJ_FLAGS.ALPHAMAP_MODULATE,Blending=1,MagFilter=(GXTexFilter)1,TexMapID=GXTexMapID.GX_TEXMAP0,GXTexGenSrc=GXTexGenSrc.GX_TG_TEX0,WrapS=GXWrapMode.CLAMP,WrapT=GXWrapMode.CLAMP};
    var raw=Convert.FromBase64String(tx.GetProperty("rgba").GetString());bool alpha=tx.TryGetProperty("alphaMode",out var mode)&&mode.GetString()!="OPAQUE";
    if(!alpha)for(int k=3;k<raw.Length;k+=4)raw[k]=255;
    string imageKey=tx.GetProperty("width").GetInt32()+":"+tx.GetProperty("height").GetInt32()+":"+Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(raw));if(images.TryGetValue(imageKey,out var shared))to.ImageData=shared;else{to.EncodeImageData(SwapRedBlue(raw),tx.GetProperty("width").GetInt32(),tx.GetProperty("height").GetInt32(),GXTexFmt.RGBA8,GXTlutFmt.RGB5A3);images[imageKey]=to.ImageData;}mo.Textures=to;mo.RenderFlags|=RENDER_MODE.TEX0;if(alpha)mo.RenderFlags|=RENDER_MODE.XLU;attrs.Add(GXAttribName.GX_VA_TEX0);
   }
   var dobj=new HSD_DOBJ{Mobj=mo,Pobj=gen.CreatePOBJsFromTriangleList(vertices,attrs.ToArray(),binds,values)};if(tail==null)root.Dobj=dobj;else tail.Next=dobj;tail=dobj;gen.SaveChanges();
  }
  root.UpdateFlags();root.Flags|=JOBJ_FLAG.OPA|JOBJ_FLAG.XLU|JOBJ_FLAG.ROOT_OPA|JOBJ_FLAG.ROOT_XLU;
  var file=new HSDRawFile();file.Roots.Add(new HSDRootNode{Name=model.GetProperty("symbol").GetString(),Data=root});file.Save(output);
 }
 static List<FOBJKey> LinearKeys(float[] v,float epsilon){
  var keep=new SortedSet<int>{0,v.Length-1};var pending=new Stack<(int,int)>();pending.Push((0,v.Length-1));
  while(pending.Count>0){var(a,b)=pending.Pop();if(b-a<2)continue;float error=epsilon;int at=-1;for(int i=a+1;i<b;i++){float e=Math.Abs(v[i]-(v[a]+(v[b]-v[a])*(i-a)/(b-a)));if(e>error){error=e;at=i;}}if(at>=0){keep.Add(at);pending.Push((a,at));pending.Push((at,b));}}
  if(v.All(n=>Math.Abs(n-v[0])<=epsilon))keep=new SortedSet<int>{0};
  return keep.Select(i=>new FOBJKey{Frame=i,Value=v[i],InterpolationType=GXInterpolationType.HSD_A_OP_LIN}).ToList();
 }
 static List<FOBJKey> SplineKeys(List<FOBJKey> source,float epsilon){
  if(source.Count<=2)return source;
  if(source.All(k=>Math.Abs(k.Value-source[0].Value)<=epsilon&&Math.Abs(k.Tan)<=epsilon))return new List<FOBJKey>{source[0]};
  var keep=new SortedSet<int>{0,source.Count-1};var stack=new Stack<(int,int)>();stack.Push((0,source.Count-1));
  while(stack.Count>0){var(a,b)=stack.Pop();if(b-a<2)continue;var first=source[a];var last=source[b];float span=last.Frame-first.Frame;if(span<=0)return source;float worst=epsilon;int split=-1;
   for(int i=a+1;i<b;i++){float t=(source[i].Frame-first.Frame)/span;float u=t-1;float value=first.Value+(source[i].Frame-first.Frame)*u*(u*first.Tan+t*last.Tan)+t*t*(3-2*t)*(last.Value-first.Value);float error=Math.Abs(value-source[i].Value);if(error>worst){worst=error;split=i;}}
   if(split>=0){keep.Add(split);stack.Push((a,split));stack.Push((split,b));}
  }
  return keep.Select(i=>source[i]).ToList();
 }
 static void CompileAnimations(string input,string output){
  Directory.CreateDirectory(output);var report=new List<object>();
  foreach(var path in Directory.GetFiles(input,"*.json").Order()){
   var a=JsonDocument.Parse(File.ReadAllText(path)).RootElement;var nodes=new List<FigaTreeNode>();
   foreach(var n in a.GetProperty("nodes").EnumerateArray()){
    var node=new FigaTreeNode();foreach(var t in n.EnumerateArray()){
     int type=t.GetProperty("type").GetInt32();var values=Floats(t.GetProperty("values"));if(values.Length==0)continue;
     List<FOBJKey> keys;
     if(t.TryGetProperty("keys",out var original)){keys=original.EnumerateArray().Select(k=>new FOBJKey{Frame=k.GetProperty("frame").GetSingle(),Value=k.GetProperty("value").GetSingle(),Tan=k.GetProperty("tangent").GetSingle(),InterpolationType=GXInterpolationType.HSD_A_OP_SPL}).ToList();if(keys[0].Frame>0)keys.Insert(0,new FOBJKey{Frame=0,Value=values[0],Tan=0,InterpolationType=GXInterpolationType.HSD_A_OP_SPL});}
     else keys=LinearKeys(values,type<=3?0.0003f:0.0005f);
     var f=FOBJFrameEncoder.EncodeFrames(SplineKeys(keys,0.0003f),(byte)type,0.00001f);var linear=FOBJFrameEncoder.EncodeFrames(LinearKeys(values,0.0003f),(byte)type,0.00001f);if(linear.Buffer.Length<f.Buffer.Length)f=linear;var player=new FOBJ_Player(f);float error=Enumerable.Range(0,values.Length).Max(i=>Math.Abs(player.GetValue(i)-values[i]));
     if(error>0.001f){Console.Error.WriteLine("Spline fallback "+Path.GetFileName(path)+" track "+type+" error "+error);f=FOBJFrameEncoder.EncodeFrames(LinearKeys(values,0.0003f),(byte)type,0.000001f);}
     node.Tracks.Add(new HSD_Track(f));
    }nodes.Add(node);
   }
   var tree=new HSD_FigaTree{Type=1,FrameCount=a.GetProperty("frames").GetSingle(),Nodes=nodes};var file=new HSDRawFile();string name=Path.GetFileNameWithoutExtension(path);file.Roots.Add(new HSDRootNode{Name="PlyDiddy5K_Share_ACTION_"+name+"_figatree",Data=tree});string dest=Path.Combine(output,name+".dat");file.Save(dest);report.Add(new{name,bytes=new FileInfo(dest).Length,frames=tree.FrameCount});
  }
  File.WriteAllText(Path.Combine(output,"animation-sizes.json"),JsonSerializer.Serialize(report));
 }
 static void CompileFighter(HSDRawFile file,JsonElement config,string output){
  var data=file.Roots.Select(r=>r.Data).OfType<SBM_FighterData>().First();
  var bank=config.GetProperty("animations");var aliases=config.GetProperty("aliases");var mappings=new List<object>();var fallbacks=new List<object>();
  string Name(SBM_FighterAction a){string n=a.Name??"";int at=n.IndexOf("_ACTION_");return at>=0?n[(at+8)..].Replace("_figatree",""):n;}
  SBM_FighterAction ConvertAction(SBM_FighterAction action,int id,string scope){
   string source=Name(action),clip=source;if(scope!="lua"&&aliases.TryGetProperty(source,out var alias))clip=alias.GetString();
   if(scope=="fighter"&&config.TryGetProperty("actionOverrides",out var overrides)&&overrides.TryGetProperty(id.ToString(),out var replacement))clip=replacement.GetString();
   if(!bank.TryGetProperty(clip,out var entry)){fallbacks.Add(new{id,scope,source});clip=source.Contains("Air")||source.Contains("Fall")?"Fall":"Wait1";entry=bank.GetProperty(clip);}
   action.Name="PlyDiddy5K_Share_ACTION_"+clip+"_figatree";action.AnimationOffset=entry.GetProperty("offset").GetInt32();action.AnimationSize=entry.GetProperty("size").GetInt32();
   action.SubAction=new SBM_FighterSubactionData();action.SubAction._s.SetData(new byte[4]);
   action.Flags=(action.Flags&~0x40000000u)|(entry.GetProperty("loop").GetBoolean()?0x40000000u:0);action._s.SetInt32(0x14,0);mappings.Add(new{id,scope,source,clip});return action;
  }
  var actions=data.FighterActionTable.Commands.Select((a,i)=>ConvertAction(a,i,"fighter")).ToList();
  foreach(var entry in bank.EnumerateObject()){int id=actions.Count;actions.Add(ConvertAction(new SBM_FighterAction{Name="PlyDiddy5K_Share_ACTION_"+entry.Name+"_figatree",Flags=3},id,"lua"));}
  data.FighterActionTable.Commands=actions.ToArray();data.DemoActionTable.Commands=data.DemoActionTable.Commands.Select((a,i)=>ConvertAction(a,i,"demo")).ToArray();
  var names=config.GetProperty("bones");byte Bone(string n)=>(byte)names.GetProperty(n).GetInt32();
  var model=data.ModelLookupTables;model._s.SetInt32(0,0);model._s.SetInt32(8,0);
  model.CostumeVisibilityLookups=new HSDArrayAccessor<SBM_CostumeLookupTable>{Array=new[]{new SBM_CostumeLookupTable()}};
  model.CostumeMaterialLookups=new HSDArrayAccessor<SBM_CostumeMaterialLookup>{Array=new[]{new SBM_CostumeMaterialLookup()}};
  model.ItemHoldBone=Bone("RHaveN");model.ShieldBone=Bone("BustN");model.TopOfHeadBone=Bone("HeadN");model.LeftFootBone=Bone("LFootJ");model.RightFootBone=Bone("RFootJ");
  data.ModelPartAnimations=null;data.Physics=null;data.CoinCollisionSpheres=null;
  var costume=new HSDRawFile(config.GetProperty("costume").GetString());var pose=(HSD_JOBJ)costume.Roots[0].Data;pose.Dobj=null;data.ShieldPoseContainer=new SBM_ShieldModelContainer{ShieldPose=pose};data.MetalModel=null;
  foreach(var a in config.GetProperty("attributes").EnumerateObject()){var prop=data.Attributes.GetType().GetProperty(a.Name);if(prop==null)throw new Exception("Unknown attribute "+a.Name);prop.SetValue(data.Attributes,JsonSerializer.Deserialize(a.Value.GetRawText(),prop.PropertyType,Json));}
  var hurtboxes=new SBM_HurtboxBank<SBM_Hurtbox>();hurtboxes._s.Resize(8);hurtboxes.Hurtboxes=config.GetProperty("hurtboxes").EnumerateArray().Select(h=>JsonSerializer.Deserialize<SBM_Hurtbox>(h.GetRawText(),Json)).ToArray();data.Hurtboxes=hurtboxes;
  data.CenterBubble.BoneIndex=Bone("BustN");data.CenterBubble.Size=8;data.FighterBoneTable=new SBM_FighterBoneIDs{HeadBone=Bone("HeadN"),RightArm=Bone("RArmJ"),LeftArm=Bone("LArmJ"),LeftLeg=Bone("LKneeJ"),RightLeg=Bone("RKneeJ")};
  var e=data.EnvironmentCollision;e.ECBBone1=Bone("HeadN");e.ECBBone2=Bone("RShoulderJ");e.ECBBone3=Bone("LShoulderJ");e.ECBBone4=Bone("RKneeJ");e.ECBBone5=Bone("LKneeJ");e.ECBBone6=0;e.LedgeGrabWidth=10;e.LedgeGrabYOffset=8;e.LedgeGrabHeight=8;
  var ik=data.FighterIK;ik.LLegJ=Bone("LLegJ");ik.RLegJ=Bone("RLegJ");ik.LKneeJ=Bone("LKneeJ");ik.RKneeJ=Bone("RKneeJ");ik.LFootJ=Bone("LFootJ");ik.RFootJ=Bone("RFootJ");ik.LShoulderJ=Bone("LShoulderJ");ik.RShoulderJ=Bone("RShoulderJ");ik.LArmJ=Bone("LArmJ");ik.RArmJ=Bone("RArmJ");ik.LegParam=2.18f;ik.KneeParam=2.71f;ik.FootParam1=1.8f;ik.ShoulderParam=2.53f;ik.ArmParam=3.08f;
  file.Save(output);File.WriteAllText(output+".mapping.json",JsonSerializer.Serialize(new{actions=actions.Count,demoActions=data.DemoActionTable.Count,mappings,fallbacks},Json));
 }

}
