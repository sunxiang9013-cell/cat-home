import {initializeDeviceNetwork} from './device-network.js';
import { validSeconds } from './model.js';
import { initializeCameraView } from './camera-view.js';
import { initializeWifiSettings } from './wifi-settings.js';

const $ = (selector) => document.querySelector(selector);
const names = ['一号粮仓', '二号粮仓', '三号粮仓'];
const seconds = [1.2, 1.2, 1.2];
let feedState = null;
let feedOnline = false;
let feedSending = false;
let feedStopping = false;
let feedUncertain = false;
let pendingFeedId = null;
let pendingChannel = 0;
let feedVersion = 0;
let feedPollTimer;
let feedReceivedAt = 0;
let lastFeedEvent = '';
let toastTimer;
const clock = new Intl.DateTimeFormat('zh-CN', { timeZone: 'Asia/Shanghai', hour: '2-digit', minute: '2-digit' });
$('#today').textContent = new Intl.DateTimeFormat('zh-CN', { timeZone: 'Asia/Shanghai', month: 'numeric', day: 'numeric' }).format(new Date());

$('#feed-grid').innerHTML = names.map((name, index) => `
  <article class="feed-card" id="feeder-${index + 1}">
    <div class="feed-card-top"><span class="feed-symbol"><svg class="icon"><use href="#i-bowl"/></svg></span><span class="feed-number">0${index + 1}</span></div>
    <h3>${name}</h3><p class="feed-description">每次约 5 圈</p>
    <button class="feed-button" data-channel="${index + 1}" aria-label="${name}下料约5圈"><span>下料 · 约 5 圈</span><svg class="icon"><use href="#i-arrow"/></svg></button>
    <div class="feed-meta"><span class="feed-state">正在连接设备</span><span class="feed-duration">预计 6.0 秒</span></div>
    <div class="feed-progress" role="progressbar" aria-label="${name}下料进度" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0"></div>
  </article>`).join('');

function toast(message) {
  $('#toast').textContent = message;
  $('#toast').classList.add('visible');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => $('#toast').classList.remove('visible'), 3500);
}
function addActivity(title, detail) {
  $('.empty-activity')?.remove();
  const row = document.createElement('li');
  const dot = document.createElement('span'); dot.className = 'activity-dot';
  const content = document.createElement('div');
  const heading = document.createElement('strong'); heading.textContent = title;
  const text = document.createElement('p'); text.textContent = detail;
  const time = document.createElement('time'); time.dateTime = new Date().toISOString(); time.textContent = clock.format(new Date());
  content.append(heading, text); row.append(dot, content, time);
  $('#activity-list').prepend(row);
  while ($('#activity-list').children.length > 12) $('#activity-list').lastElementChild.remove();
}
function feedingBusy() { return feedSending || feedStopping || feedUncertain || Boolean(feedState?.active); }
function refreshControls() {
  const busy = feedingBusy();
  document.querySelectorAll('.feed-button').forEach(button => { button.disabled = busy || !feedOnline || !feedState?.ready; });
  $('#stop-feeding').disabled = feedStopping || !(busy || !feedOnline);
  document.body.classList.toggle('feeding-active', busy);
  $('#settings-form button[type="submit"]').disabled = busy;
  document.querySelectorAll('.calibration-row input').forEach(input => { input.disabled = busy; });
}
function renderFeeding() {
  for (let channel = 1; channel <= 3; channel++) {
    const card = $(`#feeder-${channel}`);
    const active = feedState?.active && feedState.channel === channel;
    const remaining = active ? Math.max(0, feedState.remainingMs - (performance.now() - feedReceivedAt)) : 0;
    const fraction = active ? Math.max(0, Math.min(1, 1 - remaining / feedState.durationMs)) : 0;
    card.classList.toggle('running', Boolean(active));
    card.querySelector('.feed-button span').textContent = active ? '正在下料' : (feedSending && pendingChannel === channel ? '发送中…' : '下料 · 约 5 圈');
    card.querySelector('.feed-state').textContent = !feedOnline ? '设备状态未知' : feedUncertain ? '请停止或核对设备' : active ? (remaining ? '电机正在运行' : '等待设备停止') : feedState?.ready ? '设备就绪' : '电机不可用';
    card.querySelector('.feed-duration').textContent = active ? `剩余 ${(remaining / 1000).toFixed(1)} 秒` : `预计 ${(seconds[channel - 1] * 5).toFixed(1)} 秒`;
    const progress = card.querySelector('.feed-progress'); progress.style.width = `${fraction * 100}%`; progress.setAttribute('aria-valuenow', String(Math.round(fraction * 100)));
  }
  refreshControls();
}
function acceptFeedState(state) {
  if (state.supported !== true || typeof state.active !== 'boolean' || !state.bootId) throw new Error('下料设备状态无效');
  feedState = state; feedOnline = true; feedReceivedAt = performance.now();
  if (feedUncertain && pendingFeedId && state.requestId === pendingFeedId) feedUncertain = false;
  const event = state.requestId + ':' + state.result;
  if (state.requestId && event !== lastFeedEvent) {
    if (state.result === 'completed') { addActivity('下料任务已结束', '设备已输出停止指令 · 圈数与出粮量需实测'); toast('下料已结束'); }
    else if (state.result === 'stopped') addActivity('下料已停止', '设备已确认停止指令');
    else if (state.result === 'fault') toast('电机输出异常，请检查设备');
    lastFeedEvent = event;
  }
  renderFeeding();
}
async function pollFeeding() {
  clearTimeout(feedPollTimer);
  if (document.hidden) { feedPollTimer = setTimeout(pollFeeding, 4000); return; }
  if (feedSending || feedStopping) { feedPollTimer = setTimeout(pollFeeding, 300); return; }
  const version = feedVersion;
  try {
    const response = await fetch('/api/feed/status', { cache: 'no-store', signal: AbortSignal.timeout(4000) });
    if (!response.ok) throw new Error('无法读取下料状态');
    const state = await response.json();
    if (version === feedVersion && !feedSending && !feedStopping) acceptFeedState(state);
  } catch {
    if (version === feedVersion && !feedSending && !feedStopping) {
      feedOnline = false;
      if (feedState?.active) feedUncertain = true;
      renderFeeding();
    }
  } finally {
    feedPollTimer = setTimeout(pollFeeding, feedState?.active && !document.hidden ? 400 : 1500);
  }
}
async function feedCommand(path, body) {
  const response = await fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Cat-Home': '1' }, body: JSON.stringify(body), signal: AbortSignal.timeout(5000) });
  const result = await response.json();
  if (!response.ok) {
    const labels = { motor_busy:'其他粮仓正在下料', device_restarted:'设备已重启，正在更新状态', invalid_parameters:'下料参数超出允许范围', motor_unavailable:'电机不可用，请检查设备', request_id_conflict:'请求已失效，请刷新页面' };
    const error = new Error(labels[result.error] || '设备拒绝了下料请求'); error.rejected = true; throw error;
  }
  return result;
}
function makeFeedId() { const bytes = new Uint8Array(16); crypto.getRandomValues(bytes); return Array.from(bytes, n => n.toString(16).padStart(2,'0')).join(''); }
document.querySelectorAll('.feed-button').forEach(button => button.addEventListener('click', async () => {
  if (feedingBusy() || !feedOnline || !feedState?.ready) return;
  const channel = Number(button.dataset.channel);
  const id = makeFeedId(); pendingFeedId = id; pendingChannel = channel;
  feedSending = true; feedVersion++; renderFeeding();
  try {
    const state = await feedCommand('/api/feed', { channel, durationMs: Math.round(seconds[channel - 1] * 5000), requestId: id, bootId: feedState.bootId });
    // A later stop takes precedence over a late start response.
    if (pendingFeedId === id && !feedStopping) {
      acceptFeedState(state);
      if (state.active && state.requestId === id) addActivity(names[channel - 1] + '开始下料', '真实电机 · 按运行时间估算约 5 圈');
    }
  } catch (error) {
    if (pendingFeedId === id) {
      if (error.rejected) { pendingFeedId = null; feedOnline = false; toast(error.message); }
      else { feedUncertain = true; feedOnline = false; toast('未确认设备状态，请使用停止按钮；不会自动重发下料'); }
    }
  } finally { feedSending = false; renderFeeding(); }
}));
$('#stop-feeding').addEventListener('click', async () => {
  if (feedStopping) return;
  feedStopping = true; feedVersion++; renderFeeding();
  const cancelId = pendingFeedId;
  try {
    const state = await feedCommand('/api/feed/stop', cancelId ? { cancelRequestId: cancelId } : {});
    pendingFeedId = null; feedUncertain = false; acceptFeedState(state); toast('设备已确认停止');
  } catch { feedUncertain = true; feedOnline = false; toast('停止尚未确认，请检查设备连接；设备仍有运行超时保护'); }
  finally { feedStopping = false; renderFeeding(); }
});
setInterval(renderFeeding, 150);
pollFeeding();

let lightState = null;
let lightOnline = false;
let lightPending = 0;
let lightVersion = 0;
let lightTimer;
function renderLights() {
  for (let channel = 1; channel <= 2; channel++) {
    const entry = lightState?.lights.find(item => item.channel === channel);
    const button = $(`#light-${channel}`);
    button.disabled = Boolean(lightPending) || !lightOnline || !lightState?.ready;
    button.setAttribute('aria-busy', String(lightPending === channel));
    button.setAttribute('aria-checked', String(Boolean(entry?.on)));
    $(`#light-icon-${channel}`).classList.toggle('lit', Boolean(entry?.on) && lightOnline);
    $(`#light-status-${channel}`).textContent = lightPending === channel ? '正在设置…' : !lightOnline ? '状态未知 · 正在连接' : !lightState.ready ? '灯带控制不可用' : entry?.on ? '已开启' : '已关闭';
  }
}
function acceptLightState(state) {
  if (state.supported !== true || !state.bootId || !Array.isArray(state.lights) ||
      ![1,2].every(channel => state.lights.some(item => item.channel === channel && typeof item.on === 'boolean'))) throw new Error('灯带状态无效');
  lightState = state; lightOnline = true; renderLights();
}
async function pollLights() {
  clearTimeout(lightTimer);
  if (document.hidden) { lightTimer = setTimeout(pollLights, 4000); return; }
  if (lightPending) { lightTimer = setTimeout(pollLights, 300); return; }
  const version = lightVersion;
  try {
    const response = await fetch('/api/lights', { cache:'no-store', signal:AbortSignal.timeout(4000) });
    if (!response.ok) throw new Error('灯带未连接');
    const state = await response.json();
    if (version === lightVersion && !lightPending) acceptLightState(state);
  } catch {
    if (version === lightVersion && !lightPending) { lightOnline = false; renderLights(); }
  } finally { lightTimer = setTimeout(pollLights, document.hidden ? 4000 : 1500); }
}
for (let channel = 1; channel <= 2; channel++) {
  $(`#light-${channel}`).addEventListener('click', async () => {
    if (lightPending || !lightOnline || !lightState?.ready) return;
    const on = !lightState.lights.find(item => item.channel === channel).on;
    lightPending = channel; lightVersion++; renderLights();
    try {
      const response = await fetch(`/api/lights/${channel}`, {
        method:'PUT', headers:{'Content-Type':'application/json','X-Cat-Home':'1'},
        body:JSON.stringify({on,bootId:lightState.bootId}), signal:AbortSignal.timeout(5000)
      });
      const state = await response.json();
      if (!response.ok) throw new Error(state.error === 'device_restarted' ? '设备已重启，正在更新状态' : '灯带控制未成功，请检查设备');
      acceptLightState(state);
      const confirmed = state.lights.find(item => item.channel === channel).on;
      addActivity((channel === 1 ? '窝内' : '氛围') + '灯带' + (confirmed ? '已开启' : '已关闭'), '设备已确认开关输出');
      toast((channel === 1 ? '窝内' : '氛围') + '灯带' + (confirmed ? '已开启' : '已关闭'));
    } catch (error) {
      console.warn('灯带控制未确认', { channel, error: error.message });
      lightOnline = false; toast('连接超时或中断，正在重新读取灯带状态');
    } finally {
      lightPending = 0; renderLights();
    }
  });
}
renderLights(); pollLights();

$('#calibration-fields').innerHTML = names.map((name, index) => `<label class="calibration-row" for="seconds-${index}">${name}<input id="seconds-${index}" type="number" min="0.2" max="6" step="0.1" required value="1.2"><span>秒 / 圈</span></label>`).join('');
$('#settings-open').addEventListener('click', () => {
  seconds.forEach((value, index) => { $(`#seconds-${index}`).value = String(value); });
  $('#settings-error').textContent = feedingBusy() ? '下料正在运行或等待确认，停止后可修改设置。' : '';
  refreshControls(); $('#settings-dialog').showModal();
});
$('#settings-close').addEventListener('click', () => $('#settings-dialog').close());
$('#settings-form').addEventListener('submit', event => {
  event.preventDefault();
  if (feedingBusy()) return;
  const values = seconds.map((_, index) => Number($(`#seconds-${index}`).value));
  if (!values.every(value => validSeconds(value) && value <= 6)) { $('#settings-error').textContent = '请填写 0.2～6 秒之间的有效数值。'; return; }
  values.forEach((value, index) => {
    seconds[index] = value;
    $(`#feeder-${index + 1} .feed-duration`).textContent = `预计 ${(value * 5).toFixed(1)} 秒`;
  });
  $('#settings-dialog').close(); addActivity('下料时间设置已更新', '将用于真实下料，每路最多运行30秒，需实测校准'); toast('已保存本次下料时间设置');
});
$('#camera-expand').addEventListener('click', async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else if ($('#camera-scene').requestFullscreen) await $('#camera-scene').requestFullscreen();
    else toast('当前浏览器不支持全屏，可横屏查看画面');
  } catch { toast('当前浏览器无法进入全屏'); }
});
document.querySelectorAll('.nav-item').forEach(link => link.addEventListener('click', () => {
  document.querySelectorAll('.nav-item').forEach(item => {
    item.classList.toggle('active', item === link);
    if (item === link) item.setAttribute('aria-current', 'location');
    else item.removeAttribute('aria-current');
  });
}));

let environmentState = null;
let environmentExpiresAt = 0;
function renderEnvironment() {
  const fresh = environmentState && performance.now() < environmentExpiresAt;
  for (const [name, key] of [['temperature', 'temperatureC'], ['humidity', 'humidityPct']]) {
    $(`#${name}-value`).textContent = fresh ? environmentState[key].toFixed(1) : '—';
    $(`#${name}-tag`).textContent = fresh ? '实时' : '暂无数据';
    $(`#${name}-status`).textContent = fresh ? '传感器已连接 · 自动更新' : '未收到有效读数 · 正在重试';
  }
}
async function pollEnvironment() {
  if (!document.hidden) {
    try {
      const started = performance.now();
      const response = await fetch('/api/environment', {cache:'no-store', signal:AbortSignal.timeout(3000)});
      if (!response.ok) throw new Error('传感器未连接');
      const state = await response.json();
      if (state.ready !== true || !Number.isFinite(state.temperatureC) || !Number.isFinite(state.humidityPct) ||
          state.temperatureC < -50 || state.temperatureC > 150 || state.humidityPct < 0 || state.humidityPct > 100 ||
          !Number.isFinite(state.sampleAgeMs) || state.sampleAgeMs < 0 || state.sampleAgeMs >= 6000) throw new Error('读数不可用');
      environmentState = state;
      environmentExpiresAt = started + 6000 - state.sampleAgeMs;
    } catch { environmentState = null; }
    renderEnvironment();
  }
  setTimeout(pollEnvironment, 3000);
}
setInterval(renderEnvironment, 1000);
document.addEventListener('visibilitychange', renderEnvironment);
pollEnvironment();

initializeDeviceNetwork();
initializeCameraView({ toast, addActivity });
initializeWifiSettings({canChange:()=>!feedingBusy()});
