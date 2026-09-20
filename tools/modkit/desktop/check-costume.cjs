const fs=require('node:fs/promises');
const path=require('node:path');
const assert=require('node:assert/strict');
const {setTimeout:delay}=require('node:timers/promises');
async function run({win,CHECK_DIR}) {
 await fs.mkdir(CHECK_DIR,{recursive:true});win.showInactive();
 const errors=[];win.webContents.on('console-message',(_event,level,message)=>{if(level>=3)errors.push(message)});
 const js=code=>win.webContents.executeJavaScript(code);
 const wait=async(code)=>{const until=Date.now()+150000;while(!await js(code)){if(Date.now()>until)throw new Error(await js("document.querySelector('#status').textContent"));await delay(150)}};
 await wait("document.querySelector('#status').textContent==='Captain Falcon loaded'");
 await js("document.querySelector('[data-kind=costume]').click();document.querySelector('[data-id=\"fierce-deity-link\"]').click()");
 await wait("document.querySelector('#status').textContent==='Fierce Deity Link loaded'");
 await wait("document.querySelectorAll('.texture-card').length>10");
 await js("document.querySelector('.details').style.display='none';document.querySelector('.viewport-wrap').style.flex='1';document.querySelector('.viewport-wrap').style.height='600px';document.querySelector('#show-hit').checked=false;document.querySelector('#show-hit').dispatchEvent(new Event('change'))");
 await delay(500);
 await js("document.querySelector('[data-move=\"3\"]').click()");await delay(700);
 const count=await js("document.querySelector('#costume').options.length");
 for(let i=0;i<count;i++){
  {await js(`document.querySelector('#costume').selectedIndex=${i};document.querySelector('#costume').dispatchEvent(new Event('change'))`);await delay(3500)}
  await js("document.querySelector('#reset-view').click()");
  await delay(250);
  await js("document.querySelector('#front-view').click()");
  await delay(1800);
  await fs.writeFile(path.join(CHECK_DIR,`costume-${i}.png`),(await win.webContents.capturePage()).toPNG());
  const rect=await js("(()=>{const r=document.querySelector('#viewport').getBoundingClientRect();return {x:Math.round(r.x),y:Math.round(r.y),width:Math.round(r.width),height:Math.round(r.height)}})()");
  await fs.writeFile(path.join(CHECK_DIR,`preview-${i}.png`),(await win.webContents.capturePage(rect)).toPNG());
 }
 if(process.env.MELEE_WORKSHOP_PUBLIC_CHECK){
  await js("document.querySelector('#community-open').click()");
  await wait("document.querySelector('#community-mods').textContent.includes('Fierce Deity Link')");
  await delay(800);await fs.writeFile(path.join(CHECK_DIR,'live-mod-browser.png'),(await win.webContents.capturePage()).toPNG());
  const listed=await js("window.workshop.request('/api/community/catalog')");assert(listed.mods.some(m=>m.sha256==='a02fc8575b63e5fc2b97e05f1a9f28737dbc030e66bab84b8043b7ffd082498b'&&m.installed));
 }
 const report={costumes:count,errors,status:await js("document.querySelector('#status').textContent"),viewport:await js("({...document.querySelector('#viewport').dataset})")};
 await fs.writeFile(path.join(CHECK_DIR,'report.json'),JSON.stringify(report,null,2));assert.equal(errors.length,0);
}
module.exports={run};
