// Hardware read-only soak: status + snapshots with the currently selected preview.
// Uses no feeding, lighting or network-setting commands.
const assert=require('node:assert/strict');
const base=process.env.CAMERA_DEVICE_URL||'http://192.168.0.6';
async function json(route){const r=await fetch(base+route,{signal:AbortSignal.timeout(5000)});assert(r.ok,route+' '+r.status);return r.json();}
(async()=>{
  const initial=await json('/api/camera/status');
  assert.equal(initial.firmware,'cat-camera-remote-pair-2');
  const results=[];
  for(let i=0;i<30;i++){
    const begin=Date.now();
    const [state,photo]=await Promise.all([
      json('/api/camera/status'),
      fetch(base+'/api/camera/capture',{signal:AbortSignal.timeout(7000)}).then(async r=>{assert(r.ok,'Snapshot '+r.status);return Buffer.from(await r.arrayBuffer());})
    ]);
    assert(state.ready&&state.wifi);assert(state.uptimeMs>=initial.uptimeMs,'Unexpected reboot');
    assert(photo[0]===255&&photo[1]===216&&photo.at(-2)===255&&photo.at(-1)===217,'Invalid JPEG');
    results.push({ms:Date.now()-begin,clients:state.httpClients,frames:state.frames,failures:state.frameFailures,preview:state.activeStreams});
    if(i===14)console.log('15/30 concurrent snapshot and status checks passed.');
    await new Promise(resolve=>setTimeout(resolve,Math.max(0,2000-(Date.now()-begin))));
  }
  const final=await json('/api/camera/status');
  assert(final.frames>initial.frames);assert.equal(final.frameFailures,initial.frameFailures);
  console.log(JSON.stringify({checks:results.length,maxMs:Math.max(...results.map(r=>r.ms)),maxClients:Math.max(...results.map(r=>r.clients)),framesAdded:final.frames-initial.frames,frameFailures:final.frameFailures,previewObserved:results.some(r=>r.preview>0),bootStable:true}));
})().catch(e=>{console.error(e.message);process.exitCode=1;});
