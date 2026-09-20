using mexLib;
using mexLib.Types;
using mexLib.Utilties;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

static class ContentComposer {
 static T Clone<T>(T item)=>MexJsonSerializer.Deserialize<T>(Encoding.UTF8.GetBytes(MexJsonSerializer.Serialize(item)))!;
 public static void Compose(MexWorkspace source,MexCode core,string iso,string directory) {
  var root=Path.GetFullPath(directory);var boundary=Path.GetFullPath("build/akaneia-content")+Path.DirectorySeparatorChar;
  if(!root.StartsWith(boundary,StringComparison.OrdinalIgnoreCase)||Directory.Exists(root))throw new Exception("Use a new directory within build/akaneia-content");
  using(var stream=File.OpenRead(iso))if(Convert.ToHexString(MD5.HashData(stream)).ToLowerInvariant()!="0e63d4223b01d9aba596259dc155a174")throw new Exception("A verified Melee 1.02 base is required");
  Directory.CreateDirectory(root);
  var target=MexWorkspace.NewWorkspace(Path.Combine(root,"yampp-content.mexproj"),iso,core,Array.Empty<MexCode>());
  // Keep all non-content configuration derived from the original Melee disc.
  // Never import source.Build, SceneData, Codes, Patches, ReservedAssets,
  // MenuPlaylist, or any stock fighter/stage replacement.
  target.Project.StartingScene=1; // Main menu, whose content remains owned by YAMPP.
  if(target.Project.Fighters.Count!=33 || target.Project.Stages.Count!=71 || target.Project.Music.Count!=98)
   throw new Exception("Unexpected base content layout");
  var baseStages=target.Project.Stages.Select(MexJsonSerializer.Serialize).ToArray();
  var baseMusic=target.Project.Music.Select(MexJsonSerializer.Serialize).ToArray();
  var preservedMenus=new Dictionary<string,string>();
  foreach(var name in new[]{"MnMaAll.usd","MnMaAll.dat","SdMenu.usd","SdMenu.dat"}) {
   var bytes=target.FileManager.Get(target.GetFilePath(name));
   if(bytes!=null)preservedMenus[name]=Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
  }
  var baseScene=MexJsonSerializer.Serialize(target.Project.SceneData);
  var baseMenu=MexJsonSerializer.Serialize(target.Project.MenuPlaylist);
  var baseFighters=target.Project.Fighters.Take(27).Select(MexJsonSerializer.Serialize).ToArray();
  var importedFiles=new List<string>();
  string CopyFile(string file) {
   if(string.IsNullOrEmpty(file))return file;
   var normalized=file.Replace('\\','/').TrimStart('/');
   if(normalized.Split('/').Any(x=>x is ".." or ".")||normalized.Contains(':'))throw new Exception("Invalid upstream asset path");
   var data=source.FileManager.Get(source.GetFilePath(normalized));
   if(data==null)throw new Exception("Missing upstream asset: "+normalized);
   var dest=target.FileManager.GetUniqueFilePath(target.GetFilePath(normalized));target.FileManager.Set(dest,data);
   importedFiles.Add(Path.GetRelativePath(target.GetFilePath(""),dest).Replace('\\','/'));
   return Path.GetRelativePath(target.GetFilePath(""),dest).Replace('\\','/');
  }
  var music=new Dictionary<int,int>();var addedMusic=new List<object>();
  for(int i=0;i<98;i++)music[i]=i;
  for(int i=98;i<source.Project.Music.Count;i++) {
   var track=Clone(source.Project.Music[i]);track.FileName=CopyFile("audio/"+track.FileName)[6..];
   music[i]=target.Project.Music.Count;target.Project.Music.Add(track);
   addedMusic.Add(new{sourceID=i,targetID=music[i],track.Name,track.FileName,
    sha256=Convert.ToHexString(SHA256.HashData(target.FileManager.Get(target.GetFilePath("audio/"+track.FileName)))).ToLowerInvariant()});
  }
  int Track(int id)=>music.TryGetValue(id,out var mapped)?mapped:throw new Exception("Unknown source music ID: "+id);
  var sounds=new Dictionary<int,int>();
  int Sound(int id) {
   if(id==55)return 55;if(sounds.TryGetValue(id,out var mapped))return mapped;
   using var package=new MemoryStream();MexSoundGroup.ToPackage(source.Project.SoundGroups[id],package);package.Position=0;
   var error=MexSoundGroup.FromPackage(target,package,out var group);if(error!=null||group==null)throw new Exception(error?.Message??"Missing sound group");
   return sounds[id]=target.Project.AddSoundGroup(group);
  }
  var series=new Dictionary<int,int>();
  int Series(int id) {
   if(series.TryGetValue(id,out var mapped))return mapped;
   var from=source.Project.Series[id];
   for(int n=0;n<target.Project.Series.Count;n++)if(target.Project.Series[n].Name==from.Name)return series[id]=n;
   var value=new MexSeries{Name=from.Name};
   var icon=from.IconAsset.GetTexFile(source);if(icon!=null)value.IconAsset.SetFromMexImage(target,icon);
   var sss=from.StageSelectIconAsset.GetTexFile(source);if(sss!=null)value.StageSelectIconAsset.SetFromMexImage(target,sss);
   var model=from.ModelAsset.GetOBJFile(source);if(model!=null)value.ModelAsset.SetFromObjFile(target,model);
   mapped=target.Project.Series.Count;target.Project.Series.Add(value);return series[id]=mapped;
  }
  // m-ex's extracted metadata does not enumerate every file opened by a
  // stage module (e.g. Boxing Ring titles and Village scenery/visitors).
  // Follow only stage-owned archive/table references, preserving their exact
  // names: the stage code opens those names directly at runtime.
  var stageFiles=new SortedDictionary<string,string>(StringComparer.Ordinal);
  void IncludeStageFiles(MexStage from,MexStage stage) {
   var first=(from.FileName??throw new Exception("Missing stage file")).TrimStart('/');
   var queue=new Queue<string>();queue.Enqueue(first);
   foreach(var name in from.AdditionalFiles)queue.Enqueue(name.TrimStart('/'));
   var visited=new HashSet<string>(StringComparer.Ordinal);
   while(queue.Count>0) {
    var name=queue.Dequeue();if(!visited.Add(name))continue;
    if(name.Contains('/')||name.Contains('\\')||name.Contains(':')||name.Contains(".."))throw new Exception("Invalid stage dependency: "+name);
    var sourcePath=source.GetFilePath(name);
    if(!source.FileManager.Exists(sourcePath))throw new Exception("Missing stage dependency: "+name);
    var bytes=source.FileManager.Get(sourcePath);
    if(name!=first) {
     var path=target.GetFilePath(name);var exists=target.FileManager.Exists(path);var existing=target.FileManager.Get(path);
     if(exists&&!existing.SequenceEqual(bytes))throw new Exception("Stage dependency conflicts with base content: "+name);
     if(!exists){target.FileManager.Set(path,bytes);importedFiles.Add(name);}
     if(!stage.AdditionalFiles.Contains(name))stage.AdditionalFiles.Add(name);
    }
    stageFiles[name]=Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    foreach(Match match in Regex.Matches(Encoding.Latin1.GetString(bytes),@"(?<![A-Za-z0-9_])Gr[A-Za-z0-9_]+\.(?:dat|usd|csv)(?=\x00)"))queue.Enqueue(match.Value);
   }
  }
  var stageIDs=new Dictionary<int,int>();var addedStages=new List<object>();var versus=new List<(int Id,MexStage Stage)>();
  for(int i=71;i<source.Project.Stages.Count;i++) {
   var from=source.Project.Stages[i];
   if(from.Name=="Volleyball")continue; // A separate rules/mode module, outside this content-only import.
   using var package=new MemoryStream();MexStage.ToPackage(package,source,from,new(){ExportFiles=true,ExportSound=false});package.Position=0;
   var error=MexStage.FromPackage(package,target,out var stage);if(error!=null||stage==null)throw new Exception(error?.Message??"Missing stage");
   IncludeStageFiles(from,stage);
   stage.SeriesID=Series(from.SeriesID);stage.SoundBank=Sound(from.SoundBank);stage.Playlist=Clone(from.Playlist);
   foreach(var entry in stage.Playlist.Entries)entry.MusicID=Track(entry.MusicID);
   var id=target.Project.AddStage(stage);stageIDs[MexStageIDConverter.ToExternalID(i)]=MexStageIDConverter.ToExternalID(id);
   addedStages.Add(new {sourceID=i,targetID=id,stage.Name,stage.FileName});
   if(!from.Name.StartsWith("Targets!"))versus.Add((MexStageIDConverter.ToExternalID(id),stage));
  }
  var names=new[]{"Wolf","Diddy Kong","Charizard","Lucas","Sonic","King Dedede","Tails"};var addedFighters=new List<object>();
  foreach(var name in names) {
   var from=source.Project.Fighters.Single(f=>f.Name==name);
   using var package=new MemoryStream();from.ToPackage(source,package,new(){ExportFiles=true,ExportSoundBank=false,ExportCostumes=true,ExportMedia=false});package.Position=0;
   var packageBytes=package.ToArray();
   var error=MexFighter.FromPackage(target,package,out var fighter);if(error!=null||fighter==null)throw new Exception(error?.Message??"Missing fighter");
   using(var filePackage=new MemoryStream(packageBytes))using(var zip=new ZipArchive(filePackage,ZipArchiveMode.Read,true)){var fe=fighter.Files.FromPackage(target,zip);if(fe!=null)throw new Exception(fe.Message);}
   // DemoWait is a joint symbol, despite the upstream package helper treating
   // it as a file name. Preserve it for the fighter's native demo animation.
   fighter.Files.DemoWait=from.Files.DemoWait;
   if(!string.IsNullOrEmpty(from.Files.KirbyCapFileName))fighter.Files.KirbyCapFileName=CopyFile(from.Files.KirbyCapFileName);
   fighter.SeriesID=Series(from.SeriesID);fighter.SoundBank=Sound(from.SoundBank);
   fighter.AnnouncerCall=Sound(from.AnnouncerCall/10000)*10000+from.AnnouncerCall%10000;
   fighter.FighterMusic1=Track(from.FighterMusic1);fighter.FighterMusic2=Track(from.FighterMusic2);fighter.VictoryTheme=Track(from.VictoryTheme);
   if(stageIDs.TryGetValue(from.TargetTestStage,out var targetStage))fighter.TargetTestStage=targetStage;
   target.Project.AddNewFighter(fighter);
   addedFighters.Add(new {fighter.Name,internalID=target.Project.Fighters.IndexOf(fighter),fighter.SoundBank});
  }
  for(int i=27;i<34;i++)target.Project.CharacterSelect.FighterIcons.Add(new(){Fighter=MexFighterIDConverter.ToExternalID(i,target.Project.Fighters.Count),SFXID=0});
  var layout=target.Project.CharacterSelect.Template;layout.IconsPerRow=11;layout.ScaleX=.85f;layout.ScaleY=.85f;layout.CenterY=9.2f;layout.Apply(target.Project.CharacterSelect.FighterIcons);
  var page=new MexStageSelect{Name="Akaneia"};
  for(int i=0;i<versus.Count;i++)page.StageIcons.Add(new(){StageID=versus[i].Id,X=-20+(i%5)*10,Y=14-(i/5)*8,Z=0,ScaleX=1,ScaleY=1,Width=3.1f,Height=2.7f,Status=MexStageSelectIcon.StageIconStatus.Unlocked});
  target.Project.StageSelects.Add(page);
  if(target.Project.Codes.Count!=0||target.Project.Patches.Count!=0||MexJsonSerializer.Serialize(target.Project.SceneData)!=baseScene||MexJsonSerializer.Serialize(target.Project.MenuPlaylist)!=baseMenu)throw new Exception("Non-content project configuration changed");
  for(int i=0;i<27;i++)if(MexJsonSerializer.Serialize(target.Project.Fighters[i])!=baseFighters[i])throw new Exception("Base fighter changed: "+i);
  for(int i=0;i<71;i++)if(MexJsonSerializer.Serialize(target.Project.Stages[i])!=baseStages[i])throw new Exception("Base stage changed: "+i);
  for(int i=0;i<98;i++)if(MexJsonSerializer.Serialize(target.Project.Music[i])!=baseMusic[i])throw new Exception("Base music changed: "+i);
  using(var writer=new StreamWriter(Path.Combine(root,"compile.log"))){writer.AutoFlush=true;target.Save(writer);}
  foreach(var entry in preservedMenus) {
   var bytes=File.ReadAllBytes(target.GetFilePath(entry.Key));
   if(Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant()!=entry.Value)throw new Exception("Menu asset changed: "+entry.Key);
  }
  var audit=new {stageFileRevision=1,stageFiles,scope=new[]{"fighters","stages","music"},fighters=addedFighters,stages=addedStages,musicTracks=music.Count-98,music=addedMusic,menuAssets=preservedMenus,importedFiles,optionalCodes=0,optionalPatches=0,baseMenusPreserved=true,baseScenesPreserved=true};
  File.WriteAllText(Path.Combine(root,"content-audit.json"),JsonSerializer.Serialize(audit,new JsonSerializerOptions{WriteIndented=true}));
  target.ExportISO(Path.Combine(root,"yampp-content.iso"),(sender,args)=>{});
  Console.WriteLine($"Built content-only import: {names.Length} fighters, {versus.Count} versus stages, {music.Count-98} additional music tracks.");
 }
}
