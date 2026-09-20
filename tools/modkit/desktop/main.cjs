const {app, BrowserWindow, Menu, dialog, ipcMain, protocol, shell} = require('electron');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const {spawn} = require('node:child_process');
const readline = require('node:readline');
const {createHash} = require('node:crypto');

function findRoot() {
  let candidate = __dirname;
  while (!fs.existsSync(path.join(candidate, 'tools/modkit/catalog.py'))) {
    const parent = path.dirname(candidate);
    if (parent === candidate) throw new Error('Keep Melee Workshop inside the YAMPP project.');
    candidate = parent;
  }
  return candidate;
}
const ROOT = findRoot();
const COMMUNITY_CHECK = process.argv.includes('--workshop-community-check');
const COSTUME_CHECK = process.argv.includes('--workshop-costume-check');
const CHECK = COSTUME_CHECK || COMMUNITY_CHECK || process.argv.includes('--workshop-check');
const UI = fs.existsSync(path.join(__dirname, 'ui')) ? path.join(__dirname, 'ui') : path.join(__dirname, '../web');
const CHECK_DIR = COSTUME_CHECK ? path.join(ROOT,'build/modkit/electron-costume-check') : COMMUNITY_CHECK && process.env.MELEE_WORKSHOP_COMMUNITY_CHECK_DIR ? path.resolve(process.env.MELEE_WORKSHOP_COMMUNITY_CHECK_DIR) : path.join(ROOT, COMMUNITY_CHECK ? 'build/modkit/electron-community-check' : 'build/modkit/electron-check');
if (CHECK && !within(path.join(ROOT,'build/modkit'),CHECK_DIR)) throw new Error('Invalid Workshop check directory');
const ORIGIN = 'workshop://app';
const importMap = fs.readFileSync(path.join(UI, 'index.html'), 'utf8').match(/<script type="importmap">(.*?)<\/script>/s)[1];
const CSP = `default-src 'self'; script-src 'self' 'sha256-${createHash('sha256').update(importMap).digest('base64')}'; style-src 'self' 'unsafe-inline'; img-src 'self' blob: data:; connect-src 'self' blob: data:; object-src 'none'; frame-src 'none'; base-uri 'none'; form-action 'none'`;
app.setName('Melee Workshop');
app.setPath('userData', path.join(ROOT, 'user/workshop', CHECK ? 'check-profile' : 'desktop-profile'));
protocol.registerSchemesAsPrivileged([{scheme:'workshop', privileges:{standard:true, secure:true, supportFetchAPI:true, corsEnabled:true}}]);

function within(root, file) {
  const relative = path.relative(root, file);
  return !path.isAbsolute(relative) && relative !== '..' && !relative.startsWith('..' + path.sep);
}
function trusted(url) {
  const parsed = new URL(url);
  return parsed.protocol === 'workshop:' && parsed.hostname === 'app' && !parsed.port && !parsed.username;
}
class Worker {
  constructor() {
    this.pending = new Map(); this.sequence = 0; this.stderr = ''; this.closed = false;
    const bundledPython = path.join(ROOT, 'tools/python/python.exe');
    const installedPython = path.join(process.env.LOCALAPPDATA || '', 'Programs/Python/Python310/python.exe');
    const python = process.env.MELEE_WORKSHOP_PYTHON || (fs.existsSync(bundledPython) ? bundledPython : fs.existsSync(installedPython) ? installedPython : 'python');
    this.child = spawn(python, ['-u', path.join(ROOT, 'tools/modkit/desktop_worker.py')], {
      cwd: ROOT, windowsHide:true, stdio:['pipe','pipe','pipe'], env:{...process.env, PYTHONIOENCODING:'utf-8', ...(CHECK ? {MELEE_WORKSHOP_TEST_MODS:path.join(CHECK_DIR,'mods')} : {})}
    });
    readline.createInterface({input:this.child.stdout}).on('line', line => {
      try {
        const message = JSON.parse(line), item = this.pending.get(message.id);
        if (!item) return;
        clearTimeout(item.timer); this.pending.delete(message.id);
        message.error ? item.reject(new Error(message.error)) : item.resolve(message.result);
      } catch (error) { this.fail(error); }
    });
    this.child.stderr.on('data', bytes => {this.stderr = (this.stderr + bytes.toString()).slice(-6000);});
    this.child.on('error', error => this.fail(error));
    this.child.on('exit', code => {this.closed = true; this.fail(new Error('Asset worker exited ('+code+'). '+this.stderr));});
    this.child.stdin.on('error', error => this.fail(error));
  }
  fail(error) {
    for (const item of this.pending.values()) {clearTimeout(item.timer); item.reject(error);}
    this.pending.clear();
  }
  call(op, request = {}) {
    if (this.closed) return Promise.reject(new Error('The asset worker has closed. Reopen Melee Workshop.'));
    return new Promise((resolve, reject) => {
      const id = ++this.sequence;
      const timer = setTimeout(() => {this.pending.delete(id); reject(new Error('Asset conversion timed out.'));}, 15*60*1000);
      this.pending.set(id, {resolve,reject,timer});
      this.child.stdin.write(JSON.stringify({id,op,...request})+'\n', 'utf8', error => {if(error)this.fail(error);});
    });
  }
  stop() {this.closed=true; this.fail(new Error('Workshop is closing.')); this.child.kill();}
}
let win, worker;
const READ = new Set(['/api/lua','/api/catalog','/api/fighter','/api/animation','/api/stage','/api/textures','/api/community/catalog','/api/community/packages','/api/community/job']);
const WRITE = new Set(['/api/lua/validate','/api/lua/save','/api/lua/launch','/api/clone','/api/fighter/save','/api/stage/save','/api/package/enabled','/api/costume/clone','/api/community/prepare','/api/community/upload','/api/community/install']);
function localPath(raw) {
  if (typeof raw !== 'string' || !raw.startsWith('/') || raw.startsWith('//')) throw new Error('Invalid asset request');
  const parsed = new URL(raw, ORIGIN);
  if (!trusted(parsed.href)) throw new Error('Invalid asset origin');
  return parsed;
}
function assetExport(raw) {
  const url = localPath(raw);
  return url.pathname === '/api/export' || url.pathname === '/api/texture' || url.pathname === '/download/blender-addon.py' || /^\/mods\/[a-z0-9-]+\/stage\.glb$/.test(url.pathname);
}
function senderCheck(event) {
  if (event.sender !== win.webContents || event.senderFrame !== win.webContents.mainFrame || !trusted(event.senderFrame.url)) throw new Error('Unknown editor frame');
}
async function exportAsset(raw, suggested) {
  if (!assetExport(raw)) throw new Error('Unknown export format');
  const addon = raw.startsWith('/download/'), texture=raw.startsWith('/api/texture');
  const target = await dialog.showSaveDialog(win, {title:addon?'Save Blender addon':texture?'Export PNG texture':'Export Blender scene', defaultPath:path.basename(String(suggested)), filters:[{name:addon?'Blender Python addon':texture?'PNG texture':'glTF binary scene',extensions:[addon?'py':texture?'png':'glb']} ]});
  if (target.canceled || !target.filePath) return {canceled:true};
  const result = await worker.call('read', {path:raw});
  if (!result.assetFile || !within(ROOT, result.assetFile)) throw new Error('Invalid export source');
  await fsp.copyFile(result.assetFile, target.filePath);
  return {canceled:false, path:target.filePath};
}
async function importStage() {
  const selection = await dialog.showOpenDialog(win, {title:'Import Blender stage',properties:['openFile'],filters:[{name:'Blender glTF scene',extensions:['glb','gltf']}]});
  if (selection.canceled || !selection.filePaths.length) return {canceled:true};
  const primary = await fsp.realpath(selection.filePaths[0]);
  if (!/\.(glb|gltf)$/i.test(primary)) throw new Error('Choose a GLB or glTF file.');
  const folder = path.dirname(primary), files = new Map([[path.basename(primary), primary]]);
  if (path.extname(primary).toLowerCase() === '.gltf') {
    const gltf = JSON.parse(await fsp.readFile(primary, 'utf8'));
    for (const item of [...(gltf.buffers || []), ...(gltf.images || [])]) {
      if (!item.uri || item.uri.startsWith('data:')) continue;
      const relative = decodeURIComponent(item.uri);
      if (/^[a-z]+:/i.test(relative) || path.isAbsolute(relative)) throw new Error('glTF companion files must be local to the scene folder.');
      const companion = await fsp.realpath(path.resolve(folder, relative));
      if (!within(folder, companion)) throw new Error('A glTF companion points outside the scene folder.');
      files.set(relative.replaceAll('\\','/'), companion);
    }
  }
  let total = 0; const payload = [];
  for (const [name, filename] of files) {
    const stat = await fsp.stat(filename); total += stat.size;
    if (total > 210*1024*1024) throw new Error('Stage source files exceed 210 MiB.');
    payload.push({name, data:(await fsp.readFile(filename)).toString('base64')});
  }
  return worker.call('mutate', {path:'/api/stage/import',data:{name:path.basename(primary,path.extname(primary)).slice(0,48),primary:path.basename(primary),files:payload}});
}
function command(name) {win.webContents.send('workshop:command', name);}
function installMenu() {
  Menu.setApplicationMenu(Menu.buildFromTemplate([
    {label:'File',submenu:[
      {label:'Import Blender stage...',accelerator:'CmdOrCtrl+O',click:()=>command('import')},
      {label:'Export GLB...',accelerator:'CmdOrCtrl+E',click:()=>command('export')},
      {label:'Save package',accelerator:'CmdOrCtrl+S',click:()=>command('save')},
      {type:'separator'}, {label:'Open mods folder',click:()=>shell.openPath(path.join(ROOT,'user/mods'))},
      {type:'separator'}, {role:'quit'}]},
    {label:'Edit',submenu:[{role:'undo'},{role:'redo'},{type:'separator'},{role:'cut'},{role:'copy'},{role:'paste'},{role:'selectAll'},{type:'separator'},{label:'Clone fighter...',click:()=>command('clone')}]},
    {label:'View',submenu:[{role:'resetZoom'},{role:'zoomIn'},{role:'zoomOut'},{role:'togglefullscreen'}]},
    {label:'Tools',submenu:[{label:'Save Blender addon...',click:()=>command('blender')}]},
    {label:'Help',submenu:[{label:'About Melee Workshop',click:()=>dialog.showMessageBox(win,{type:'info',title:'Melee Workshop',message:'Melee Workshop',detail:'Desktop character and stage editor for Yet Another Melee PC Port (YAMPP).\nGLB / glTF import and Blender export.\nVersion '+app.getVersion()})}]}
  ]));
}
async function startup() {
  worker = new Worker();
  await worker.call('ping');
  protocol.handle('workshop', async request => {
    try {
      if (!trusted(request.url) || request.method !== 'GET') return new Response('Blocked', {status:403});
      const url = new URL(request.url); let filename;
      if (url.pathname.startsWith('/api/') || url.pathname.startsWith('/mods/')) {
        const result = await worker.call('read', {path:url.pathname+url.search});
        if (!result.assetFile || !within(ROOT, result.assetFile)) return Response.json(result);
        filename = result.assetFile;
      } else {
        filename = path.resolve(UI, '.' + decodeURIComponent(url.pathname === '/' ? '/index.html' : url.pathname));
        if (!within(UI, filename)) return new Response('Blocked', {status:403});
      }
      const mime = {'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css; charset=utf-8','.glb':'model/gltf-binary','.json':'application/json','.png':'image/png','.svg':'image/svg+xml'}[path.extname(filename)] || 'application/octet-stream';
      return new Response(await fsp.readFile(filename), {headers:{'Content-Type':mime,'Content-Security-Policy':CSP,'X-Content-Type-Options':'nosniff'}});
    } catch(error) {return Response.json({error:error.message},{status:400});}
  });
  win = new BrowserWindow({width:1480,height:980,minWidth:1100,minHeight:740,show:false,title:'Melee Workshop',backgroundColor:'#111923',
    webPreferences:{backgroundThrottling:!CHECK,preload:path.join(__dirname,'preload.cjs'),contextIsolation:true,nodeIntegration:false,sandbox:true}});
  win.webContents.setWindowOpenHandler(()=>({action:'deny'}));
  win.webContents.on('will-navigate', (event, url)=>{if(!trusted(url))event.preventDefault();});
  win.webContents.session.setPermissionRequestHandler((_contents,_permission,callback)=>callback(false));
  win.webContents.session.setPermissionCheckHandler(()=>false);
  ipcMain.handle('workshop:request', (event, raw, body) => {
    senderCheck(event); const url=localPath(raw);
    if (!(body === undefined ? READ : WRITE).has(url.pathname)) throw new Error('Unknown editor operation');
    return worker.call(body === undefined ? 'read':'mutate', {path:raw,data:body});
  });
  ipcMain.handle('workshop:import-texture',async(event,options)=>{
    senderCheck(event);
    if(!options||!['texture','css','stock'].includes(options.kind))throw new Error('Unknown texture import');
    const selection=await dialog.showOpenDialog(win,{title:'Import PNG texture',properties:['openFile'],filters:[{name:'PNG image',extensions:['png']}]});
    if(selection.canceled||!selection.filePaths.length)return {canceled:true};
    const filename=selection.filePaths[0];if(path.extname(filename).toLowerCase()!=='.png'||(await fsp.stat(filename)).size>16*1024*1024)throw new Error('Choose a PNG below 16 MiB.');
    return worker.call('mutate',{path:'/api/texture/import',data:{id:options.id,costume:options.costume,image:options.image,kind:options.kind,data:(await fsp.readFile(filename)).toString('base64')}});
  });
  ipcMain.handle('workshop:import-stage',event=>{senderCheck(event);return importStage();});
  ipcMain.handle('workshop:export',(event,raw,name)=>{senderCheck(event);return exportAsset(raw,name);});
  installMenu();
  if (!CHECK) win.once('ready-to-show',()=>{win.show();win.focus();});
  await win.loadURL(ORIGIN+'/');
  if (CHECK) {
    try {await require(COSTUME_CHECK ? './check-costume.cjs' : COMMUNITY_CHECK ? './check-community.cjs' : './check.cjs').run({app,win,worker,dialog,ROOT,CHECK_DIR,exportAsset}); app.quit();}
    catch(error) {await fsp.mkdir(CHECK_DIR,{recursive:true});await fsp.writeFile(path.join(CHECK_DIR,'failure.txt'),error.stack);console.error(error);worker?.stop();app.exit(1);}
  }
}
const hasLock = app.requestSingleInstanceLock();
if (!hasLock) app.quit();
else {
  app.on('second-instance',()=>{if(win){if(win.isMinimized())win.restore();win.show();win.focus();}});
  app.whenReady().then(startup).catch(error=>{if(CHECK)console.error(error);else dialog.showErrorBox('Melee Workshop could not start',error.message);worker?.stop();app.exit(1);});
  app.on('window-all-closed',()=>app.quit());
  app.on('before-quit',()=>worker?.stop());
  app.on('will-quit',()=>worker?.stop());
}
