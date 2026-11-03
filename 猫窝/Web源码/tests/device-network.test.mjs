import test from 'node:test';
import assert from 'node:assert/strict';
import {validMain,subnetCandidates,findMain} from '../device-network.js';
import {commitWifiJob} from '../wifi-settings.js';
const identity={product:'cat-home',role:'main',id:'1c:db:d4:75:10:ec',ip:'192.168.0.104',discoveryVersion:1};
const device={id:2,base:'http://192.168.0.107',bootId:'boot-one'};
const job='a'.repeat(32);
const state=phase=>({bootId:device.bootId,requestId:job,phase});
test('only the paired main on a private address is accepted',()=>{
 assert(validMain(identity));
 for(const change of [{role:'camera'},{id:'other'},{ip:'8.8.8.8'},{ip:'192.168.0.300'},{discoveryVersion:0},{product:'other'}])assert(!validMain({...identity,...change}));
 assert.equal(subnetCandidates('192.168.0.104').length,254);
 assert.deepEqual(subnetCandidates('example.com'),[]);
});
test('mDNS recovers the main after moving to another subnet',async()=>{
 let calls=0;
 const found=await findMain({origins:['http://192.168.0.104'],probe:async base=>{calls++;assert.equal(base,'http://cat-home.local');return {...identity,ip:'10.0.3.41'};}});
 assert.equal(found.base,'http://10.0.3.41');assert.equal(calls,1);
});
test('bounded discovery rejects wrong boards and finds changed DHCP address',async()=>{
 let active=0,max=0;
 const found=await findMain({origins:['http://192.168.0.104'],probe:async base=>{
  active++;max=Math.max(max,active);await new Promise(r=>setTimeout(r,1));active--;
  if(base==='http://192.168.0.9')return {...identity,ip:'192.168.0.9'};
  return {...identity,id:'not-our-board'};
 }});
 assert.equal(found.base,'http://192.168.0.9');assert(max<=6);
});
test('a lost commit acknowledgement is reconciled without cancelling',async()=>{
 const actions=[];
 const actual=await commitWifiJob(async(d,body)=>{actions.push(body?.action||'read');if(body)throw new Error('lost reply');return state('queued');},device,job);
 assert.equal(actual.phase,'queued');assert.deepEqual(actions,['commit','read']);
});
test('retry reuses job only after the board reports still prepared',async()=>{
 const actions=[];
 const actual=await commitWifiJob(async(d,body)=>{
  actions.push(body?.action||'read');
  if(actions.length===1)throw new Error('not delivered');
  if(!body)return state('prepared');
  assert.equal(body.requestId,job);return state('connecting');
 },device,job);
 assert.equal(actual.phase,'connecting');assert.deepEqual(actions,['commit','read','commit']);
});
test('unknown state or a different boot never produces false success or cancellation',async()=>{
 for(const reply of [null,{...state('connected'),bootId:'restarted'},{...state('connected'),requestId:'different'}]){
  const actions=[];
  const actual=await commitWifiJob(async(d,body)=>{actions.push(body?.action||'read');if(body||!reply)throw new Error('unavailable');return reply;},device,job);
  assert.equal(actual,null);assert.deepEqual(actions,['commit','read']);
 }
});
