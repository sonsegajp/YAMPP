using HSDRaw;
using HSDRaw.Common;
using HSDRaw.Common.Animation;
using HSDRaw.GX;
using HSDRaw.Melee.Pl;
using HSDRaw.Melee.Cmd;
using HSDRaw.Melee.Mn;
using HSDRaw.Melee.Gr;
using HSDRaw.Tools;
using HSDRaw.Tools.Melee;
using System.Collections;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Serialization;

static partial class Bridge {
 static byte[] SwapRedBlue(byte[] data) { for(int i=0;i+3<data.Length;i+=4)(data[i],data[i+2])=(data[i+2],data[i]); return data; }
 static JsonSerializerOptions Json = new() { IncludeFields=true, Converters={new JsonStringEnumConverter()}, NumberHandling=JsonNumberHandling.AllowNamedFloatingPointLiterals };
 static object Props(object value, int depth=2) {
  if(value==null)return null;
  var t=value.GetType();
  if(t.IsEnum)return value.ToString();
  if(t.IsPrimitive || value is string || value is decimal)return value;
  if(value is byte[] bytes)return Convert.ToHexString(bytes);
  if(depth<0)return t.Name;
  if(value is IEnumerable seq)return seq.Cast<object>().Take(4096).Select(x=>Props(x,depth-1)).ToArray();
  var result=new Dictionary<string,object>();
  foreach(var p in t.GetProperties(BindingFlags.Public|BindingFlags.Instance)) {
   if(p.GetIndexParameters().Length!=0 || !p.CanRead || p.Name is "Parent" or "Next" or "Child" or "TrimmedSize" or "Nodes" or "Animation" || p.Name.StartsWith("_"))continue;
   try { result[p.Name]=Props(p.GetValue(value),depth-1); } catch(Exception e){result[p.Name]=new{unavailable=e.GetBaseException().Message};}
  }
  return result;
 }
 static object Graph(HSDAccessor root) {
  var ids=new Dictionary<HSDStruct,int>();var nodes=new List<object>();
  int Visit(HSDStruct s) {
   if(ids.TryGetValue(s,out int existing))return existing;
   int id=ids.Count;ids[s]=id;nodes.Add(null);
   var refs=s.References.ToDictionary(k=>k.Key.ToString("X"),v=>Visit(v.Value));
   nodes[id]=new{id,length=s.Length,bytes=Convert.ToBase64String(s.GetData()),references=refs};return id;
  }
  Visit(root._s);return nodes;
 }
 static object Visibility(SBM_FighterData fighter) {
  var tables=fighter.ModelLookupTables;
  if(tables==null)return null;
  int[][][] Groups(HSDArrayAccessor<SBM_LookupTable> groups) => groups?.Array.Take(tables.VisibilityLookupLength).Select(g=>g.LookupEntries?.Array.Take(g.Count).Select(v=>(v.Entries??Array.Empty<byte>()).Select(x=>(int)x).ToArray()).ToArray()??Array.Empty<int[]>()).ToArray();
  return new { groupCount=tables.VisibilityLookupLength,costumes=tables.CostumeVisibilityLookups.Array.Select(c=>new{high=Groups(c.HighPoly),low=Groups(c.LowPoly),metal=Groups(c.MetalPoly),metalMain=Groups(c.MetalMainModel)}) };
 }
 // Texture animation images inherit the material TObj palette when no palette
 // animation is present. Match the original joint/material/texmap hierarchy;
 // image order stays the archive graph order used by existing patch manifests.
 static Dictionary<HSDStruct,HSD_Tlut> ImagePalettes(HSDRawFile file,List<HSDStruct> structs) {
  var palettes=new Dictionary<HSDStruct,HSD_Tlut>();
  foreach(var st in structs) {
   if(st.Length<0x54||!st.References.TryGetValue(0x4c,out var image))continue;
   var palette=new HSD_TOBJ{_s=st}.TlutData;
   if(palette!=null&&!palettes.ContainsKey(image))palettes.Add(image,palette);
  }
  void Match(HSD_JOBJ joint,HSD_MatAnimJoint anim) {
   for(;joint!=null&&anim!=null;joint=joint.Next,anim=anim.Next) {
    var dobj=joint.Dobj;
    for(var ma=anim.MaterialAnimation;ma!=null&&dobj!=null;ma=ma.Next,dobj=dobj.Next) {
     for(var ta=ma.TextureAnimation;ta!=null;ta=ta.Next) {
      var texture=dobj.Mobj?.Textures;
      while(texture!=null&&(int)texture.TexMapID!=(int)ta.GXTexMapID)texture=texture.Next;
      var palette=texture?.TlutData;
      if(ta.ImageBuffers==null)continue;
      var images=ta.ImageBuffers.Array.Take(ta.ImageCount).Select(b=>b.Data).ToArray();
      var animatedPalettes=ta.TlutBuffers?.Array.Take(ta.TlutCount).Select(b=>b.Data).ToArray()??Array.Empty<HSD_Tlut>();
      if(animatedPalettes.Length==1)palette=animatedPalettes[0]??palette;
      if(palette!=null)foreach(var image in images)
       if(image!=null&&!palettes.ContainsKey(image._s))palettes.Add(image._s,palette);
      if(animatedPalettes.Length==0)continue;
      FOBJ_Player imageTrack=null,paletteTrack=null;
      for(var track=ta.AnimationObject?.FObjDesc;track!=null;track=track.Next) {
       if(track.TexTrackType==TexTrackType.HSD_A_T_TIMG)imageTrack=new FOBJ_Player(track.ToFOBJ());
       if(track.TexTrackType==TexTrackType.HSD_A_T_TCLT)paletteTrack=new FOBJ_Player(track.ToFOBJ());
      }
      if(imageTrack==null||paletteTrack==null)continue;
      // A descriptor may inherit a different TLUT at each image key. Decode
      // each image using its first actual TIMG/TCLT pairing, never array guesses.
      var paired=new HashSet<int>();
      var times=imageTrack.Keys.Select(k=>k.Frame).Concat(paletteTrack.Keys.Select(k=>k.Frame)).Append(0f).Distinct().OrderBy(f=>f);
      foreach(float frame in times) {
       int imageIndex=(int)imageTrack.GetValue(frame),paletteIndex=(int)paletteTrack.GetValue(frame);
       if(imageIndex<0||imageIndex>=images.Length||paletteIndex<0||paletteIndex>=animatedPalettes.Length||!paired.Add(imageIndex))continue;
       if(images[imageIndex]!=null&&animatedPalettes[paletteIndex]!=null)palettes[images[imageIndex]._s]=animatedPalettes[paletteIndex];
      }
     }
    }
    Match(joint.Child,anim.Child);
   }
  }
  foreach(var root in file.Roots) {
   if(root.Data is not HSD_MatAnimJoint animation)continue;
   const string suffix="_matanim_joint";
   if(!root.Name.EndsWith(suffix,StringComparison.Ordinal))continue;
   string jointName=root.Name[..^suffix.Length]+"_joint";
   if(file.Roots.FirstOrDefault(r=>r.Name==jointName)?.Data is HSD_JOBJ joint)Match(joint,animation);
  }
  return palettes;
 }
 static List<(HSD_Image image,HSD_Tlut palette)> Images(HSDRawFile file) {
  var structs=file.Roots.SelectMany(r=>r.Data._s.GetSubStructs()).Distinct().ToList();var result=new List<(HSD_Image,HSD_Tlut)>();
  var palettes=ImagePalettes(file,structs);
  foreach(var data in structs)try{
   if(data.Length!=0x18 || !data.References.ContainsKey(0))continue;
   var img=new HSD_Image{_s=data};int w=img.Width,h=img.Height,fmt=(int)img.Format;
   if(w<1||h<1||w>2048||h>2048||!(fmt<=6||fmt==8||fmt==9||fmt==10||fmt==14)||fmt<0||img.MipMap<0||img.MipMap>1)continue;
   if(!float.IsFinite(img.MinLOD)||!float.IsFinite(img.MaxLOD)||img.MinLOD<0||img.MaxLOD>12)continue;
   palettes.TryGetValue(data,out var pal);
   result.Add((img,pal));
  }catch{}
  return result;
 }
 static object ExportImages(HSDRawFile file) => Images(file).Select((entry,i)=>{
  var tobj=new HSD_TOBJ{ImageData=entry.image,TlutData=entry.palette};string rgba=null,error=null;
  try{rgba=Convert.ToBase64String(SwapRedBlue(tobj.GetDecodedImageData()));}catch(Exception ex){error=ex.Message;}
  return new{id=i,width=entry.image.Width,height=entry.image.Height,format=(int)entry.image.Format,rgba,error};
 }).ToArray();
 static void PatchImage(HSDRawFile file,JsonElement patch,string output){
  var images=Images(file);
  var patches=patch.ValueKind==JsonValueKind.Array?patch.EnumerateArray().ToArray():new[]{patch};
  foreach(var item in patches){
  patch=item;var entry=images[patch.GetProperty("id").GetInt32()];
  int width=patch.GetProperty("width").GetInt32(),height=patch.GetProperty("height").GetInt32();
  if(width!=entry.image.Width||height!=entry.image.Height)throw new Exception("Texture dimensions must match the source UV layout");
  byte[] rgba=Convert.FromBase64String(patch.GetProperty("rgba").GetString());if(rgba.Length!=width*height*4)throw new Exception("Invalid RGBA texture length");
  var tobj=new HSD_TOBJ{ImageData=entry.image,TlutData=entry.palette};
  tobj.EncodeImageData(SwapRedBlue(rgba),width,height,GXTexFmt.RGBA8,GXTlutFmt.RGB5A3);entry.image.MipMap=0;entry.image.MinLOD=entry.image.MaxLOD=0;
  }file.Save(output);
 }
 static object Inspect(HSDRawFile file) {
  var data=file.Roots.Select(r=>r.Data).OfType<SBM_FighterData>().FirstOrDefault();
  if(data==null)return new{roots=file.Roots.Select(r=>new{r.Name,type=r.Data.GetType().Name,fields=Props(r.Data),graph=Graph(r.Data)})};
  var moves=data.FighterActionTable.Commands.Select((a,i)=>{
   string script="",error=null;
   try{script=new ActionDecompiler().Decompile("action_"+i,a.SubAction);}catch(Exception e){error=e.Message;}
   return new{id=i,name=a.Name,symbol=a.SymbolName?.Value,animationOffset=a.AnimationOffset,animationSize=a.AnimationSize,flags=a.Flags,script,error};
  }).ToArray();
  return new{modelVisibility=Visibility(data),attributes=Props(data.Attributes),specialAttributes=Props(data.Attributes2),hurtboxes=Props(data.Hurtboxes),moves,sections=Props(data),graph=Graph(data)};
 }
 static object Animation(string path,int offset,int size) {
  byte[] data=File.ReadAllBytes(path);if(offset<0||size<32||(long)offset+size>data.Length)throw new Exception("Animation range exceeds AJ archive");
  var file=new HSDRawFile(data.Skip(offset).Take(size).ToArray());
  var tree=file.Roots.Select(r=>r.Data).OfType<HSD_FigaTree>().First();
  int frames=(int)Math.Ceiling(tree.FrameCount)+1;
  if(frames>10000)throw new Exception("Animation exceeds 10000 frames");
  return new{frames=tree.FrameCount,nodes=tree.Nodes.Select(n=>n.Tracks.Where(t=>t.TrackType<=10).Select(t=>{
   var player=new FOBJ_Player(t.ToFOBJ());return new{type=t.TrackType,keys=player.Keys,values=Enumerable.Range(0,frames).Select(f=>player.GetValue(f)).ToArray()};
  }).ToArray()).ToArray()};
 }
 static int[] Triangles(GX_DisplayList dl) {
  var indices=new List<int>();int offset=0;
  foreach(var pg in dl.Primitives) {
   int type=(int)pg.PrimitiveType & 0xF8;
   if(type==0x90)for(int i=0;i+2<pg.Count;i+=3)indices.AddRange(new[]{offset+i,offset+i+1,offset+i+2});
   else if(type==0x98)for(int i=2;i<pg.Count;i++)indices.AddRange(i%2==0?new[]{offset+i-2,offset+i-1,offset+i}:new[]{offset+i-1,offset+i-2,offset+i});
   else if(type==0xA0)for(int i=2;i<pg.Count;i++)indices.AddRange(new[]{offset,offset+i-1,offset+i});
   else if(type==0x80)for(int i=0;i+3<pg.Count;i+=4)indices.AddRange(new[]{offset+i,offset+i+1,offset+i+2,offset+i,offset+i+2,offset+i+3});
   offset+=pg.Count;
  }
  return indices.ToArray();
 }
 static object Model(HSDRawFile file) {
  var roots=file.Roots.Select(r=>r.Data).OfType<HSD_JOBJ>().ToList();
  foreach(var map in file.Roots.Select(r=>r.Data).OfType<SBM_Map_Head>())roots.AddRange(map.ModelGroups.Array.Select(g=>g.RootNode).Where(n=>n!=null));
  var joints=new List<HSD_JOBJ>();var parents=new List<int>();
  void Visit(HSD_JOBJ j,int parent) {for(;j!=null;j=j.Next){if(joints.Contains(j))continue;int id=joints.Count;joints.Add(j);parents.Add(parent);Visit(j.Child,id);}}
  foreach(var j in roots)Visit(j,-1);
  var meshes=new List<object>();var errors=new List<string>();var dobjs=new Dictionary<HSDStruct,int>();
  foreach(var j in joints)if((j.Flags & (JOBJ_FLAG.PTCL|JOBJ_FLAG.SPLINE))==0)for(var d=j.Dobj;d!=null;d=d.Next)if(!dobjs.ContainsKey(d._s))dobjs[d._s]=dobjs.Count;
  for(int ji=0;ji<joints.Count;ji++) {
   var j=joints[ji]; if((j.Flags & (JOBJ_FLAG.PTCL|JOBJ_FLAG.SPLINE))!=0)continue;
   for(var d=j.Dobj;d!=null;d=d.Next)for(var p=d.Pobj;p!=null;p=p.Next)try {
    var dl=new GX_DisplayList(p);var positions=new List<float[]>();var normals=new List<float[]>();var texcoords=new List<float[]>();var skin=new List<int[]>();var weights=new List<float[]>();var rigid=new List<bool>();
    foreach(var v in dl.Vertices) {
     positions.Add(new[]{v.POS.X,v.POS.Y,v.POS.Z});normals.Add(new[]{v.NRM.X,v.NRM.Y,v.NRM.Z});texcoords.Add(new[]{v.TEX0.X,v.TEX0.Y});
     var env=dl.Envelopes.Count>v.PNMTXIDX/3?dl.Envelopes[v.PNMTXIDX/3]:null;
     var js=new int[4];var ws=new float[4];
     if(env!=null) {var pairs=env.ToList().OrderByDescending(x=>x.Item2).Take(4).ToArray();for(int k=0;k<pairs.Length;k++){js[k]=Math.Max(0,joints.IndexOf(pairs[k].Item1));ws[k]=pairs[k].Item2;}rigid.Add(pairs.Length==1);}
     else {js[0]=p.SingleBoundJOBJ==null?ji:Math.Max(0,joints.IndexOf(p.SingleBoundJOBJ));ws[0]=1;rigid.Add(true);}
     skin.Add(js);weights.Add(ws);
    }
    var mat=d.Mobj?.Material;var tex=d.Mobj?.Textures;
    object texture=null;
    if(tex?.ImageData!=null)try{texture=new{width=tex.ImageData.Width,height=tex.ImageData.Height,rgba=Convert.ToBase64String(SwapRedBlue(tex.GetDecodedImageData())),wrapS=(int)tex.WrapS,wrapT=(int)tex.WrapT,scale=new[]{tex.SX,tex.SY},offset=new[]{tex.TX,tex.TY}};}catch(Exception e){errors.Add("Texture: "+e.Message);}
    meshes.Add(new{name="Mesh_"+meshes.Count,joint=ji,dobj=dobjs[d._s],positions,normals,texcoords,skin,weights,rigid,indices=Triangles(dl),color=mat==null?new[]{1f,1f,1f,1f}:new[]{mat.DIF_R/255f,mat.DIF_G/255f,mat.DIF_B/255f,mat.Alpha},texture,hidden=(j.Flags&JOBJ_FLAG.HIDDEN)!=0});
   }catch(Exception e){errors.Add("Joint "+ji+": "+e.Message);}
  }
  return new{bones=joints.Select((j,i)=>new{id=i,name="JOBJ_"+i,parent=parents[i],translation=new[]{j.TX,j.TY,j.TZ},rotation=new[]{j.RX,j.RY,j.RZ},scale=new[]{j.SX,j.SY,j.SZ},flags=(uint)j.Flags}),meshes,errors};
 }
 static string CompilerScript(string script) => System.Text.RegularExpressions.Regex.Replace(script,@"\b(Goto|Subroutine)\(",m=>m.Groups[1].Value=="Goto"?"Subroutine(":"GoTo(");
 static object CssTemplate(HSDRawFile file){
  var data=(SBM_SelectChrDataTable)file.Roots[0].Data;var rows=new List<object>();int count=0;
  void Visit(HSD_JOBJ j,int parent){for(;j!=null;j=j.Next){int id=count++;var images=new List<object>();for(var d=j.Dobj;d!=null;d=d.Next)for(var t=d.Mobj?.Textures;t!=null;t=t.Next)images.Add(new{w=t.ImageData?.Width,h=t.ImageData?.Height});rows.Add(new{id,parent,j.TX,j.TY,j.TZ,j.SX,j.SY,flags=(uint)j.Flags,images});Visit(j.Child,id);}}
  Visit(data.MenuModel,-1);return rows;
 }
 static void CssPortrait(HSDRawFile file,string pixelsPath,string output) {
  var data=(SBM_SelectChrDataTable)file.Roots[0].Data;
  var joints=new List<HSD_JOBJ>();void Walk(HSD_JOBJ j){for(;j!=null;j=j.Next){joints.Add(j);Walk(j.Child);}}Walk(data.MenuModel);
  // Original Falcon tile: retain Melee's geometry, UVs, render state and border.
  var source=joints[22];var next=source.Next;source.Next=null;
  var tile=new HSD_JOBJ{_s=source._s.DeepClone()};source.Next=next;
  byte[] raw=File.ReadAllBytes(pixelsPath);int w=BitConverter.ToInt32(raw,0),h=BitConverter.ToInt32(raw,4);
  if(w!=64||h!=56||raw.Length!=8+w*h*4)throw new Exception("CSS portrait must be 64 x 56 RGBA");
  for(var d=tile.Dobj;d!=null;d=d.Next)for(var t=d.Mobj?.Textures;t!=null;t=t.Next){
   if(t.ImageData?.Width!=64||t.ImageData?.Height!=56)continue;
   t.EncodeImageData(SwapRedBlue(raw[8..]),w,h,GXTexFmt.RGBA8,GXTlutFmt.RGB5A3);
  }
  var result=new HSDRawFile();result.Roots.Add(new HSDRootNode{Name="workshop_css_portrait",Data=tile});result.Save(output);
 }
 static void PatchFighter(HSDRawFile file,JsonElement patch,string output) {
  var data=file.Roots.Select(r=>r.Data).OfType<SBM_FighterData>().First();
  if(patch.TryGetProperty("attributes",out var attrs))foreach(var a in attrs.EnumerateObject()) {
   var prop=data.Attributes.GetType().GetProperty(a.Name);if(prop==null||!prop.CanWrite)throw new Exception("Unknown attribute "+a.Name);
   prop.SetValue(data.Attributes,JsonSerializer.Deserialize(a.Value.GetRawText(),prop.PropertyType,Json));
  }
  if(patch.TryGetProperty("moves",out var moves)) {
   var commands=data.FighterActionTable.Commands;
   foreach(var move in moves.EnumerateArray()) {
    int id=move.GetProperty("id").GetInt32();string script=move.GetProperty("script").GetString();
    if(id<0||id>=commands.Length||script.Length>128*1024)throw new Exception("Invalid action or oversized movescript");
    ActionCompiler.Compile(CompilerScript(script));
   }
   ActionCompiler.LinkStructs();
   foreach(var move in moves.EnumerateArray())commands[move.GetProperty("id").GetInt32()].SubAction=new SBM_FighterSubactionData{_s=ActionCompiler.GetBinary(CompilerScript(move.GetProperty("script").GetString()))};
   data.FighterActionTable.Commands=commands;
  }
  file.Save(output);
 }

 static float[] Floats(JsonElement value)=>value.EnumerateArray().Select(x=>x.GetSingle()).ToArray();
 static HSD_JOBJ Joint()=>new(){SX=1,SY=1,SZ=1,Flags=JOBJ_FLAG.ROOT_OPA|JOBJ_FLAG.ROOT_XLU};
 static void CompileStage(HSDRawFile file,JsonElement scene,string output) {
  var map=(SBM_Map_Head)file.Roots.First(r=>r.Name=="map_head").Data;
  var root=Joint();HSD_DOBJ tail=null;var generator=new POBJ_Generator{UseTriangleStrips=false,CullMode=GenCullMode.None};
  foreach(var mesh in scene.GetProperty("meshes").EnumerateArray()) {
   if(mesh.TryGetProperty("role",out var role)&&role.GetString()=="collision")continue;
   var positions=mesh.GetProperty("positions").EnumerateArray().Select(Floats).ToArray();var normals=mesh.GetProperty("normals").EnumerateArray().Select(Floats).ToArray();var uv=mesh.GetProperty("texcoords").EnumerateArray().Select(Floats).ToArray();
   var vertices=new List<GX_Vertex>();foreach(var i in mesh.GetProperty("indices").EnumerateArray().Select(v=>v.GetInt32()))vertices.Add(new GX_Vertex{POS=new GXVector3(positions[i][0],positions[i][1],positions[i][2]),NRM=new GXVector3(normals[i][0],normals[i][1],normals[i][2]),TEX0=new GXVector2(uv[i][0],uv[i][1])});
   var color=Floats(mesh.GetProperty("color"));var mat=new HSD_Material{AMB_R=255,AMB_G=255,AMB_B=255,AMB_A=255,DIF_R=(byte)(color[0]*255),DIF_G=(byte)(color[1]*255),DIF_B=(byte)(color[2]*255),DIF_A=255,Alpha=color[3]};
   var mobj=new HSD_MOBJ{Material=mat,RenderFlags=RENDER_MODE.CONSTANT|RENDER_MODE.ALPHA_MAT};
   var attrs=new List<GXAttribName>{GXAttribName.GX_VA_POS,GXAttribName.GX_VA_NRM};
   if(mesh.TryGetProperty("texture",out var tex)&&tex.ValueKind!=JsonValueKind.Null) {
    var tobj=new HSD_TOBJ{SX=1,SY=1,SZ=1,RepeatS=1,RepeatT=1,Flags=TOBJ_FLAGS.LIGHTMAP_DIFFUSE|TOBJ_FLAGS.COLORMAP_MODULATE|TOBJ_FLAGS.ALPHAMAP_MODULATE,Blending=1,MagFilter=(GXTexFilter)1,TexMapID=GXTexMapID.GX_TEXMAP0,GXTexGenSrc=GXTexGenSrc.GX_TG_TEX0,WrapS=(GXWrapMode)tex.GetProperty("wrapS").GetInt32(),WrapT=(GXWrapMode)tex.GetProperty("wrapT").GetInt32()};
    tobj.EncodeImageData(SwapRedBlue(Convert.FromBase64String(tex.GetProperty("rgba").GetString())),tex.GetProperty("width").GetInt32(),tex.GetProperty("height").GetInt32(),GXTexFmt.RGBA8,GXTlutFmt.RGB5A3);mobj.Textures=tobj;mobj.RenderFlags|=RENDER_MODE.TEX0;attrs.Add(GXAttribName.GX_VA_TEX0);
   }
   if(color[3]<1)mobj.RenderFlags|=RENDER_MODE.XLU;
   var pobj=generator.CreatePOBJsFromTriangleList(vertices,attrs.ToArray(),(List<HSD_JOBJ[]>)null,null);
   var dobj=new HSD_DOBJ{Mobj=mobj,Pobj=pobj};if(tail==null)root.Dobj=dobj;else tail.Next=dobj;tail=dobj;
  }
  generator.SaveChanges();
  root.Flags |= JOBJ_FLAG.OPA | JOBJ_FLAG.XLU;
  // Keep the template's lighting/camera groups required by Ground initialization.
  // Replace visual joints and remove animations/collision links to the old mesh.
  var groups=map.ModelGroups.Array;
  for(int i=0;i<groups.Length;i++){groups[i].RootNode=i==0?root:Joint();groups[i].JointAnimations=null;groups[i].MaterialAnimations=null;groups[i].ShapeAnimations=null;groups[i].CollisionLinks=null;groups[i].CollisionLinks2=null;}
  map.ModelGroups.Array=groups;
  var meta=scene.GetProperty("metadata");var col=(SBM_Coll_Data)file.Roots.First(r=>r.Name=="coll_data").Data;
  var cv=new List<SBM_CollVertex>();var cl=new List<SBM_CollLine>();
  foreach(var platform in meta.GetProperty("platforms").EnumerateArray()) {
   var a=Floats(platform.GetProperty("a"));var b=Floats(platform.GetProperty("b"));short vi=(short)cv.Count;
   cv.Add(new(){X=a[0],Y=a[1]});cv.Add(new(){X=b[0],Y=b[1]});
   cl.Add(new(){VertexIndex1=vi,VertexIndex2=(short)(vi+1),NextLine=-1,PreviousLine=-1,NextLineAltGroup=-1,PreviousLineAltGroup=-1,CollisionFlag=CollPhysics.Top,Flag=platform.GetProperty("dropThrough").GetBoolean()?CollProperty.DropThrough:CollProperty.LedgeGrab,Material=CollMaterial.Basic});
  }
  if(cl.Count==0||cl.Count>1000)throw new Exception("Stage needs 1 to 1000 collision platforms");
  col.Vertices=cv.ToArray();col.Links=cl.ToArray();col.TopLinksOffset=0;col.TopLinksCount=(short)cl.Count;col.BottomLinksOffset=col.RightLinksOffset=col.LeftLinksOffset=col.DynamicLinksOffset=(short)cl.Count;col.BottomLinksCount=col.RightLinksCount=col.LeftLinksCount=col.DynamicLinksCount=0;
  col.LineGroups=new[]{new SBM_CollLineGroup{TopLineIndex=0,TopLineCount=(short)cl.Count,BottomLineIndex=(short)cl.Count,RightLineIndex=(short)cl.Count,LeftLineIndex=(short)cl.Count,DynamicLineIndex=(short)cl.Count,XMin=cv.Min(v=>v.X),XMax=cv.Max(v=>v.X),YMin=cv.Min(v=>v.Y),YMax=cv.Max(v=>v.Y),VertexStart=0,VertexCount=(short)cv.Count}};
  var pointsRoot=Joint();var points=new List<SBM_GeneralPointInfo>();HSD_JOBJ last=null;
  void Point(int type,float x,float y,float z=0) {var j=Joint();j.TX=x;j.TY=y;j.TZ=z;if(last==null)pointsRoot.Child=j;else last.Next=j;last=j;points.Add(new(){JOBJIndex=(short)(points.Count+1),Type=(PointType)type});}
  var spawns=meta.GetProperty("spawns").EnumerateArray().Select(Floats).ToArray();for(int i=0;i<4;i++){var v=spawns[i%spawns.Length];Point(i,v[0],v[1]);Point(i+4,v[0],v[1]+35);}
  var bounds=Floats(meta.GetProperty("cameraBounds"));Point(149,bounds[0],bounds[3]);Point(150,bounds[1],bounds[2]);
  bounds=Floats(meta.GetProperty("blastBounds"));Point(151,bounds[0],bounds[3]);Point(152,bounds[1],bounds[2]);Point(148,0,0);Point(127,0,20);
  // General-point descriptors must reference a joint tree actually loaded by Ground.
  root.Child=pointsRoot.Child;
  map.GeneralPoints=new HSDArrayAccessor<SBM_GeneralPoints>{Array=new[]{new SBM_GeneralPoints{JOBJReference=root,Points=points.ToArray()}}};
  file.Save(output);
 }

 static object ImageInfo(HSD_Image img,HSD_Tlut tlut=null){
  if(img==null)return null;string rgba=null,error=null;
  try{var tobj=new HSD_TOBJ{ImageData=img,TlutData=tlut};rgba=Convert.ToBase64String(SwapRedBlue(tobj.GetDecodedImageData()));}catch(Exception e){error=e.Message;}
  return new{width=img.Width,height=img.Height,format=(int)img.Format,raw=Convert.ToBase64String(img.ImageData),rgba,error};
 }
 static object SymbolTextures(HSDRawFile file,string symbol,int frameLimit){
  var root=file.Roots.FirstOrDefault(r=>r.Name==symbol);if(root==null)throw new Exception("No symbol "+symbol);
  if(root.Data is HSD_JOBJ jobjRoot){
   var joints=new List<object>();int idx=0;
   void Visit(HSD_JOBJ j){for(;j!=null;j=j.Next){int mine=idx++;var textures=new List<object>();
    for(var d=j.Dobj;d!=null;d=d.Next)for(var t=d.Mobj?.Textures;t!=null;t=t.Next)textures.Add(new{texmap=(int)t.TexMapID,image=ImageInfo(t.ImageData,t.TlutData)});
    joints.Add(new{index=mine,flags=(uint)j.Flags,translation=new[]{j.TX,j.TY,j.TZ},textures});Visit(j.Child);}}
   Visit(jobjRoot);return new{kind="joint",joints};
  }
  if(root.Data is HSD_MatAnimJoint majRoot){
   var palettes=ImagePalettes(file,file.Roots.SelectMany(r=>r.Data._s.GetSubStructs()).Distinct().ToList());
   var joints=new List<object>();int idx=0;
   void Visit(HSD_MatAnimJoint m){for(;m!=null;m=m.Next){int mine=idx++;var mats=new List<object>();
    for(var ma=m.MaterialAnimation;ma!=null;ma=ma.Next){var texanims=new List<object>();
     for(var ta=ma.TextureAnimation;ta!=null;ta=ta.Next){
      var images=ta.ImageBuffers?.Array.Take(ta.ImageCount).Select(b=>ImageInfo(b.Data,b.Data!=null&&palettes.TryGetValue(b.Data._s,out var p)?p:null)).ToArray();var tracks=new List<object>();
      for(var fobj=ta.AnimationObject?.FObjDesc;fobj!=null;fobj=fobj.Next){var player=new FOBJ_Player(fobj.ToFOBJ());tracks.Add(new{type=(int)fobj.TrackType,values=Enumerable.Range(0,frameLimit).Select(f=>(int)Math.Round(player.GetValue(f))).ToArray()});}
      texanims.Add(new{texmap=(int)ta.GXTexMapID,images,tracks});
     }
     mats.Add(new{texanims});
    }
    joints.Add(new{index=mine,mats});Visit(m.Child);}}
   Visit(majRoot);return new{kind="matanim",joints};
  }
  // Some scene roots are an array of models rather than a single joint.
  if(root.Data.GetType().Name.StartsWith("HSDNullPointerArrayAccessor")){
   var models=new List<object>();int slot=0;
   var arrayProp=root.Data.GetType().GetProperty("Array");
   foreach(var raw in (System.Collections.IEnumerable)arrayProp.GetValue(root.Data)){
    var entry=raw as HSD_JOBJ;
    var joints=new List<object>();int idx=0;
    void Walk(HSD_JOBJ j){for(;j!=null;j=j.Next){int mine=idx++;var textures=new List<object>();
     for(var d=j.Dobj;d!=null;d=d.Next)for(var t=d.Mobj?.Textures;t!=null;t=t.Next)textures.Add(new{texmap=(int)t.TexMapID,image=ImageInfo(t.ImageData,t.TlutData)});
     joints.Add(new{index=mine,textures});Walk(j.Child);}}
    if(entry!=null)Walk(entry);
    models.Add(new{slot=slot++,joints});
   }
   return new{kind="models",models};
  }
  if(root.Data is HSDRaw.Melee.SIS_SdData sis){
   // The SIS font: character sheets plus the per-glyph spacing the game uses.
   var pages=new List<object>();
   foreach(var ch in sis.Images.Array){
    var st=ch._s;var refs=new List<object>();
    foreach(var kv in st.References) refs.Add(new{at=kv.Key,len=kv.Value.Length,
      head=Convert.ToBase64String(kv.Value.GetData(),0,Math.Min(64,kv.Value.Length))});
    pages.Add(new{len=st.Length,raw=Convert.ToBase64String(st.GetData(),0,Math.Min(96,st.Length)),refs});
   }
   var spacing=sis.SpacingParams.Array.Select(sp=>new{sp.TrimmedSize,sp.Before,sp.After}).ToList();
   return new{kind="sis",pages,spacing};
  }
  throw new Exception("Unsupported root type "+root.Data.GetType().Name);
 }
 public static int Main(string[] args) {
  try {
   string command=args[0];object result;
   if(command=="native-texture"){
    var patch=JsonDocument.Parse(File.ReadAllText(args[1])).RootElement;
    int width=patch.GetProperty("width").GetInt32(),height=patch.GetProperty("height").GetInt32();
    if(!((width==136&&height==188)||(width==24&&height==24)))throw new Exception("Native art must be a 136x188 portrait or 24x24 stock icon");
    byte[] rgba=Convert.FromBase64String(patch.GetProperty("rgba").GetString());
    if(rgba.Length!=width*height*4)throw new Exception("Invalid native art pixel count");
    var texture=new HSD_TOBJ();texture.EncodeImageData(SwapRedBlue(rgba),width,height,GXTexFmt.CMP,GXTlutFmt.RGB5A3);
    byte[] pixels=texture.ImageData.ImageData;
    int expected=((width+7)/8)*((height+7)/8)*32;
    if(pixels.Length!=expected)throw new Exception("Unexpected CMPR payload size");
    byte[] resultBytes=new byte[24+pixels.Length];System.Text.Encoding.ASCII.GetBytes("YAMPPTEX").CopyTo(resultBytes,0);
    System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(resultBytes.AsSpan(8),(uint)width);
    System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(resultBytes.AsSpan(12),(uint)height);
    System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(resultBytes.AsSpan(16),14);
    System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(resultBytes.AsSpan(20),(uint)pixels.Length);
    pixels.CopyTo(resultBytes,24);File.WriteAllBytes(args[2],resultBytes);return 0;
   }
   if(command=="image-archive"){
    var patch=JsonDocument.Parse(File.ReadAllText(args[1])).RootElement;
    var tobj=new HSD_TOBJ();int w=patch.GetProperty("width").GetInt32(),h=patch.GetProperty("height").GetInt32();
    tobj.EncodeImageData(SwapRedBlue(Convert.FromBase64String(patch.GetProperty("rgba").GetString())),w,h,GXTexFmt.RGBA8,GXTlutFmt.RGB5A3);
    var output=new HSDRawFile();output.Roots.Add(new HSDRootNode{ Name="custom_icon_image",Data=tobj.ImageData});output.Save(args[2]);return 0;
   }
   if(command=="compile-model"){CompileModel(JsonDocument.Parse(File.ReadAllText(args[1])).RootElement,args[2]);return 0;}
   if(command=="compile-animations"){CompileAnimations(args[1],args[2]);return 0;}
   if(command=="animation")result=Animation(args[1],int.Parse(args[2]),int.Parse(args[3]));
   else {
    // The running game holds original archives with write sharing denied.
    // Workshop only reads sources, including for edits that save a new DAT.
    var file=new HSDRawFile();
    using(var input=new FileStream(args[1],FileMode.Open,FileAccess.Read,FileShare.Read))
     file.Open(input);
    if(command=="compile-fighter") {CompileFighter(file,JsonDocument.Parse(File.ReadAllText(args[2])).RootElement,args[3]);return 0;}
    if(command=="stage") {CompileStage(file,JsonDocument.Parse(File.ReadAllText(args[2])).RootElement,args[3]);return 0;}
    else if(command=="model")result=Model(file);
    else if(command=="images")result=ExportImages(file);
    else if(command=="symbol-textures")result=SymbolTextures(file,args[2],args.Length>4?int.Parse(args[3]):8000);
    else if(command=="symbol-model"){var one=new HSDRawFile();one.Roots.Add(new HSDRootNode{Name="model",Data=file.Roots.First(r=>r.Name==args[2]).Data});result=Model(one);}
    else if(command=="patch-image"){PatchImage(file,JsonDocument.Parse(File.ReadAllText(args[2])).RootElement,args[3]);return 0;}
    else if(command=="inspect")result=Inspect(file);
    else if(command=="patch") {PatchFighter(file,JsonDocument.Parse(File.ReadAllText(args[2])).RootElement,args[3]);return 0;}
    else if(command=="menu-model"){
     var model=(HSD_JOBJ)file.Roots[0].Data.GetType().GetProperty(args[2]).GetValue(file.Roots[0].Data);
     var modelFile=new HSDRawFile();modelFile.Roots.Add(new HSDRootNode{Name="workshop_menu_joint",Data=model});modelFile.Save(args[3]);return 0;
    }
    else if(command=="stage-info"){
     var map=(SBM_Map_Head)file.Roots.First(r=>r.Name=="map_head").Data;
     result=new{groups=map.ModelGroups.Array.Select((g,i)=>new{i,fields=Props(g,0)}),points=Props(map.GeneralPoints,1)};
    }
    else if(command=="css-template")result=CssTemplate(file);
    else if(command=="css-portrait"){CssPortrait(file,args[2],args[3]);return 0;}
    else if(command=="roots")result=file.Roots.Select(r=>new{r.Name,type=r.Data.GetType().Name,fields=Props(r.Data)});
    else throw new Exception("Unknown command: "+command);
   }
   File.WriteAllText(args[^1],JsonSerializer.Serialize(result,Json));return 0;
  }catch(Exception e){Console.Error.WriteLine(e);return 1;}
 }
}
