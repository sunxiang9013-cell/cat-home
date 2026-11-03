// Offline: intercept every request, never change a real board's network.
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const fs=require('node:fs/promises'),path=require('node:path'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'..');
const mainId='1c:db:d4:75:10:ec',cameraId='88:f1:55:67:97:d0';
(async()=>{
 const browser=await chromium.launch({channel:'msedge',headless:true});
 try{
  for(const scenario of ['success','lost-reply','prepare-failure','peer-missing','wrong-identity','changed-ip']){
   const page=await browser.newPage({viewport:{width:390,height:844}});
   const calls=[],states=[{},{}],errors=[];
   let peerIp='192.168.0.107';
   page.on('pageerror',e=>errors.push(e.message));
   await page.route('**/*',async route=>{
    const req=route.request(),url=new URL(req.url()),second=url.hostname!=='cat-test.local',index=second?1:0;
    const json=(data,status=200)=>route.fulfill({status,contentType:'application/json',headers:{'Access-Control-Allow-Origin':'*'},body:JSON.stringify(data)});
    if(url.pathname==='/api/device')return json({product:'cat-home',role:'main',id:mainId,discoveryVersion:1,ip:'192.168.0.104',peer:{id:cameraId,role:'camera',online:scenario!=='peer-missing',ip:peerIp}});
    if(url.pathname==='/api/wifi'){
     const state=()=>({connected:true,ssid:states[index].phase==='connected'?'NewHome':'Current',ip:second?peerIp:'192.168.0.104',deviceId:scenario==='wrong-identity'&&second?'wrong':second?cameraId:mainId,bootId:'boot-'+index,phase:'idle',...states[index]});
     if(req.method()==='GET')return json(state());
     const body=req.postDataJSON();calls.push({index,action:body.action,id:body.requestId,boot:body.bootId});
     assert.equal(body.bootId,'boot-'+index);
     if(body.action==='prepare'){
      if(scenario==='prepare-failure'&&second)return json({error:'scan_busy'},409);
      assert.equal(body.ssid,'NewHome');assert.equal(body.password,'test-password');
      states[index]={phase:'prepared',requestId:body.requestId};return json(state());
     }
     if(body.action==='cancel'){states[index]={phase:'cancelled',requestId:body.requestId};return json(state());}
     if(body.action==='commit'){
      states[index]={phase:'connected',requestId:body.requestId};
      if(scenario==='lost-reply'&&second)return route.abort('connectionreset');
      return json(state());
     }
     return json({},400);
    }
    if(url.pathname.startsWith('/api/'))return json({},404);
    if(url.pathname==='/app.js')return route.fulfill({contentType:'text/javascript',body:''});
    const file=url.pathname==='/'?'index.html':url.pathname.slice(1);
    if(!/^[a-z-]+\.(html|css|js)$/.test(file))return route.abort();
    return route.fulfill({body:await fs.readFile(path.join(root,file)),contentType:file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html'});
   });
   await page.goto('http://cat-test.local/');
   await page.evaluate(async()=>{const wifi=await import('/wifi-settings.js');wifi.initializeWifiSettings({canChange:()=>true});});
   assert.equal(await page.locator('input[name="wifi-target"]').count(),0);
   assert.equal(await page.locator('#camera-address').count(),0);
   await page.locator('#wifi-open').click();
   await page.waitForFunction(()=>document.querySelector('#wifi-device-1').textContent.includes('Current'));
   if(scenario==='changed-ip'){
    assert.equal(await page.evaluate(async()=> (await import('/device-network.js')).getSecondCameraAddress()),'http://192.168.0.107');
    peerIp='192.168.0.122';
    await page.evaluate(async()=> (await import('/device-network.js')).refreshDevices());
    assert.equal(await page.evaluate(async()=> (await import('/device-network.js')).getSecondCameraAddress()),'http://192.168.0.122');
   }
   await page.locator('#wifi-ssid').fill('NewHome');await page.locator('#wifi-password').fill('test-password');await page.locator('#wifi-submit').click();
   await page.waitForFunction(()=>!document.querySelector('#wifi-submit').disabled);
   const message=await page.locator('#wifi-result').innerText();
   if(['success','lost-reply','changed-ip'].includes(scenario)){
    assert.match(message,/两台设备均已连接/);assert.equal(calls.filter(c=>c.action==='commit').length,2);assert.equal(calls.filter(c=>c.action==='cancel').length,0);
    const jobs=new Set(calls.map(c=>c.id));assert.equal(jobs.size,1);
   }else if(scenario==='prepare-failure'){assert.match(message,/没有开始切换/);assert.equal(calls.filter(c=>c.action==='commit').length,0);}
   else{assert.equal(calls.length,0);assert.match(message,scenario==='peer-missing'?/尚未找到二号/:/身份不匹配/);}
   assert.equal(await page.locator('#wifi-password').inputValue(),'');
   assert.equal(await page.evaluate(()=>Object.values(localStorage).some(v=>v.includes('test-password'))),false);
   assert.deepEqual(errors,[]);
   for(const width of [320,390]){await page.setViewportSize({width,height:844});assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));}
   console.log('PASS '+scenario);await page.close();
  }
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
