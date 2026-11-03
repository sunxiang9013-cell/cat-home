import { FrameDecoder } from './frame-stream.js';

import {getSecondCameraAddress,onDevicesChanged,refreshDevices} from './device-network.js';
export {getSecondCameraAddress} from './device-network.js';

async function rotatedPhoto(blob) {
  const url=URL.createObjectURL(blob), image=new Image();
  try {
    image.src=url;await image.decode();
    const canvas=document.createElement('canvas');
    canvas.width=image.naturalWidth;canvas.height=image.naturalHeight;
    const context=canvas.getContext('2d');
    context.translate(canvas.width,canvas.height);context.rotate(Math.PI);
    context.drawImage(image,0,0);
    return await new Promise((resolve,reject)=>canvas.toBlob(value=>value?resolve(value):reject(new Error('照片旋转失败')),'image/jpeg',0.95));
  }finally{URL.revokeObjectURL(url);}
}

export function initializeCameraView({ toast, addActivity }) {
  const $ = selector => document.querySelector(selector);
  let secondAddress = getSecondCameraAddress();
  let selected = 0, running = true, generation = 0, controller = null, maintenance = false;
  let work = Promise.resolve(), frameUrl = null, saving = false;
  const names = ['一号摄像头', '二号摄像头'];
  const clock = new Intl.DateTimeFormat('zh-CN', {timeZone:'Asia/Shanghai',hour12:false,hour:'2-digit',minute:'2-digit',second:'2-digit'});
  const available = () => selected === 0 || Boolean(secondAddress);
  const source = () => selected === 0 ? '' : secondAddress;
  function controls() {
    [0,1].forEach(i => $(`#camera-select-${i+1}`).setAttribute('aria-pressed', String(selected === i)));
    $('#camera-number').textContent = `CAT HOME / 0${selected+1}`;
    $('#camera-image').alt = names[selected] + '实际画面';
    $('#camera-image').style.transform = selected === 1 ? 'rotate(180deg)' : '';
    $('#camera-toggle').disabled = !available();
    $('#camera-toggle').textContent = running ? '暂停画面' : '继续画面';
    $('#camera-save').disabled = !available() || saving || maintenance;
    $('#camera-save').textContent = saving ? '正在拍摄…' : '拍照保存';
  }
  function empty(message, mode = '连接中') {
    $('#camera-image').hidden = true;
    $('#camera-image').removeAttribute('src');
    if (frameUrl) URL.revokeObjectURL(frameUrl);
    frameUrl = null;
    $('#camera-empty').hidden = false;
    $('#camera-scene').classList.remove('has-frame');
    $('#camera-message').textContent = message;
    $('#camera-mode').textContent = mode;
    $('#camera-help').textContent = available() ? '请保持设备供电并连接同一网络' : '正在自动寻找二号摄像头，请确认两块板连接同一 Wi-Fi';
    $('#camera-connection').textContent = available() ? names[selected] + ' · 等待画面' : '正在自动发现二号摄像头';
    $('#camera-resolution').textContent = '—';
    $('#frame-time').textContent = '尚未收到画面';
  }
  async function display(blob, id, transport, stats) {
    if (id !== generation) return;
    const url = URL.createObjectURL(blob), image = new Image(); image.src = url;
    try { await image.decode(); } catch (error) { URL.revokeObjectURL(url); throw error; }
    if (id !== generation) { URL.revokeObjectURL(url); return; }
    if (frameUrl) URL.revokeObjectURL(frameUrl);
    frameUrl = url;
    $('#camera-image').src = url; $('#camera-image').hidden = false;
    $('#camera-empty').hidden = true; $('#camera-scene').classList.add('has-frame');
    $('#camera-mode').textContent = running ? '实物画面' : '已暂停';
    stats.frames++;
    const now = performance.now();
    if (!stats.updated || now - stats.updated >= 1000) {
      const fps = stats.frames * 1000 / Math.max(1, now - stats.started);
      $('#camera-connection').textContent = `${names[selected]} · ${transport}` + (stats.frames > 2 ? ` · ${fps.toFixed(1)} 帧/秒` : '');
      $('#camera-resolution').textContent = `${image.naturalWidth} × ${image.naturalHeight}`;
      $('#frame-time').textContent = `拍摄于 ${clock.format(new Date())}`;
      stats.updated = now;
    }
  }
  const stats = () => ({started:performance.now(),frames:0,updated:0});
  // The same AbortController covers discovery, streaming, snapshots and retry waits.
  // A new source is queued behind cleanup of the previous source, including image decode.
  function cancel() {
    generation++;
    controller?.abort();
    return generation;
  }
  async function timedFetch(url, signal, timeout = 5000) {
    const response = await fetch(url, {cache:'no-store',credentials:'omit',signal:AbortSignal.any([signal,AbortSignal.timeout(timeout)])});
    if (!response.ok) throw new Error(response.status === 503 ? '摄像头忙，正在重试' : '摄像头暂时无法连接');
    return response;
  }
  function wait(ms, signal) {
    return new Promise(resolve => {
      const finish = () => { clearTimeout(timer); signal.removeEventListener('abort', finish); resolve(); };
      const timer = setTimeout(finish, ms);
      signal.addEventListener('abort', finish, {once:true});
      if (signal.aborted) finish();
    });
  }
  async function snapshot(base, signal) {
    const response = await timedFetch(base + '/api/camera/capture', signal, 11000);
    const blob = await response.blob();
    if (blob.type !== 'image/jpeg') throw new Error('设备未返回有效照片');
    return {blob, transport:response.headers.get('X-Camera-Transport') === 'wifi' ? 'Wi-Fi' : 'USB'};
  }
  async function stream(base, id, signal) {
    const timeoutController = new AbortController();
    const streamSignal = AbortSignal.any([signal, timeoutController.signal]);
    let timer, reader;
    const arm = () => {clearTimeout(timer);timer=setTimeout(()=>timeoutController.abort(),6000);};
    const decoder = new FrameDecoder(), frameStats = stats();
    try {
      arm();
      const response = await fetch(base+'/api/camera/stream',{cache:'no-store',credentials:'omit',signal:streamSignal});
      if (!response.ok) throw new Error(response.status === 503 ? '摄像头忙，正在重试' : '无法连接摄像头');
      reader = response.body.getReader();
      while (!streamSignal.aborted && id === generation) {
        const {value,done} = await reader.read();
        if (done) throw new Error('画面连接中断，正在重试');
        arm();
        const frames = decoder.push(value);
        if (frames.length) await display(new Blob([frames.at(-1)],{type:'image/jpeg'}),id,'Wi-Fi',frameStats);
      }
    } finally {
      clearTimeout(timer); timeoutController.abort();
      if (reader) {try {await reader.cancel();} catch {} reader.releaseLock();}
    }
  }
  function restart({clear = false} = {}) {
    const id = cancel(), base = source();
    controls();
    if (clear) empty(available() ? '正在连接' + names[selected] + '…' : '二号摄像头等待接入', available() ? '连接中' : '待连接');
    if (!running || maintenance || document.hidden || !available()) return;
    work = work.catch(()=>{}).then(async()=>{
      if (id !== generation) return;
      const own = new AbortController(); controller = own;
      let failures = 0, streamSupported = null, statusFailures = 0, streamFailures = 0, snapshotsUntil = 0;
      const frameStats = stats();
      try {
        while (id === generation && !own.signal.aborted) {
          const started = performance.now();
          try {
            if (streamSupported === null) {
              let status;
              try {
                const response=await timedFetch(base+'/api/camera/status',own.signal);
                status=await response.json();statusFailures=0;
              } catch(error) {
                if(++statusFailures<2)throw error;
                // A status response can be lost while JPEG capture still works.
                streamSupported=false;
              }
              if(status){
                if(status.ready!==true&&base)throw new Error('二号摄像头尚未就绪');
                streamSupported=status.stream===true;
              }
            }
            if(streamSupported&&Date.now()>=snapshotsUntil){
              try{await stream(base,id,own.signal);}
              catch(error){if(++streamFailures>=2)snapshotsUntil=Date.now()+30000;throw error;}
            }else{
              const shot=await snapshot(base,own.signal);
              await display(shot.blob,id,shot.transport+(base?' · 抓拍模式':''),frameStats);
            }
            failures = 0;
          } catch (error) {
            if (id !== generation || own.signal.aborted) return;
            failures++;
            if(frameUrl){$('#camera-mode').textContent='画面未更新';$('#camera-connection').textContent='连接中断，正在重试 · 当前为上一帧';}
            else empty(error.name === 'TimeoutError' || error.name === 'AbortError' ? '连接超时，正在重试' : error.message,'未连接');
          }
          await wait(failures ? Math.min(15000,failures*3000) : Math.max(0,(base?500:40)-(performance.now()-started)),own.signal);
        }
      } finally {own.abort();if(controller===own)controller=null;}
    });
  }
  [0,1].forEach(i => $(`#camera-select-${i+1}`).addEventListener('click',()=>{
    if (selected === i) return;
    selected = i; running = true; restart({clear:true});
  }));
  $('#camera-toggle').addEventListener('click',()=>{
    running=!running;restart();
    if(!running){$('#camera-mode').textContent='已暂停';$('#camera-connection').textContent=names[selected]+' · 画面已暂停';}
  });
  $('#camera-save').addEventListener('click',async()=>{
    if(saving || !available())return;
    saving=true;controls();
    const id=cancel(),base=source(),name=names[selected],cameraIndex=selected;
    work=work.catch(()=>{}).then(async()=>{
      if(id!==generation)return;
      const own=new AbortController();controller=own;
      try {
        const shot=await snapshot(base,own.signal);
        if(id!==generation)return;
        await display(shot.blob,id,shot.transport,stats());
        if(id!==generation)return;
        const photo=cameraIndex===1?await rotatedPhoto(shot.blob):shot.blob;
        if(id!==generation)return;
        const url=URL.createObjectURL(photo),anchor=document.createElement('a');
        anchor.href=url;anchor.download=`cat-home-camera-${cameraIndex+1}-${Date.now()}.jpg`;
        document.body.append(anchor);anchor.click();anchor.remove();setTimeout(()=>URL.revokeObjectURL(url),30000);
        addActivity(name+'照片已拍摄','已请求浏览器保存');toast('照片已拍摄，请查看浏览器下载');
      }catch {if(id===generation)toast('拍照失败，请检查摄像头连接');}
      finally {own.abort();if(controller===own)controller=null;}
    });
    await work;
    saving=false;controls();if(id===generation)restart();
  });
  onDevicesChanged(address=>{
    if(secondAddress===address)return;
    secondAddress=address;if(selected===1)restart({clear:true});
  });
  $('#camera-settings').addEventListener('click',()=>refreshDevices().catch(()=>toast('暂未找到设备，请确认两块板在同一 Wi-Fi')));
  window.addEventListener('cat-network-maintenance',event=>{maintenance=Boolean(event.detail);restart();});
  document.addEventListener('visibilitychange',()=>restart());
  window.addEventListener('pagehide',cancel);
  restart({clear:true});
}
