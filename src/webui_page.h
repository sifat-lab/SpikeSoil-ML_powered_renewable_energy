#pragma once

// The dashboard, served from flash as one self-contained document.
//
// No CDN, no external fonts, no remote anything: the node is its own access
// point and a phone joined to it has no route to the internet. The chart is
// hand-drawn on a canvas for the same reason.
//
// Kept in its own header purely so webui.cpp stays readable.

#include <pgmspace.h>

static const char kIndexHtml[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>SpikeSoil</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#0b0f14; --card:#141b24; --line:#243040; --tx:#e6edf3; --dim:#8b9bb0;
  --ok:#3fb950; --warn:#d29922; --bad:#f85149; --acc:#58a6ff;
}
body{background:var(--bg);color:var(--tx);font:15px/1.45 ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  padding:12px;max-width:760px;margin:0 auto;-webkit-text-size-adjust:100%}
header{display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;margin-bottom:12px}
h1{font-size:17px;letter-spacing:.5px}
h1 b{color:var(--acc)}
.meta{font-size:12px;color:var(--dim);margin-left:auto;text-align:right}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
h2{font-size:12px;font-weight:600;letter-spacing:.9px;text-transform:uppercase;color:var(--dim);margin-bottom:10px}

/* a. the loss, large */
.loss{display:flex;align-items:baseline;gap:12px;flex-wrap:wrap}
.big{font-size:64px;font-weight:700;line-height:1;font-variant-numeric:tabular-nums;letter-spacing:-2px}
.big.stale{color:var(--warn)}
.big.none{color:var(--dim);font-size:34px;letter-spacing:0}
.sub{font-size:13px;color:var(--dim);font-variant-numeric:tabular-nums}

/* b. validity */
.state{margin-top:12px;display:flex;align-items:center;gap:8px;font-size:13px;flex-wrap:wrap}
.dot{width:9px;height:9px;border-radius:50%;flex:0 0 auto}
.s-ok .dot{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.s-warn .dot{background:var(--warn)}
.s-bad .dot{background:var(--bad)}
.s-ok{color:var(--ok)} .s-warn{color:var(--warn)} .s-bad{color:var(--bad)}
.age{color:var(--dim)}

/* c. charts */
canvas{width:100%;display:block;touch-action:pan-y}
#chartLoss{height:190px}
#chartCtx{height:130px}
.sect{font-size:11px;color:var(--dim);letter-spacing:.5px;margin:14px 0 2px}
.sect:first-of-type{margin-top:0}
.legend{display:flex;gap:14px;flex-wrap:wrap;font-size:12px;color:var(--dim);margin-top:6px}
.legend label{display:flex;align-items:center;gap:5px;cursor:pointer;user-select:none}
.swatch{width:14px;height:3px;border-radius:2px;display:inline-block}

/* readouts */
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(94px,1fr));gap:10px}
.cell .k{font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.5px}
.cell .v{font-size:17px;font-variant-numeric:tabular-nums;margin-top:2px}
.cell .u{font-size:11px;color:var(--dim)}

/* d. payback */
.io{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-bottom:14px}
.io label{display:block;font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.5px;margin-bottom:4px}
input[type=number]{width:100%;background:#0b1119;border:1px solid var(--line);color:var(--tx);
  border-radius:8px;padding:8px 10px;font:inherit;font-variant-numeric:tabular-nums}
input[type=number]:focus{outline:none;border-color:var(--acc)}
.live-row{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--dim);margin-top:6px}
.chain{border-top:1px solid var(--line);padding-top:12px;font-size:13px}
.chain div{display:flex;justify-content:space-between;gap:10px;padding:3px 0;font-variant-numeric:tabular-nums}
.chain .lbl{color:var(--dim)}
.pay{margin-top:10px;padding-top:10px;border-top:1px solid var(--line);display:flex;align-items:baseline;gap:10px}
.pay .n{font-size:36px;font-weight:700;font-variant-numeric:tabular-nums;color:var(--ok)}
.pay .n.never{color:var(--dim);font-size:20px}
.note{font-size:11px;color:var(--dim);margin-top:10px;line-height:1.5}
.warnbar{background:#3a2a08;border:1px solid #6b4c0c;color:#f0c674;font-size:12px;
  padding:8px 10px;border-radius:8px;margin-bottom:12px;display:none}
</style>
</head>
<body>

<header>
  <h1><b>Spike</b>Soil</h1>
  <div class="meta"><span id="conn">connecting</span><br><span id="clock">--</span></div>
</header>

<div class="warnbar" id="warnbar"></div>

<div class="card">
  <div class="loss">
    <div class="big none" id="big">--</div>
    <div class="sub" id="losssub"></div>
  </div>
  <div class="state s-warn" id="state"><span class="dot"></span><span id="statetx">waiting for node</span></div>
</div>

<div class="card">
  <h2>Last 5 minutes</h2>

  <div class="sect">SOILING LOSS</div>
  <canvas id="chartLoss"></canvas>
  <div class="legend">
    <label><input type="checkbox" id="cLoss" checked><span class="swatch" style="background:#58a6ff"></span>soiling loss</label>
  </div>
  <div class="note" id="lossnote"></div>

  <div class="sect">CONTEXT</div>
  <canvas id="chartCtx"></canvas>
  <div class="legend">
    <label><input type="checkbox" id="cLux" checked><span class="swatch" style="background:#d29922"></span>lux &middot; left axis</label>
    <label><input type="checkbox" id="cIb" checked><span class="swatch" style="background:#3fb950"></span>panel B current &middot; right axis</label>
  </div>
  <div class="note" id="ctxnote"></div>
</div>

<div class="card">
  <h2>Live sensors</h2>
  <div class="grid">
    <div class="cell"><div class="k">Lux</div><div class="v" id="v_lux">--</div></div>
    <div class="cell"><div class="k">B current</div><div class="v" id="v_ib">--<span class="u"> mA</span></div></div>
    <div class="cell"><div class="k">B voltage</div><div class="v" id="v_vb">--<span class="u"> V</span></div></div>
    <div class="cell"><div class="k">B power</div><div class="v" id="v_pb">--<span class="u"> mW</span></div></div>
    <div class="cell"><div class="k">A current<span class="u"> *</span></div><div class="v" id="v_ia">--<span class="u"> mA</span></div></div>
  </div>
  <div class="note">* Panel A is the clean reference channel. It is the most recent 250 ms sample, not a 1 s mean like the model inputs.</div>
</div>

<div class="card">
  <h2>Payback</h2>
  <div class="io">
    <div><label for="arrayWp">Whole array rating (Wp)</label><input type="number" id="arrayWp" min="0" step="100"></div>
    <div><label for="sunHours">Peak sun hours (h/day)</label><input type="number" id="sunHours" min="0" step="0.1"></div>
    <div><label for="tariff">Tariff (BDT/kWh)</label><input type="number" id="tariff" min="0" step="0.25"></div>
    <div><label for="cleanCost">Cleaning cost (BDT)</label><input type="number" id="cleanCost" min="0" step="10"></div>
    <div>
      <label for="lossIn">Soiling loss (0-1)</label>
      <input type="number" id="lossIn" min="0" max="1" step="0.01">
      <div class="live-row"><input type="checkbox" id="lossLive" checked><label for="lossLive" style="margin:0;text-transform:none;letter-spacing:0">follow live estimate</label></div>
    </div>
  </div>
  <div class="chain">
    <div><span class="lbl">Clean yield = Wp/1000 x sun hours</span><span id="p_clean">--</span></div>
    <div><span class="lbl">Lost to soiling = clean x loss</span><span id="p_lost">--</span></div>
    <div><span class="lbl">Recovered value = lost x tariff</span><span id="p_value">--</span></div>
  </div>
  <div class="pay"><div class="n" id="p_days">--</div><div class="sub" id="p_daysub">days to pay back one cleaning</div></div>
  <div class="note">Figures scale linearly with array size; the test panel here is 100 Wp.</div>
  <div class="note">Assumes a wash restores the panel fully and that today's loss is representative. Tariff and cleaning cost are stored in the node's RAM, so every phone on the AP sees the same numbers; they are lost on reset and nothing is written to the SD card.</div>
</div>

<div class="card">
  <h2>Node</h2>
  <div class="grid">
    <div class="cell"><div class="k">Spike rate</div><div class="v" id="v_rate">--</div></div>
    <div class="cell"><div class="k">MACs</div><div class="v" id="v_macs">--</div></div>
    <div class="cell"><div class="k">Latency</div><div class="v" id="v_lat">--<span class="u"> ms</span></div></div>
    <div class="cell"><div class="k">Step period</div><div class="v" id="v_step">--</div></div>
    <div class="cell"><div class="k">Late ticks</div><div class="v" id="v_late">--</div></div>
    <div class="cell"><div class="k">Free heap</div><div class="v" id="v_heap">--<span class="u"> kB</span></div></div>
  </div>
  <div class="note" id="timingnote"></div>
</div>

<script>
const $=i=>document.getElementById(i);
const F=['arrayWp','sunHours','tariff','cleanCost'];
let live=null, hist={now:0,rows:[]}, cfg=null, editing=null, missed=0;

const num=(v,d)=>v==null?'--':v.toFixed(d);
const clamp01=v=>v<0?0:(v>1?1:v);

/* ---- polling ---------------------------------------------------------- */
async function pollState(){
  try{
    const r=await fetch('/api/state',{cache:'no-store'});
    live=await r.json(); missed=0;
    $('conn').textContent='live';
    $('conn').style.color='var(--ok)';
    if(cfg===null){cfg=live.config;fillInputs();}
    render();
  }catch(e){
    if(++missed>2){$('conn').textContent='disconnected';$('conn').style.color='var(--bad)';}
  }
}
async function pollHistory(){
  try{
    const r=await fetch('/api/history',{cache:'no-store'});
    hist=await r.json(); draw();
  }catch(e){}
}

/* ---- a + b: the number and why to trust it ---------------------------- */
function render(){
  if(!live) return;
  const s=$('state'), big=$('big');

  $('clock').textContent=(live.timeSynced?'clock synced':'uptime')+' '+fmtDur(live.uptimeS)
    +' · '+live.clients+' client'+(live.clients==1?'':'s');

  let shown=null, stale=false;
  if(live.valid && live.lossDisplay!=null){ shown=live.lossDisplay; }
  else if(live.lastValidLoss!=null){ shown=clamp01(live.lastValidLoss); stale=true; }

  if(shown==null){
    big.className='big none';
    big.textContent=live.ready?'no reading yet':'--';
    $('losssub').textContent='';
  }else{
    big.className='big'+(stale?' stale':'');
    big.textContent=(shown*100).toFixed(1)+'%';
    const raw=stale?live.lastValidLoss:live.loss;
    let sub='loss '+shown.toFixed(3);
    if(raw!=null && Math.abs(raw-shown)>1e-4) sub+=' (model said '+raw.toFixed(3)+', clamped)';
    const ema=stale?live.lastValidEma:live.lossEma;
    if(ema!=null) sub+=' · smoothed '+clamp01(ema).toFixed(3);
    $('losssub').textContent=sub;
  }

  // Never blank the screen while gated: show the last valid number and its age.
  let cls='s-ok', tx='live estimate, updating every second';
  if(!live.ready){ cls='s-warn'; tx='node starting up'; }
  else if(live.valid){ cls='s-ok'; tx='live · '+live.state; }
  else{
    cls=(live.state.indexOf('fault')>=0||live.state.indexOf('self-test')>=0)?'s-bad':'s-warn';
    tx='gated: '+live.state;
    if(live.secondsSinceValid!=null){
      tx+=' — showing last valid '+clamp01(live.lastValidLoss).toFixed(2)
        +', <span class="age">'+fmtDur(live.secondsSinceValid)+' ago</span>';
    }else{
      tx+=' — no valid estimate yet this session';
    }
  }
  s.className='state '+cls;
  $('statetx').innerHTML=tx;

  $('warnbar').style.display=(live.state=='kernel self-test failed')?'block':'none';
  $('warnbar').textContent='Kernel self-test failed at boot. Inference is disabled; nothing on this page is a model output.';

  $('v_lux').textContent=num(live.lux,0);
  $('v_ib').innerHTML=num(live.iB_mA,1)+'<span class="u"> mA</span>';
  $('v_vb').innerHTML=num(live.vB,2)+'<span class="u"> V</span>';
  $('v_pb').innerHTML=num(live.pB_mW,0)+'<span class="u"> mW</span>';
  $('v_ia').innerHTML=num(live.iA_mA,1)+'<span class="u"> mA</span>';
  $('v_rate').textContent=num(live.spikeRate,3);
  $('v_macs').textContent=live.macs.toLocaleString();
  $('v_lat').innerHTML=(live.latencyUs/1000).toFixed(2)+'<span class="u"> ms</span>';

  const t=live.timing;
  $('v_step').innerHTML=t.steps>1?(t.minStepMs+'-'+t.maxStepMs+'<span class="u"> ms</span>'):'--';
  $('v_late').textContent=t.lateTicks+' / '+t.ticks;
  $('v_heap').innerHTML=(live.heap/1024).toFixed(0)+'<span class="u"> kB</span>';
  $('timingnote').textContent='Sub-sample tick mean lateness '+t.meanLateMs.toFixed(2)
    +' ms, worst '+t.maxLateMs+' ms, '+t.resyncs+' cadence resyncs. '
    +'A late tick is one that fired more than 50 ms after its slot.';

  if($('lossLive').checked && editing!='lossIn'){
    $('lossIn').value=shown==null?'':shown.toFixed(3);
  }
  payback();
}

function fmtDur(s){
  if(s==null) return '--';
  if(s<60) return s+' s';
  if(s<3600) return Math.floor(s/60)+' m '+(s%60)+' s';
  return Math.floor(s/3600)+' h '+Math.floor((s%3600)/60)+' m';
}

/* ---- c: charts -------------------------------------------------------- */
/* Two plots, not one. A single "% of full scale" axis had to carry 0.36 loss,
   14 000 lux and 3 mA at once, and none of the three was readable on it.

   Top, loss alone, fixed 0-1: the loss fraction is the claim this node is
   making, so 0.36 has to draw at 0.36 whatever else the window contains. It is
   also the series with gaps -- gated seconds are null and stay null -- and an
   auto-range would rescale the whole plot every time the gate reopened.

   Bottom, context, one axis each: auto-ranged over the visible window so real
   movement in a narrow band is visible, but never narrower than MINSPAN. That
   floor is the point. Auto-ranging to a steady panel's own min/max made
   B current wander over the full plot height on 0.2 mA of ADC noise and read
   as an unstable instrument. The endpoints are printed on both axes and again
   under the chart, so a flat trace can always be checked against its scale. */
const MINSPAN={lux:2000,ib:50};
const AX={L:46,R:54,T:14,B:18};
const f0=v=>Math.round(v).toLocaleString();
const f1=v=>v.toFixed(1);

function prep(id){
  const c=$(id), dpr=window.devicePixelRatio||1;
  const W=c.clientWidth, H=c.clientHeight;
  if(!W) return null;
  c.width=W*dpr; c.height=H*dpr;
  const g=c.getContext('2d'); g.scale(dpr,dpr);
  g.clearRect(0,0,W,H);
  g.font='10px ui-sans-serif,system-ui,sans-serif'; g.lineWidth=1;
  return {g,W,H,pw:W-AX.L-AX.R,ph:H-AX.T-AX.B};
}

/* Both charts share one time window, so the two plots line up column for
   column and the reader can drop a finger down from a loss step to the lux
   that caused it. */
function win(rows){
  const tMax=hist.now, tMin=Math.min(rows[0][0], tMax-60);
  return {tMin,tMax,span:Math.max(1,tMax-tMin)};
}

function gridlines(p,n){
  p.g.strokeStyle='#243040';
  for(let i=0;i<=n;i++){
    const y=AX.T+p.ph*i/n;
    p.g.beginPath(); p.g.moveTo(AX.L,y); p.g.lineTo(AX.L+p.pw,y); p.g.stroke();
  }
}

function noData(p,id){
  p.g.fillStyle='#8b9bb0'; p.g.textAlign='center'; p.g.textBaseline='middle';
  p.g.fillText('waiting for data',AX.L+p.pw/2,AX.T+p.ph/2);
  $(id).textContent='';
}

function timeAxis(p,w){
  p.g.fillStyle='#8b9bb0'; p.g.textAlign='center'; p.g.textBaseline='top';
  for(let i=0;i<=4;i++){
    const t=w.tMin+w.span*i/4;
    p.g.fillText('-'+Math.round(w.tMax-t)+'s',
                 AX.L+p.pw*(t-w.tMin)/w.span, AX.T+p.ph+4);
  }
}

/* Min/max over the visible window, widened to at least minSpan about its own
   midpoint. Neither lux nor current can be negative, so a window that would
   start below zero is slid up instead of being padded into meaningless space;
   the span is preserved either way. Returns null if the window holds no
   samples for this column at all. */
function range(rows,col,minSpan){
  let lo=Infinity, hi=-Infinity;
  for(const r of rows){
    const v=r[col];
    if(v==null) continue;
    if(v<lo) lo=v;
    if(v>hi) hi=v;
  }
  if(lo>hi) return null;
  if(hi-lo<minSpan){ const mid=(lo+hi)/2; lo=mid-minSpan/2; hi=mid+minSpan/2; }
  else { const pad=(hi-lo)*0.08; lo-=pad; hi+=pad; }
  if(lo<0){ hi-=lo; lo=0; }
  return {lo,hi};
}

function trace(p,rows,w,col,color,lo,hi,lw,alpha){
  const g=p.g, sp=(hi-lo)||1;
  g.strokeStyle=color; g.lineWidth=lw; g.globalAlpha=alpha; g.lineJoin='round';
  g.beginPath();
  let pen=false, clip=false;
  for(const r of rows){
    const v=r[col];
    if(v==null){pen=false;continue;}            // gated or faulted: leave a gap
    let f=(v-lo)/sp;
    if(f>1){f=1;clip=true;}
    if(f<0){f=0;clip=true;}
    const x=AX.L+p.pw*(r[0]-w.tMin)/w.span, y=AX.T+p.ph*(1-f);
    pen?g.lineTo(x,y):g.moveTo(x,y); pen=true;
  }
  g.stroke(); g.globalAlpha=1;
  return clip;
}

function drawLoss(){
  const p=prep('chartLoss'); if(!p) return;
  const g=p.g, rows=hist.rows||[];

  gridlines(p,4);
  g.fillStyle='#8b9bb0'; g.textAlign='right'; g.textBaseline='middle';
  for(let i=0;i<=4;i++) g.fillText(((4-i)/4).toFixed(2),AX.L-6,AX.T+p.ph*i/4);
  g.textAlign='left'; g.textBaseline='alphabetic';
  g.fillText('loss fraction',AX.L,AX.T-4);

  if(!rows.length){ noData(p,'lossnote'); return; }
  const w=win(rows);
  timeAxis(p,w);

  if(!$('cLoss').checked){
    $('lossnote').textContent='Loss trace hidden.';
    return;
  }
  const clip=trace(p,rows,w,1,'#58a6ff',0,1,2,1);
  $('lossnote').textContent = 'Fixed 0 to 1 axis. Gaps are seconds the gate held the estimate back.'
    + (clip?' Samples outside 0-1 are drawn at the axis limit.':'');
}

function drawCtx(){
  const p=prep('chartCtx'); if(!p) return;
  const g=p.g, rows=hist.rows||[];
  const wantLux=$('cLux').checked, wantIb=$('cIb').checked;

  gridlines(p,2);
  if(!rows.length){ noData(p,'ctxnote'); return; }
  const w=win(rows);
  timeAxis(p,w);

  const rL=wantLux?range(rows,2,MINSPAN.lux):null;
  const rI=wantIb ?range(rows,3,MINSPAN.ib ):null;

  if(rL){
    g.fillStyle='#d29922'; g.textAlign='right'; g.textBaseline='middle';
    for(let i=0;i<=2;i++) g.fillText(f0(rL.hi-(rL.hi-rL.lo)*i/2),AX.L-6,AX.T+p.ph*i/2);
    g.textAlign='left'; g.textBaseline='alphabetic';
    g.fillText('lux',AX.L,AX.T-4);
  }
  if(rI){
    g.fillStyle='#3fb950'; g.textAlign='left'; g.textBaseline='middle';
    for(let i=0;i<=2;i++) g.fillText(f1(rI.hi-(rI.hi-rI.lo)*i/2),AX.L+p.pw+6,AX.T+p.ph*i/2);
    g.textAlign='right'; g.textBaseline='alphabetic';
    g.fillText('mA',AX.L+p.pw+AX.R-4,AX.T-4);
  }
  if(rL) trace(p,rows,w,2,'#d29922',rL.lo,rL.hi,1.5,.9);
  if(rI) trace(p,rows,w,3,'#3fb950',rI.lo,rI.hi,1.5,.9);

  const bits=[];
  if(wantLux) bits.push(rL?('lux '+f0(rL.lo)+' to '+f0(rL.hi)):'lux: no samples in window');
  if(wantIb)  bits.push(rI?('B current '+f1(rI.lo)+' to '+f1(rI.hi)+' mA'):'B current: no samples in window');
  $('ctxnote').textContent = bits.length
    ? 'Axes: '+bits.join(' · ')+'. Auto-ranged over the visible window, never narrower than '
      +f0(MINSPAN.lux)+' lx or '+MINSPAN.ib+' mA, so a steady signal draws flat instead of filling the plot with noise.'
    : 'No context series selected.';
}

function draw(){ drawLoss(); drawCtx(); }

/* ---- d: payback ------------------------------------------------------- */
function payback(){
  const wp=+$('arrayWp').value, sh=+$('sunHours').value;
  const tf=+$('tariff').value, cc=+$('cleanCost').value;
  const ls=clamp01(+$('lossIn').value||0);

  const clean=wp*sh/1000, lost=clean*ls, value=lost*tf;
  $('p_clean').textContent=clean.toFixed(2)+' kWh/day';
  $('p_lost').textContent=lost.toFixed(3)+' kWh/day';
  $('p_value').textContent=value.toFixed(2)+' BDT/day';

  const el=$('p_days');
  if(value>0 && cc>0){
    const d=cc/value;
    el.className='n'; el.textContent=d<1?d.toFixed(2):d.toFixed(1);
    $('p_daysub').textContent='days to pay back one cleaning';
  }else if(cc<=0){
    el.className='n'; el.textContent='0';
    $('p_daysub').textContent='cleaning is free at this cost';
  }else{
    el.className='n never'; el.textContent='not yet';
    $('p_daysub').textContent='no recoverable energy at this loss';
  }
}

/* ---- config ----------------------------------------------------------- */
function fillInputs(){ for(const k of F) $(k).value=cfg[k]; payback(); }

let pushTimer=null;
function pushConfig(){
  clearTimeout(pushTimer);
  pushTimer=setTimeout(()=>{
    const b=new URLSearchParams();
    for(const k of F) b.append(k,$(k).value);
    fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})
      .then(r=>r.json()).then(j=>{cfg=j;}).catch(()=>{});
  },600);
}
for(const k of F){
  $(k).addEventListener('focus',()=>editing=k);
  $(k).addEventListener('blur',()=>editing=null);
  $(k).addEventListener('input',()=>{payback();pushConfig();});
}
$('lossIn').addEventListener('focus',()=>{editing='lossIn';$('lossLive').checked=false;});
$('lossIn').addEventListener('blur',()=>editing=null);
$('lossIn').addEventListener('input',payback);
$('lossLive').addEventListener('change',render);
for(const k of ['cLoss','cLux','cIb']) $(k).addEventListener('change',draw);
addEventListener('resize',draw);

pollState(); pollHistory();
setInterval(pollState,1000);
setInterval(pollHistory,10000);
</script>
</body>
</html>)HTML";
