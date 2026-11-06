#pragma once
#include <Arduino.h>

/* ============================================================================
 *  手机端遥控界面（存在 Flash 里，不占 RAM）
 * ----------------------------------------------------------------------------
 *  ★ 电机是纯数字直驱（L298N 不接 EN，IN 只有高低电平）
 *    所以每个轮子只有 3 种状态：正转 / 反转 / 停。
 *    两轮组合出的所有动作一共 9 种，全部是差速车真实存在的行为：
 *
 *      左轮  右轮    车身运动                 界面名称
 *      ─────────────────────────────────────────────────
 *       +     +     直线前进                 前进
 *       +     0     绕右轮前滚（大半径右弯） 右前弧
 *       +     -     原地顺时针转             原地右转
 *       0     -     绕右轮后退（大半径右弯） 右后弧
 *       -     -     直线后退                 后退
 *       -     0     绕左轮后退（大半径左弯） 左后弧
 *       -     +     原地逆时针转             原地左转
 *       0     +     绕左轮前滚（大半径左弯） 左前弧
 *       0     0     停                       停
 *
 *    注意：差速车没有"横移"，上面 8 个方向里斜的都只是一轮驱动另一轮
 *    拖行的圆弧，不是平移。这就是为什么不能做成麦轮那种 8 向按键。
 *
 *  设计要点：
 *    · 整屏不滚动，避免操作摇杆时拖页
 *    · 摇杆按角度扇区量化，死区之外立即出动作
 * ========================================================================== */

const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="theme-color" content="#0c1424">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="format-detection" content="telephone=no">
<title>玻璃清洁机器人</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;-webkit-touch-callout:none}
:root{
  --c1:#1a2740;--c2:#141d2f;--line:#26344e;
  --txt:#eef3fa;--dim:#8ea3c2;--acc:#3d92ff;--ok:#22c97e;--bad:#ff4d4f;
}
html{-webkit-text-size-adjust:100%}
html,body{margin:0;height:100%;overscroll-behavior:none}
body{
  background:radial-gradient(680px 380px at 50% -6%,#1d3355,#0c1424 66%) fixed,#0c1424;
  color:var(--txt);font-size:15px;line-height:1.35;
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans SC",sans-serif;
  user-select:none;-webkit-user-select:none;touch-action:manipulation;
  height:100vh;height:100dvh;display:flex;flex-direction:column;overflow:hidden;
  padding:calc(env(safe-area-inset-top) + 6px)
          calc(env(safe-area-inset-right) + 10px)
          calc(env(safe-area-inset-bottom) + 8px)
          calc(env(safe-area-inset-left) + 10px);
}

/* ---------------------------------- 顶栏 ---------------------------------- */
.top{flex:none;display:flex;align-items:center;gap:9px;
  max-width:520px;width:100%;margin:0 auto 8px;padding:0 3px}
.logo{width:28px;height:28px;border-radius:8px;flex:none;
  background:linear-gradient(140deg,#3d92ff,#1b5cc0);
  display:grid;place-items:center;font-size:14px;
  box-shadow:0 3px 12px rgba(61,146,255,.4)}
.name{font-size:14.5px;font-weight:600;letter-spacing:.3px;flex:1;min-width:0;
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.pill{display:flex;align-items:center;gap:6px;flex:none;
  border:1px solid var(--line);border-radius:20px;padding:4px 10px;
  font-size:11.5px;color:var(--dim);background:rgba(20,29,47,.85)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--bad);flex:none;
  box-shadow:0 0 9px var(--bad);transition:.25s}
.dot.on{background:var(--ok);box-shadow:0 0 9px var(--ok)}

/* ---------------------------------- 卡片 ---------------------------------- */
main{flex:1 1 auto;min-height:0;display:flex;flex-direction:column;gap:8px;
  max-width:520px;width:100%;margin:0 auto}
.card{background:linear-gradient(180deg,var(--c1),var(--c2));
  border:1px solid var(--line);border-radius:16px;padding:12px;flex:none;
  box-shadow:0 8px 22px rgba(0,0,0,.32);
  display:flex;flex-direction:column;min-height:0}
.card.grow{flex:1 1 auto}
.hd{display:flex;align-items:center;justify-content:space-between;
  gap:8px;flex:none;margin-bottom:8px}
.hd h2{margin:0;font-size:11.5px;font-weight:600;letter-spacing:1.5px;color:var(--dim)}
.hd .val{font-size:11.5px;color:var(--dim);font-variant-numeric:tabular-nums;
  white-space:nowrap}
.hd .val b{font-size:13.5px;color:var(--acc);font-weight:600}
.hd .val b.run{color:var(--ok)}
.hd .val .to{color:var(--ok);font-size:11px;font-weight:600}

/* --------------------------------- 摇杆 ---------------------------------- */
#stickWrap{flex:1 1 auto;min-height:0;display:flex;align-items:center;
  justify-content:center;padding:2px 0 4px;overflow:hidden}
#stick{position:relative;border-radius:50%;touch-action:none;flex:none;
  background:radial-gradient(circle at 50% 40%,#1e2c48,#121a2b 74%);
  border:1px solid #2e3f5c;
  box-shadow:inset 0 0 36px rgba(0,0,0,.6),0 6px 20px rgba(0,0,0,.32)}
#stick .ax{position:absolute;background:#26354f;border-radius:2px;pointer-events:none}
#stick .ax.h{left:9%;right:9%;top:50%;height:1px;margin-top:-.5px}
#stick .ax.v{top:9%;bottom:9%;left:50%;width:1px;margin-left:-.5px}
/* 45 度辅助线：标出 8 个扇区的分界 */
#stick .ax.d1,#stick .ax.d2{left:50%;top:50%;width:76%;height:1px;
  margin-left:-38%;transform-origin:50% 50%;opacity:.45}
#stick .ax.d1{transform:rotate(45deg)}
#stick .ax.d2{transform:rotate(-45deg)}
#stick .ring{position:absolute;left:50%;top:50%;width:56%;height:56%;
  margin:-28% 0 0 -28%;border-radius:50%;border:1px dashed #2b3c58;
  pointer-events:none;opacity:.55}
#stick .tag{position:absolute;font-size:10px;color:#8199ba;letter-spacing:1px;
  pointer-events:none;white-space:nowrap}
#stick .tag.t{top:5%;left:50%;transform:translateX(-50%)}
#stick .tag.b{bottom:5%;left:50%;transform:translateX(-50%)}
#stick .tag.l{left:6%;top:50%;transform:translateY(-50%)}
#stick .tag.r{right:6%;top:50%;transform:translateY(-50%)}
#knob{position:absolute;left:50%;top:50%;border-radius:50%;pointer-events:none;
  transform:translate(-50%,-50%);
  background:radial-gradient(circle at 37% 31%,#7dbaff,#2f7fdc 56%,#18539b);
  border:2px solid #9ccaff;
  box-shadow:0 6px 18px rgba(16,80,170,.6),0 0 24px rgba(61,146,255,.4)}

/* --------------------------------- 按钮 ---------------------------------- */
.estop{flex:none;width:100%;height:52px;margin-top:8px;border-radius:13px;
  cursor:pointer;border:1px solid #933236;
  background:linear-gradient(180deg,#cf3236,#8f1e21);
  color:#fff;font-size:17px;font-weight:700;letter-spacing:5px;text-indent:5px;
  transition:transform .08s,background .12s}
.estop:active{transform:scale(.975);background:linear-gradient(180deg,#ff5a5c,#b8262a)}

.grid2{flex:none;display:grid;grid-template-columns:1fr 1fr;gap:8px}
.op{height:52px;border-radius:13px;border:1px solid #2c3c58;cursor:pointer;
  background:linear-gradient(180deg,#22314e,#182338);color:var(--txt);
  font-size:15.5px;font-weight:600;touch-action:none;padding:0 6px;
  display:flex;align-items:center;justify-content:center;gap:6px;
  transition:transform .07s,background .12s,border-color .12s}
.op:active{transform:scale(.96)}
.op.act{background:linear-gradient(180deg,#3d92ff,#1e63c8);border-color:#6fb2ff;
  box-shadow:0 0 16px rgba(61,146,255,.45)}
.op.on{background:linear-gradient(180deg,#22c97e,#139a5c);border-color:#43e39b;
  color:#04170e;box-shadow:0 0 16px rgba(34,201,126,.45)}
.op.wide{grid-column:1 / -1}
.op .k{font-size:11px;opacity:.7;font-weight:500}

/* --------------------------------- 滑杆 ---------------------------------- */
.sld{flex:none;display:flex;align-items:center;gap:10px;margin-top:9px}
.sld .ic{font-size:14px;color:var(--dim);flex:none;width:15px;text-align:center}
input[type=range]{-webkit-appearance:none;appearance:none;flex:1;height:32px;
  background:transparent;margin:0;touch-action:pan-y}
input[type=range]::-webkit-slider-runnable-track{height:8px;border-radius:5px;
  background:#23324c}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:28px;height:28px;
  margin-top:-10px;border-radius:50%;background:var(--acc);border:0;
  box-shadow:0 0 12px rgba(61,146,255,.75)}
input[type=range]::-moz-range-track{height:8px;border-radius:5px;background:#23324c}
input[type=range]::-moz-range-thumb{width:26px;height:26px;border:0;border-radius:50%;
  background:var(--acc)}

/* --------------------------------- 底栏 ---------------------------------- */
.foot{flex:none;max-width:520px;width:100%;margin:8px auto 0;padding:0 3px;
  font-size:11px;color:var(--dim)}
.chips{display:flex;gap:5px;flex-wrap:wrap;margin-bottom:5px}
.chip{border:1px solid var(--line);border-radius:7px;padding:2px 7px;
  font-size:10.5px;color:var(--dim);background:rgba(20,29,47,.6);
  white-space:nowrap;font-variant-numeric:tabular-nums}
.chip.act{border-color:#43e39b;color:#43e39b}
.chip.warn{border-color:#ff6b6d;color:#ff9a9b}
.finfo{display:flex;align-items:center;gap:8px;font-size:10.5px;
  font-variant-numeric:tabular-nums}
.finfo span{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.reset{flex:none;background:none;border:1px solid var(--line);color:var(--dim);
  border-radius:7px;padding:3px 9px;font-size:10.5px;cursor:pointer}
.reset.arm{border-color:#ff6b6d;color:#ff9a9b}

/* ------------------------------ 自适应压缩 ------------------------------ */
@media (max-height:760px){
  .estop{height:46px;font-size:16px}
  .op{height:46px;font-size:15px}
  .hd{margin-bottom:6px}
  .sld{margin-top:7px}
}
/* 横屏：左右双列，摇杆在左，一屏看全 */
@media (orientation:landscape) and (max-height:560px){
  .top{max-width:780px;margin-bottom:8px}
  .name{font-size:13.5px}
  .logo{width:26px;height:26px;font-size:13px}
  main{max-width:780px;display:grid;grid-template-columns:1.05fr 1fr;
    grid-template-rows:1fr 1fr;gap:8px}
  .card.grow{grid-column:1;grid-row:1 / span 2}
  .card:nth-of-type(2){grid-column:2;grid-row:1}
  .card:nth-of-type(3){grid-column:2;grid-row:2}
  .hd{margin-bottom:6px}
  .sld{margin-top:6px}
  .estop{height:44px;letter-spacing:4px;margin-top:7px}
  .op{height:46px}
  .foot{display:none}
  #stick .tag{font-size:9px;opacity:.85}
}
</style>
</head>
<body>

<div class="top">
  <div class="logo">🪟</div>
  <div class="name">跨障碍擦玻璃机器人</div>
  <div class="pill"><span class="dot" id="dot"></span><span id="conn">连接中</span></div>
</div>

<main>

  <!-- ========================== 行走（差速） ========================== -->
  <section class="card grow">
    <div class="hd">
      <h2>行走 · 差速</h2>
      <span class="val">左 <b id="lVal">停</b> · 右 <b id="rVal">停</b></span>
    </div>

    <div id="stickWrap">
      <div id="stick">
        <div class="ax h"></div>
        <div class="ax v"></div>
        <div class="ax d1"></div>
        <div class="ax d2"></div>
        <div class="ring"></div>
        <span class="tag t">前</span>
        <span class="tag b">后</span>
        <span class="tag l">左转</span>
        <span class="tag r">右转</span>
        <div id="knob"></div>
      </div>
    </div>

    <button class="estop" id="estop">急停</button>
  </section>

  <!-- ============================ 动作 ============================ -->
  <section class="card">
    <div class="hd">
      <h2>推杆 · 舵机</h2>
      <span class="val">舵机 <b id="svVal">90</b>°<span class="to" id="svTo"></span></span>
    </div>

    <div class="grid2">
      <button class="op" id="actOut">▲ 伸出</button>
      <button class="op" id="actIn">▼ 收回</button>
    </div>

    <div class="sld">
      <span class="ic">⟲</span>
      <input id="servo" type="range" min="0" max="180" step="5" value="90" aria-label="舵机角度">
    </div>
  </section>

  <!-- ============================ 开关 ============================ -->
  <section class="card">
    <div class="hd"><h2>真空泵 · 继电器</h2></div>
    <div class="grid2">
      <button class="op wide" id="pump">真空泵 <span class="k" id="pumpSt">关</span></button>
      <button class="op toggle" id="r1" data-n="1">继电器1</button>
      <button class="op toggle" id="r2" data-n="2">继电器2</button>
    </div>
  </section>

</main>

<footer class="foot">
  <div class="chips">
    <span class="chip" id="cMove">状态 停</span>
    <span class="chip" id="cAct">推杆 停</span>
    <span class="chip" id="cCli">设备 0</span>
  </div>
  <div class="finfo">
    <span id="fInfo">--</span>
    <button class="reset" id="reboot">重启</button>
  </div>
</footer>

<script>
(function(){
"use strict";

var $ = function(s){ return document.querySelector(s); };
var SECT = [
  {n:'前进',     l: 1, r: 1},   // 角度   0°
  {n:'右前弧',   l: 1, r: 0},   // 角度  45°
  {n:'原地右转', l: 1, r:-1},   // 角度  90°
  {n:'右后弧',   l: 0, r:-1},   // 角度 135°
  {n:'后退',     l:-1, r:-1},   // 角度 180°
  {n:'左后弧',   l:-1, r: 0},   // 角度-135°
  {n:'原地左转', l:-1, r: 1},   // 角度 -90°
  {n:'左前弧',   l: 0, r: 1}    // 角度 -45°
];
var DEAD = 0.30;               // 摇杆中心死区（占行程比例）

var driveHB = null, actHB = null, svTimer = null, svBusy = false, armed = false;
var st = {pump:0, r1:0, r2:0};

function api(u){ return fetch(u, {cache:'no-store'}).catch(function(){}); }
function buzz(ms){ try { if (navigator.vibrate) navigator.vibrate(ms); } catch(e){} }

function keepAwake(){
  try {
    if ('wakeLock' in navigator) navigator.wakeLock.request('screen').catch(function(){});
  } catch(e){}
}

/* ============================================================
 *  摇杆 -> 轮子状态
 *  先把摇杆位移换算成极坐标，再按 45° 扇区量化成 8 个方向。
 *  这么做的原因：轮子只有 正/停/反 三种状态，摇杆的模拟量
 *  没有任何地方可以体现，只能落到这 8 个离散动作上。
 * ============================================================ */
var SK = {el:null, wrap:null, knob:null, R:0, cx:0, cy:0,
          active:false, pid:null, kx:0, ky:0, idx:-1};

function stickLayout(){
  if (!SK.el) return;
  var w = SK.wrap.clientWidth, h = SK.wrap.clientHeight;
  if (w < 40 || h < 40) return;

  var size = Math.floor(Math.min(w, h)) - 6;
  size = Math.max(110, Math.min(300, size));

  SK.el.style.width  = size + 'px';
  SK.el.style.height = size + 'px';

  var k = Math.round(Math.max(46, Math.min(74, size * 0.30)));
  SK.knob.style.width  = k + 'px';
  SK.knob.style.height = k + 'px';

  SK.R = size / 2 - k / 2 - 3;
  stickPaint(SK.kx || 0, SK.ky || 0, false);
}

function stickLayoutSoon(){
  requestAnimationFrame(function(){ requestAnimationFrame(stickLayout); });
}

function stickPaint(dx, dy, animate){
  SK.knob.style.transition = animate
    ? 'transform .14s cubic-bezier(.2,.85,.3,1)' : 'none';
  SK.knob.style.transform =
    'translate(calc(-50% + ' + dx.toFixed(1) + 'px), calc(-50% + ' + dy.toFixed(1) + 'px))';
}

/** 摇杆位置 -> {l, r, i}：l/r 为 -1/0/1，i 为扇区序号（-1 表示停） */
function mixWheels(){
  if (!SK.active) return {l:0, r:0, i:-1, n:'停'};

  var v = -SK.ky / SK.R;      // 屏幕 y 向下，取反 -> 前为正
  var w =  SK.kx / SK.R;      // 右为正
  var len = Math.sqrt(v * v + w * w);

  if (len < DEAD) return {l:0, r:0, i:-1, n:'停'};

  if (len > 1) { v /= len; w /= len; }

  var deg = Math.atan2(w, v) * 180 / Math.PI;      // -180 ~ 180
  var i = ((Math.round(deg / 45) % 8) + 8) % 8;
  var s = SECT[i];
  return {l:s.l, r:s.r, i:i, n:s.n};
}

function wheelText(d){ return d > 0 ? '前' : (d < 0 ? '后' : '停'); }

function paintMove(d){
  $('#lVal').textContent = wheelText(d.l);
  $('#rVal').textContent = wheelText(d.r);
  $('#lVal').classList.toggle('run', d.l !== 0);
  $('#rVal').classList.toggle('run', d.r !== 0);
  $('#cMove').textContent = '状态 ' + d.n;
  $('#cMove').classList.toggle('act', d.i >= 0);
}

function sendDrive(){
  var d = mixWheels();
  api('/api/drive?l=' + d.l + '&r=' + d.r);
  paintMove(d);
  if (d.i !== SK.idx){          // 只有跨扇区才震动，避免连续嗡嗡
    SK.idx = d.i;
    if (d.i >= 0) buzz(10);
  }
}

function driveStart(){
  clearInterval(driveHB);
  driveHB = setInterval(sendDrive, 150);   // 心跳，喂主板看门狗
  sendDrive();
}

function driveStop(){
  clearInterval(driveHB); driveHB = null;
  SK.idx = -1;
  api('/api/drive?l=0&r=0');
  paintMove({l:0, r:0, i:-1, n:'停'});
}

function stickDown(e){
  e.preventDefault();
  var r = SK.el.getBoundingClientRect();
  SK.cx = r.left + r.width / 2;
  SK.cy = r.top  + r.height / 2;

  var kw = SK.knob.getBoundingClientRect().width;
  SK.R  = Math.max(24, r.width / 2 - kw / 2 - 3);   // 半径实时读，防旋屏失效

  SK.active = true;
  SK.pid = e.pointerId;
  try { SK.el.setPointerCapture(e.pointerId); } catch(_){}
  stickMove(e);
  driveStart();
  buzz(10);
}

function stickMove(e){
  if (!SK.active || e.pointerId !== SK.pid) return;
  e.preventDefault();

  var dx = e.clientX - SK.cx;
  var dy = e.clientY - SK.cy;

  var len = Math.sqrt(dx * dx + dy * dy);
  if (len > SK.R) { var k = SK.R / len; dx *= k; dy *= k; }

  SK.kx = dx; SK.ky = dy;
  stickPaint(dx, dy, false);
  sendDrive();                       // 拖动立即响应，不等心跳
}

function stickUp(e){
  if (!SK.active) return;
  if (e && SK.pid !== null && e.pointerId !== SK.pid) return;
  SK.active = false;
  SK.pid = null;
  SK.kx = SK.ky = 0;
  stickPaint(0, 0, true);            // 回中有动画
  driveStop();
}

/* ------------------------------------------------------- 推杆 / 舵机 / 开关 */
function paintAct(v){
  $('#actOut').classList.toggle('act', v > 0);
  $('#actIn').classList.toggle('act', v < 0);
}

function actStart(v){
  api('/api/act?v=' + v);
  clearInterval(actHB);
  actHB = setInterval(function(){ api('/api/act?v=' + v); }, 250);
  paintAct(v);
  buzz(12);
}

function actStop(){
  clearInterval(actHB); actHB = null;
  api('/api/act?v=0');
  paintAct(0);
}

function setPumpUI(on){
  $('#pump').classList.toggle('on', !!on);
  $('#pumpSt').textContent = on ? '开' : '关';
}
function setRelayUI(id, on){
  var el = $('#r' + id);
  if (el) el.classList.toggle('on', !!on);
}

/* ------------------------------------------------------- 初始化 */
function init(){
  SK.el   = $('#stick');
  SK.wrap = $('#stickWrap');
  SK.knob = $('#knob');

  SK.el.addEventListener('pointerdown', stickDown);
  SK.el.addEventListener('pointermove', stickMove);
  SK.el.addEventListener('pointerup', stickUp);
  SK.el.addEventListener('pointercancel', stickUp);
  SK.el.addEventListener('lostpointercapture', stickUp);
  SK.el.addEventListener('contextmenu', function(e){ e.preventDefault(); });

  window.addEventListener('pointerup', function(){ if (SK.active) stickUp(); });

  stickLayout();
  if (window.ResizeObserver){
    new ResizeObserver(stickLayout).observe(SK.wrap);
    new ResizeObserver(stickLayout).observe(document.querySelector('main'));
  }
  window.addEventListener('resize', stickLayoutSoon);
  window.addEventListener('orientationchange', function(){
    stickLayoutSoon();
    setTimeout(stickLayout, 150);
    setTimeout(stickLayout, 500);
  });
  if (window.visualViewport){
    window.visualViewport.addEventListener('resize', stickLayoutSoon);
  }
  stickLayoutSoon();

  /* 急停 */
  $('#estop').addEventListener('click', function(){
    if (SK.active) stickUp();
    else driveStop();
    api('/api/estop');
    actStop();
    buzz([25, 45, 25]);
  });

  /* 推杆 */
  [['#actOut', 1], ['#actIn', -1]].forEach(function(p){
    var el = $(p[0]), v = p[1];
    el.addEventListener('pointerdown', function(e){
      e.preventDefault();
      try { el.setPointerCapture(e.pointerId); } catch(_){}
      actStart(v);
    });
    el.addEventListener('pointerup', actStop);
    el.addEventListener('pointercancel', actStop);
    el.addEventListener('lostpointercapture', actStop);
  });
  window.addEventListener('pointerup', function(){ if (actHB) actStop(); });

  /* 舵机（全板唯一的 PWM） */
  $('#servo').addEventListener('pointerdown', function(){ svBusy = true; });
  $('#servo').addEventListener('input', function(e){
    svBusy = true;
    $('#svVal').textContent = e.target.value;
    var v = parseInt(e.target.value, 10);
    clearTimeout(svTimer);
    svTimer = setTimeout(function(){
      api('/api/servo?a=' + v);
      svBusy = false;
    }, 100);
  });

  /* 开关量 */
  $('#pump').addEventListener('click', function(){
    var on = st.pump ? 0 : 1;
    st.pump = on; setPumpUI(on);
    api('/api/pump?on=' + on);
    buzz(18);
  });

  Array.prototype.forEach.call(document.querySelectorAll('.toggle'), function(el){
    el.addEventListener('click', function(){
      var id = parseInt(el.dataset.n, 10);
      var key = 'r' + id;
      var on = st[key] ? 0 : 1;
      st[key] = on;
      setRelayUI(id, on);
      api('/api/relay?id=' + id + '&on=' + on);
      buzz(18);
    });
  });

  /* 重启（两次确认） */
  $('#reboot').addEventListener('click', function(){
    var b = $('#reboot');
    if (!armed){
      armed = true;
      b.classList.add('arm');
      b.textContent = '再点确认';
      buzz(20);
      setTimeout(function(){
        if (armed){ armed = false; b.classList.remove('arm'); b.textContent = '重启'; }
      }, 4000);
    } else {
      armed = false;
      b.textContent = '重启中…';
      api('/api/reboot');
    }
  });

  document.addEventListener('visibilitychange', function(){
    if (document.visibilityState === 'visible') { keepAwake(); }
    else {                                    // 切后台立刻停车
      if (SK.active) stickUp();
      if (actHB) actStop();
    }
  });
  window.addEventListener('blur', function(){ if (SK.active) stickUp(); });

  document.addEventListener('pointerdown', function once(){
    keepAwake();
    buzz(6);
    document.removeEventListener('pointerdown', once);
  });

  keepAwake();
  poll();
}

/* ------------------------------------------------------- 状态轮询 */
function render(j){
  $('#cAct').textContent = j.blk ? '推杆 已锁定'
    : '推杆 ' + (j.act > 0 ? '伸出' : j.act < 0 ? '收回' : '停');
  $('#cAct').classList.toggle('act', j.act !== 0);
  $('#cAct').classList.toggle('warn', !!j.blk);

  $('#cCli').textContent = '设备 ' + (j.cli || 0);

  setPumpUI(j.pump);
  setRelayUI(1, j.r1);
  setRelayUI(2, j.r2);

  // 本机没操作摇杆时，用主板回报的轮子状态刷新（多台手机同时连也不打架）
  if (!SK.active && typeof j.l === 'number' && typeof j.r === 'number'){
    var nm = '停';
    for (var i = 0; i < SECT.length; i++){
      if (SECT[i].l === j.l && SECT[i].r === j.r){ nm = SECT[i].n; break; }
    }
    paintMove({l:j.l, r:j.r, i:(j.l || j.r) ? 0 : -1, n:nm});
  }

  if (typeof j.servo === 'number'){
    // 标题显示"当前实际角度"，滑杆显示"目标角度"
    $('#svVal').textContent = j.servo;
    if (!svBusy && typeof j.svT === 'number') $('#servo').value = j.svT;
    if (j.svMove && typeof j.svT === 'number' && j.svT !== j.servo){
      $('#svTo').textContent = ' → ' + j.svT;
    } else {
      $('#svTo').textContent = '';
    }
  }
}

function poll(){
  var moving = false;
  fetch('/api/status', {cache:'no-store'})
    .then(function(r){ return r.json(); })
    .then(function(j){
      st = j;
      moving = !!j.svMove;
      $('#dot').classList.add('on');
      $('#conn').textContent = '在线';
      $('#fInfo').textContent = (j.ssid || '--') + ' · ' + (j.ip || '192.168.4.1') +
        ' · ' + Math.floor((j.up || 0) / 60) + '分 · ' +
        Math.floor((j.heap || 0) / 1024) + 'K';
      render(j);
    })
    .catch(function(){
      $('#dot').classList.remove('on');
      $('#conn').textContent = '断开';
      if (SK.active) stickUp();      // 断线必须回中停车
    })
    .then(function(){ setTimeout(poll, moving ? 350 : 1200); });  // 舵机转动时加快刷新
}

if (document.readyState === 'loading'){
  document.addEventListener('DOMContentLoaded', init);
} else {
  init();
}

})();
</script>
</body>
</html>
)HTML";
