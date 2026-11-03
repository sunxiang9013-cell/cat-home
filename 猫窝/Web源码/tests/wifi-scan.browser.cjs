// Offline browser integration check. Set PLAYWRIGHT_MODULE to an installed playwright package.
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const fs=require('node:fs/promises');
const path=require('node:path');
const assert=require('node:assert/strict');
const root=path.resolve(__dirname,'..');
(async()=>{
  const browser=await chromium.launch({channel:'msedge',headless:true});
  try {
    const page=await browser.newPage({viewport:{width:320,height:740}});
    let mode='success',starts=[],writes=0,errors=[];
    page.on('pageerror',error=>errors.push(error.message));
    await page.route('**/*',async route=>{
      const req=route.request(),url=new URL(req.url()),second=url.hostname==='192.168.0.6';
      const json=(body,status=200)=>route.fulfill({status,contentType:'application/json',body:JSON.stringify(body),headers:{'Access-Control-Allow-Origin':'*'}});
      if(url.pathname==='/api/device')return json({product:'cat-home',role:'main',id:'1c:db:d4:75:10:ec',discoveryVersion:1,ip:'192.168.0.5',peer:{role:'camera',id:'88:f1:55:67:97:d0',online:true,ip:'192.168.0.6'}});
      if(url.pathname==='/api/wifi/scan'){
        if(req.method()==='POST') {starts.push(second?2:1);assert.equal(req.headers()['x-cat-home'],'1');}
        if(mode==='legacy'&&second)return json({},404);
        if(mode==='timeout')return route.abort('timedout');
        if(mode==='waiting')return json({scanId:'scan-1',phase:'scanning'});
        if(req.method()==='POST')return json({scanId:'scan-1',phase:'scanning'});
        if(mode==='changed')return json({scanId:'scan-2',phase:'ready',networks:[]});
        return json({scanId:'scan-1',phase:'ready',networks:mode==='empty'?[]:[
          {ssid:'Home 2.4G',rssi:second?-70:-48,open:false},
          {ssid:'<img src=x onerror=alert(1)>',rssi:-80,open:true}
        ]});
      }
      if(url.pathname==='/api/wifi'){
        if(req.method()!=='GET')writes++;
        return json({connected:true,ssid:'Current',ip:second?'192.168.0.6':'192.168.0.5',bootId:'boot-123',phase:'idle'});
      }
      if(url.pathname.startsWith('/api/'))return json({},404);
      if(url.pathname==='/app.js')return route.fulfill({contentType:'text/javascript',body:''});
      const file=url.pathname==='/'?'index.html':url.pathname.slice(1);
      if(!/^[a-z-]+\.(html|css|js)$/.test(file))return route.abort();
      return route.fulfill({body:await fs.readFile(path.join(root,file)),contentType:file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html'});
    });
    await page.goto('http://cat-test.local/');
    await page.evaluate(async()=>{const {initializeWifiSettings}=await import('/wifi-settings.js');initializeWifiSettings({canChange:()=>true});});
    await page.locator('#wifi-open').click();
    await page.waitForFunction(()=>document.querySelector('#wifi-device-1').textContent.includes('Current'));
    await page.locator('#wifi-scan').click();
    await page.waitForFunction(()=>document.querySelectorAll('.wifi-network').length===2);
    assert.deepEqual(starts.sort(),[1,2]);
    assert.match(await page.locator('.wifi-network').first().innerText(),/一号主控、二号摄像头/);
    assert.equal(await page.locator('#wifi-networks img').count(),0);
    if(process.env.QA_SCREENSHOT)await page.screenshot({path:process.env.QA_SCREENSHOT,fullPage:true});
    await page.locator('.wifi-network').first().click();
    assert.equal(await page.locator('#wifi-ssid').inputValue(),'Home 2.4G');
    assert.equal(await page.locator('#wifi-open-network').isChecked(),false);
    await page.locator('#wifi-scan').click();
    await page.waitForFunction(()=>document.querySelectorAll('.wifi-network').length===2);
    await page.locator('.wifi-network').nth(1).click();
    assert.equal(await page.locator('#wifi-open-network').isChecked(),true);
    assert.equal(await page.locator('#wifi-password').isDisabled(),true);
    assert.equal(writes,0,'Selecting a network must not connect automatically');
    for(const width of [320,390]){
      await page.setViewportSize({width,height:740});
      assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    }
    mode='legacy';await page.locator('#wifi-scan').click();
    await page.waitForFunction(()=>document.querySelector('#wifi-scan-status').textContent.includes('固件需更新'));
    assert.equal(await page.locator('.wifi-network').count(),2,'Keep results from the working board');
    mode='empty';await page.locator('#wifi-scan').click();
    await page.waitForFunction(()=>document.querySelector('#wifi-scan-status').textContent.includes('未找到'));
    assert.equal(await page.locator('.wifi-network').count(),0);
    mode='changed';await page.locator('#wifi-scan').click();
    await page.waitForFunction(()=>document.querySelector('#wifi-scan-status').textContent.includes('状态已变化'));
    mode='timeout';await page.locator('#wifi-scan').click();
    await page.waitForFunction(()=>document.querySelector('#wifi-scan-status').textContent.includes('连接超时或不可达'));
    assert.equal(await page.locator('#wifi-scan').isDisabled(),false);
    mode='waiting';await page.locator('#wifi-scan').click();
    await page.locator('#wifi-ssid').fill('Test');
    await page.locator('#wifi-submit').click();
    assert.match(await page.locator('#wifi-result').innerText(),/等待 Wi-Fi 搜索/);
    assert.equal(writes,0);
    assert.equal(await page.locator('input[name="wifi-target"]').count(),0);
    await page.locator('#wifi-close').click();
    await page.waitForTimeout(1000);
    assert.equal(await page.locator('#wifi-scan-status').innerText(),'');
    await page.locator('#wifi-open').click();
    mode='success';starts=[];await page.locator('#wifi-scan').click();
    await page.waitForFunction(()=>document.querySelectorAll('.wifi-network').length===2);
    assert.deepEqual(starts.sort(),[1,2]);
    mode='waiting';await page.locator('#wifi-scan').click();await page.locator('#wifi-close').click();
    await page.waitForTimeout(1000);
    assert.equal(await page.locator('#wifi-scan-status').innerText(),'');
    assert.deepEqual(errors,[]);
    console.log('PASS: scan/poll, merge, signal order, SSID escaping, select protected/open, no implicit connect, legacy partial results, empty result, changed scan ID, submit guard, both-only controls, close cancellation, mobile widths.');
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
