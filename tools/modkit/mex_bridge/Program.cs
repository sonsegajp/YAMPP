using mexLib;
using mexLib.Types;
using mexLib.Utilties;
using System.Text.Json;

if(args.Length<3 || (args[0]!="inspect" && args[0]!="compose")) {
 Console.Error.WriteLine("Usage: MexContentBridge inspect <extracted-release-directory> <core-code-json>");return 2;
}
var root=Path.GetFullPath(args[1]);
var code=MexJsonSerializer.Deserialize<MexCode>(File.ReadAllBytes(args[2])) ?? throw new Exception("Missing core m-ex code");
if(!File.Exists(Path.Combine(root,"files/MxDt.dat")))throw new Exception("Verified release must be extracted first");
Directory.CreateDirectory(Path.Combine(root,"assets"));Directory.CreateDirectory(Path.Combine(root,"data"));
var project=Path.Combine(root,"akaneia-source.mexproj");
MexWorkspace workspace;
if(File.Exists(project)) {
 if(!MexWorkspace.TryOpenWorkspace(project,out var loaded,out var error,out _))throw new Exception(error);
 workspace=loaded!;
} else workspace=MexWorkspace.CreateFromMexFileSystem(project,root,code,Array.Empty<MexCode>());
var report=new {
 fighters=workspace.Project.Fighters.Select((x,i)=>new {id=i,x.Name,files=x.Files}),
 stages=workspace.Project.Stages.Select((x,i)=>new {id=i,x.Name,x.FileName}),
 music=workspace.Project.Music.Select((x,i)=>new {id=i,x.Name,x.FileName}),
 sounds=workspace.Project.SoundGroups.Count,
 optionalCodes=workspace.Project.Codes.Count,patches=workspace.Project.Patches.Count
};
File.WriteAllText(Path.Combine(root,"content-inventory.json"),JsonSerializer.Serialize(report,new JsonSerializerOptions {WriteIndented=true}));
Console.WriteLine($"Source ready: {workspace.Project.Fighters.Count} fighter records, {workspace.Project.Stages.Count} stage records, {workspace.Project.Music.Count} music entries.");
if(args[0]=="compose") {
 if(args.Length!=5)throw new ArgumentException("compose <source> <core-code-json> <base-iso> <output-directory>");
 ContentComposer.Compose(workspace,code,args[3],args[4]);
}
return 0;
