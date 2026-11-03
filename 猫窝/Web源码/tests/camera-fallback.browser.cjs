const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const fs=require('node:fs/promises'),path=require('node:path'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'..');
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  for(const scenario of ['stream-failure','status-failure']){
   const page=await browser.newPage();
   const jpeg=Buffer.from(await page.evaluate(()=>{const c=document.createElement('canvas');c.width=16;c.height=16;const x=c.getContext('2d');x.fillStyle='green';x.fillRect(0,0,16,16);return c.toDataURL('image/jpeg').split(',')[1];}),'base64');
   let streams=0,shots=0,statuses=0,rejectShots=false;
   const errors=[];page.on('pageerror',e=>errors.push(e.message));
   await page.route('**/*',async route=>{
    const url=new URL(route.request().url()),remote=url.hostname==='192.168.0.107';
    const json=data=>route.fulfill({contentType:'application/json',headers:{'Access-Control-Allow-Origin':'*'},body:JSON.stringify(data)});
    if(url.pathname==='/api/device')return json({product:'cat-home',role:'main',id:'1c:db:d4:75:10:ec',discoveryVersion:1,ip:'192.168.0.104',peer:{id:'88:f1:55:67:97:d0',role:'camera',online:true,ip:'192.168.0.107'}});
    if(url.pathname==='/api/camera/status'){
     if(remote){statuses++;if(scenario==='status-failure')return route.abort('connectionreset');}
     return json({ready:true,stream:true});
    }
    if(url.pathname==='/api/camera/stream'){if(remote)streams++;return route.abort('connectionreset');}
    if(url.pathname==='/api/camera/capture'){
     assert(remote);shots++;if(rejectShots)return route.abort('connectionreset');
     return route.fulfill({contentType:'image/jpeg',body:jpeg,headers:{'Access-Control-Allow-Origin':'*','X-Camera-Transport':'wifi','Access-Control-Expose-Headers':'X-Camera-Transport'}});
    }
    if(url.pathname.startsWith('/api/'))return json({});
    if(url.pathname==='/app.js')return route.fulfill({contentType:'text/javascript',body:''});
    const file=url.pathname==='/'?'index.html':url.pathname.slice(1);
    if(!/^[a-z-]+\.(html|css|js)$/.test(file))return route.abort();
    return route.fulfill({body:await fs.readFile(path.join(root,file)),contentType:file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html'});
   });
   await page.goto('http://cat-test.local/');
   await page.evaluate(async()=>{
    await (await import('/device-network.js')).refreshDevices();
    (await import('/camera-view.js')).initializeCameraView({toast:()=>{},addActivity:()=>{}});
   });
   await page.locator('#camera-select-2').click();
   await page.waitForFunction(()=>{const i=document.querySelector('#camera-image');return !i.hidden&&i.naturalWidth===16;},{},{timeout:20000});
   assert(shots>0);
   if(scenario==='stream-failure')assert.equal(streams,2);else assert.equal(statuses,2);
   rejectShots=true;
   await page.waitForFunction(()=>document.querySelector('#camera-mode').textContent==='画面未更新');
   assert.equal(await page.locator('#camera-image').isVisible(),true);
   assert.match(await page.locator('#camera-connection').innerText(),/上一帧/);
   await page.locator('#camera-toggle').click();
   const count=shots;await page.waitForTimeout(1000);assert.equal(shots,count);
   assert.deepEqual(errors,[]);console.log('PASS '+scenario+' with stale-frame label and pause');
   await page.close();
  }
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
