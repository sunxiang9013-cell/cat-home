const MAIN_ID='1c:db:d4:75:10:ec';
const CAMERA_ID='88:f1:55:67:97:d0';
let current=null,inflight=null,misses=0,reconnecting=false,stopped=false;
let secondAddress='';
const listeners=new Set();
export function getSecondCameraAddress(){return secondAddress;}
export function onDevicesChanged(callback){listeners.add(callback);return()=>listeners.delete(callback);}
export function privateAddress(value){
  if(typeof value!=='string'||!/^\d+\.\d+\.\d+\.\d+$/.test(value))return false;
  const a=value.split('.').map(Number);
  return a.every(n=>n>=0&&n<=255)&&(a[0]===10||(a[0]===192&&a[1]===168)||(a[0]===172&&a[1]>=16&&a[1]<=31));
}
export function validMain(state){return state?.product==='cat-home'&&state.role==='main'&&state.id?.toLowerCase()===MAIN_ID&&state.discoveryVersion===1&&privateAddress(state.ip);}
function accept(state){
  if(!validMain(state))throw new Error('主控固件需更新后才能自动连接两台设备');
  current=state;misses=0;
  const peer=state.peer;
  const next=peer?.online===true&&peer.role==='camera'&&peer.id?.toLowerCase()===CAMERA_ID&&privateAddress(peer.ip)&&peer.ip!==state.ip?'http://'+peer.ip:'';
  const changed=secondAddress!==next;secondAddress=next;
  try{localStorage.setItem('cat-home-main-address','http://'+state.ip);}catch{}
  if(changed)listeners.forEach(fn=>fn(next));
  return state;
}
async function readIdentity(base,timeout=4000,signal){
  const response=await fetch(base+'/api/device',{cache:'no-store',credentials:'omit',signal:signal?AbortSignal.any([signal,AbortSignal.timeout(timeout)]):AbortSignal.timeout(timeout)});
  if(!response.ok)throw new Error('暂时无法自动发现设备');
  const state=await response.json();if(!validMain(state))throw new Error('设备身份不匹配');return state;
}
export async function refreshDevices(){
  if(inflight)return inflight;
  inflight=readIdentity('').then(accept).finally(()=>{inflight=null;});return inflight;
}
export async function requirePair(){
  const state=await refreshDevices();
  if(!secondAddress)throw new Error('尚未找到二号摄像头。请让两块板通电并连接同一 Wi-Fi，找到后才能一起更换网络。');
  return [{id:1,base:'',deviceId:MAIN_ID},{id:2,base:secondAddress,deviceId:CAMERA_ID}];
}
export function scanDevices(){return [{id:1,base:''},{id:2,base:secondAddress}];}
export function subnetCandidates(host){
  if(!privateAddress(host))return [];
  const prefix=host.split('.').slice(0,3).join('.');
  return Array.from({length:254},(_,i)=>'http://'+prefix+'.'+(i+1));
}
// A browser cannot receive the boards' UDP discovery packets. After a network
// move use mDNS first, then bounded probes of the previously known /24 subnet.
// Never redirect to an endpoint unless its product and paired identity match.
export async function findMain({probe=readIdentity,origins=[],signal}={}){
  const first=[...new Set(['http://cat-home.local',...origins])];
  for(const base of first){
    if(signal?.aborted)return null;
    try{const state=await probe(base,1600,signal);if(validMain(state))return {base:'http://'+state.ip,state};}catch{}
  }
  const candidates=[...new Set(origins.flatMap(base=>{try{return subnetCandidates(new URL(base).hostname);}catch{return [];}}))].filter(base=>!first.includes(base));
  const controller=new AbortController();
  const searchSignal=signal?AbortSignal.any([signal,controller.signal]):controller.signal;
  let index=0,found=null;
  await Promise.all(Array.from({length:6},async()=>{
    while(!found&&!searchSignal.aborted&&index<candidates.length){
      const base=candidates[index++];
      try{const state=await probe(base,800,searchSignal);if(validMain(state)&&!found){found={base:'http://'+state.ip,state};controller.abort();}}catch{}
    }
  }));
  return found;
}
function notice(text){const node=document.querySelector('#network-recovery');if(node){node.hidden=false;node.textContent=text;}}
export async function recoverMain(){
  if(reconnecting||stopped||!privateAddress(location.hostname)&&location.hostname!=='cat-home.local')return;
  reconnecting=true;
  notice('正在自动寻找猫窝。更换网络后，请先把手机连接到新的 Wi-Fi，并保持此页面打开。');
  const origins=[location.origin];
  try{const saved=localStorage.getItem('cat-home-main-address');if(saved&&privateAddress(new URL(saved).hostname))origins.push(saved);}catch{}
  try{
    const found=await findMain({origins});
    if(found&&!stopped){
      if(found.base!==location.origin){location.replace(found.base+'/'+location.hash);return;}
      accept(found.state);const node=document.querySelector('#network-recovery');if(node)node.hidden=true;
    }else notice('暂未找到猫窝。请确认手机已连接目标 Wi-Fi；可点击“重新连接”。若新网络使用不同网段且不支持本地域名，请从路由器设备列表打开 cat-home。');
  }finally{reconnecting=false;}
}
export function initializeDeviceNetwork(){
  let timer;
  async function poll(){
    if(stopped)return;
    if(!document.hidden&&!reconnecting){
      try{await refreshDevices();const node=document.querySelector('#network-recovery');if(node)node.hidden=true;}
      catch{misses++;if(misses===3){secondAddress='';listeners.forEach(fn=>fn(''));}if(misses>=3)await recoverMain();}
    }
    timer=setTimeout(poll,misses>=3?15000:5000);
  }
  window.addEventListener('online',()=>recoverMain());
  window.addEventListener('pagehide',()=>{stopped=true;clearTimeout(timer);});
  document.querySelector('#network-retry')?.addEventListener('click',()=>recoverMain());
  document.querySelector('#camera-rediscover')?.addEventListener('click',()=>refreshDevices().catch(()=>{}));
  poll();
}
