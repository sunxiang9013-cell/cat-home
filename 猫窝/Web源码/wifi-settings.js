import {requirePair,scanDevices,refreshDevices,recoverMain} from './device-network.js';
import {initializeWifiScan} from './wifi-scan.js';

export function sameWifiJob(state,device,job){return state?.bootId===device.bootId&&state.requestId===job;}
export async function commitWifiJob(request,device,job){
  // A lost reply is not a lost command. Never cancel once commit may be delivered.
  // Retry the same idempotent job only when a read confirms it is still prepared.
  for(let attempt=0;attempt<2;attempt++){
    try{const state=await request(device,{action:'commit',requestId:job,bootId:device.bootId});if(sameWifiJob(state,device,job))return state;}catch{}
    try{const state=await request(device);if(!sameWifiJob(state,device,job))return null;if(state.phase==='prepared'&&attempt===0)continue;return state;}catch{return null;}
  }
  return null;
}
export function initializeWifiSettings({canChange}){
  const $=selector=>document.querySelector(selector);
  let busy=false,loadVersion=0;
  const row=(id,text)=>{$('#wifi-device-'+id).textContent=text;};
  const result=(text,error=false)=>{$('#wifi-result').textContent=text;$('#wifi-result').classList.toggle('error',error);};
  const id=()=>Array.from(crypto.getRandomValues(new Uint8Array(16)),b=>b.toString(16).padStart(2,'0')).join('');
  const scanner=initializeWifiScan({devices:scanDevices,labels:{1:'一号主控',2:'二号摄像头'},canScan:()=>!busy&&canChange()});
  function clearPassword(){$('#wifi-password').value='';$('#wifi-password').type='password';$('#wifi-show-password').textContent='显示';$('#wifi-show-password').setAttribute('aria-pressed','false');}
  const pauseCamera=value=>window.dispatchEvent(new CustomEvent('cat-network-maintenance',{detail:value}));
  function setBusy(value){busy=value;pauseCamera(value||$('#wifi-dialog').open);$('#wifi-fields').disabled=value;$('#wifi-submit').disabled=value;$('#wifi-submit').textContent=value?'正在处理…':'两台一起连接';}
  async function request(device,body){
    if(device.id===2&&!device.base)throw new Error('尚未自动发现二号摄像头');
    const response=await fetch(device.base+'/api/wifi',{cache:'no-store',credentials:'omit',signal:AbortSignal.timeout(5000),...(body?{method:'POST',headers:{'Content-Type':'application/json','X-Cat-Home':'1'},body:JSON.stringify(body)}:{})});
    const state=await response.json();
    if(!response.ok)throw new Error(state.error==='device_restarted'?'设备已重启，请重新提交':'设备未接受网络设置，请检查两台设备连接');
    return state;
  }
  function describe(state){
    if(['connecting','queued'].includes(state.phase))return '正在切换网络…';
    if(state.phase==='failed')return state.connected?'连接失败，已回到 '+state.ssid:'连接失败，正在恢复原网络';
    return state.connected?`${state.ssid} · 已连接`:'设备尚未连接 Wi-Fi';
  }
  $('#wifi-open').addEventListener('click',async()=>{
    $('#wifi-dialog').showModal();pauseCamera(true);if(busy)return;
    clearPassword();result('');$('#wifi-reconnect').hidden=true;
    const version=++loadVersion;
    try{await refreshDevices();}catch{}
    await Promise.all(scanDevices().map(async device=>{
      row(device.id,'正在读取状态');
      try{const state=await request(device);if(version!==loadVersion||busy)return;row(device.id,describe(state));if(device.id===1&&!$('#wifi-ssid').value)$('#wifi-ssid').value=state.ssid||'';}
      catch{if(version===loadVersion&&!busy)row(device.id,'尚未连接，请确认两块板通电并在同一 Wi-Fi');}
    }));
  });
  $('#wifi-close').addEventListener('click',()=>$('#wifi-dialog').close());
  $('#wifi-dialog').addEventListener('close',()=>{clearPassword();if(!busy)pauseCamera(false);});
  $('#wifi-show-password').addEventListener('click',()=>{const show=$('#wifi-password').type==='password';$('#wifi-password').type=show?'text':'password';$('#wifi-show-password').textContent=show?'隐藏':'显示';$('#wifi-show-password').setAttribute('aria-pressed',String(show));});
  $('#wifi-open-network').addEventListener('change',()=>{const open=$('#wifi-open-network').checked;$('#wifi-password').disabled=open;$('#wifi-password').required=!open;if(open)clearPassword();});
  async function cancelPrepared(targets,job){await Promise.all(targets.filter(d=>d.bootId).map(async device=>{try{await request(device,{action:'cancel',requestId:job,bootId:device.bootId});}catch{}}));}
  $('#wifi-form').addEventListener('submit',async event=>{
    event.preventDefault();if(busy)return;
    if(scanner.isScanning()){result('请等待 Wi-Fi 搜索结束后再连接。',true);return;}
    if(!canChange()){result('请先等待下料结束，再更换网络。',true);return;}
    const ssid=$('#wifi-ssid').value,open=$('#wifi-open-network').checked;
    let password=$('#wifi-password').value;
    if(!ssid||new TextEncoder().encode(ssid).length>32||(!open&&(new TextEncoder().encode(password).length<8||new TextEncoder().encode(password).length>63))){result('网络名称需为 1–32 字节，密码需为 8–63 字节。',true);return;}
    const job=id();scanner.stop();setBusy(true);loadVersion++;result('正在确认两台设备…');$('#wifi-reconnect').hidden=true;
    let commitStarted=false;
    try{
      const targets=await requirePair();
      // Verify both boot identities before sending credentials to either board.
      await Promise.all(targets.map(async device=>{
        const state=await request(device);
        if(typeof state.bootId!=='string'||!state.bootId||state.deviceId?.toLowerCase()!==device.deviceId)throw new Error('设备身份不匹配，请更新两块板固件后重试');
        device.bootId=state.bootId;
      }));
      const prepared=await Promise.allSettled(targets.map(async device=>{
        row(device.id,'正在发送设置…');
        const state=await request(device,{action:'prepare',bootId:device.bootId,requestId:job,ssid,password:open?'':password,open});
        if(!sameWifiJob(state,device,job)||state.phase!=='prepared')throw new Error('设备未确认设置');
        row(device.id,'设置已接收');
      }));
      clearPassword();password='';
      if(prepared.some(r=>r.status==='rejected')){
        await cancelPrepared(targets,job);targets.forEach((d,index)=>row(d.id,prepared[index].status==='fulfilled'?'未开始切换':'未确认设置'));
        result('两台设备未全部确认设置，本次没有开始切换。请检查连接后重试。',true);return;
      }
      commitStarted=true;$('#wifi-reconnect').hidden=false;
      const replies=await Promise.all(targets.map(device=>commitWifiJob(request,device,job)));
      const pending=new Set(targets.map(d=>d.id));
      function show(device,state){
        if(!state){row(device.id,'未收到确认，正在核对是否已切换');return;}
        if(!sameWifiJob(state,device,job)){row(device.id,'设备已重启或任务改变，需重新确认连接');pending.delete(device.id);return;}
        if(state.phase==='connected'&&state.connected&&state.ssid===ssid){row(device.id,'已连接 '+ssid);device.success=true;pending.delete(device.id);}
        else if(['failed','cancelled'].includes(state.phase)){row(device.id,state.phase==='cancelled'?'切换已取消':describe(state));pending.delete(device.id);}
        else if(state.phase==='prepared')row(device.id,'尚未确认开始，继续核对');
        else row(device.id,'正在连接 '+ssid);
      }
      targets.forEach((d,index)=>show(d,replies[index]));
      result('正在切换两台设备。请把手机连接到 '+ssid+'，保持页面打开，系统会自动寻找设备的新地址。');
      const until=Date.now()+50000;
      while(pending.size&&Date.now()<until){
        await new Promise(resolve=>setTimeout(resolve,2000));
        try{await refreshDevices();const now=scanDevices()[1].base;if(now)targets[1].base=now;}catch{}
        await Promise.all(targets.filter(d=>pending.has(d.id)).map(async device=>{try{show(device,await request(device));}catch{row(device.id,'原连接已中断，等待在新网络确认');}}));
      }
      if(targets.every(d=>d.success))result('两台设备均已连接 '+ssid+'，地址已自动更新。');
      else{result('尚未确认两台设备的最终状态。请连接 '+ssid+'，系统会自动寻找设备；未收到回复不代表连接失败。',true);recoverMain();}
    }catch(error){result(commitStarted?'切换指令可能已送达，请连接目标 Wi-Fi 后等待自动发现。':error.message||'未完成设置，请检查两台设备连接。',true);}
    finally{password='';clearPassword();setBusy(false);}
  });
}
