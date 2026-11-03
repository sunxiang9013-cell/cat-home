export function initializeWifiScan({devices, labels, canScan}) {
  const $=selector=>document.querySelector(selector);
  let controller=null,version=0,scanning=false;
  function stop() {
    version++;controller?.abort();controller=null;scanning=false;
    $('#wifi-scan').disabled=false;$('#wifi-scan').textContent='搜索 Wi-Fi';
  }
  function clear() {stop();$('#wifi-networks').replaceChildren();$('#wifi-scan-status').textContent='';}
  $('#wifi-dialog').addEventListener('close',clear);
  async function request(device,start,signal) {
    if(device.id===2&&!device.base)throw new Error('尚未自动发现二号摄像头');
    const response=await fetch(device.base+'/api/wifi/scan',{
      credentials:'omit',cache:'no-store',signal:AbortSignal.any([signal,AbortSignal.timeout(4000)]),
      ...(start?{method:'POST',headers:{'X-Cat-Home':'1'}}:{})
    });
    if(!response.ok)throw new Error(response.status===404?'固件需更新后才能搜索':response.status===409?'设备正在切换网络，请稍后重试':'搜索失败，请检查设备连接');
    return response.json();
  }
  async function scan(device,signal) {
    let state=await request(device,true,signal);
    const scanId=state.scanId,until=Date.now()+22000;
    if(!scanId)throw new Error('设备未确认搜索，请重试');
    while(state.phase==='scanning'&&Date.now()<until) {
      await new Promise(resolve=>setTimeout(resolve,700));
      if(signal.aborted)throw new Error('cancelled');
      state=await request(device,false,signal);
      if(state.scanId!==scanId)throw new Error('设备状态已变化，请重新搜索');
    }
    if(state.phase!=='ready'||!Array.isArray(state.networks))throw new Error('搜索未完成，请稍后重试');
    return {device,networks:state.networks};
  }
  $('#wifi-scan').addEventListener('click',async()=>{
    if(scanning)return;
    if(!canScan()){$('#wifi-scan-status').textContent='请等待当前下料或网络设置结束后再搜索。';return;}
    clear();const current=version;controller=new AbortController();const signal=controller.signal;
    scanning=true;$('#wifi-scan').disabled=true;$('#wifi-scan').textContent='正在搜索…';
    $('#wifi-scan-status').textContent='正在搜索设备附近的 2.4GHz Wi-Fi，画面可能短暂变慢…';
    const targets=devices();
    const replies=await Promise.allSettled(targets.map(device=>scan(device,signal)));
    if(current!==version)return;
    scanning=false;controller=null;$('#wifi-scan').disabled=false;$('#wifi-scan').textContent='重新搜索';
    const networks=new Map(),errors=[];
    replies.forEach((reply,index)=>{
      if(reply.status==='rejected') {
        const error=reply.reason;
        const message=error.name==='TypeError'||error.name==='TimeoutError'||error.name==='AbortError'?'连接超时或不可达，请检查设备地址、供电和网络':error.message;
        errors.push(labels[targets[index].id]+'：'+message);return;
      }
      reply.value.networks.forEach(network=>{
        if(typeof network.ssid!=='string'||!network.ssid||typeof network.open!=='boolean'||!Number.isFinite(network.rssi))return;
        const key=JSON.stringify([network.ssid,network.open]),previous=networks.get(key);
        if(previous){previous.rssi=Math.max(previous.rssi,network.rssi);previous.sources.push(reply.value.device.id);}
        else networks.set(key,{...network,sources:[reply.value.device.id]});
      });
    });
    [...networks.values()].sort((a,b)=>b.rssi-a.rssi).forEach(network=>{
      const button=document.createElement('button');button.type='button';button.className='wifi-network';
      const name=document.createElement('strong');name.textContent=network.ssid;
      const info=document.createElement('span');
      const strength=network.rssi>=-60?'信号强':network.rssi>=-75?'信号中等':'信号弱';
      info.textContent=`${strength} · ${network.open?'无密码':'需密码'} · ${network.sources.map(id=>labels[id]).join('、')}`;
      button.append(name,info);
      button.addEventListener('click',()=>{
        if(!canScan())return;
        $('#wifi-ssid').value=network.ssid;$('#wifi-password').value='';
        $('#wifi-open-network').checked=network.open;$('#wifi-open-network').dispatchEvent(new Event('change'));
        $('#wifi-scan-status').textContent='已选择 '+network.ssid+(network.open?'（无密码网络）':'，请输入密码');
        $('#wifi-networks').replaceChildren();if(!network.open)$('#wifi-password').focus();
      });
      $('#wifi-networks').append(button);
    });
    const message=networks.size?`找到 ${networks.size} 个网络，点击名称选择。`:'未找到可显示的网络，可重新搜索或手动输入隐藏网络名称。';
    $('#wifi-scan-status').textContent=message+(errors.length?' '+errors.join('；'):'');
  });
  return {isScanning:()=>scanning,stop:clear};
}
