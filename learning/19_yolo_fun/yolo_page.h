#pragma once

// 物体识别网页（繁体中文）。画面向 /snap 一张一张要，框由 /events（SSE）推来，
// 触发马达的类别用 /api/cmd?cls=N 设定，/api/status 每秒轮询一次马达角度。
static const char YOLO_PAGE[] = R"HTML(<!DOCTYPE html>
<html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>YOLO 趣味辨識</title>
<style>
:root{--bg:#0f1420;--card:#182033;--line:#27324d;--tx:#e7ecf8;--mu:#8b97b5;--ac:#4cc9f0;--ok:#4ade80;--wa:#fbbf24}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.5 "Microsoft JhengHei",system-ui,sans-serif}
header{padding:14px 18px;display:flex;align-items:center;gap:12px;border-bottom:1px solid var(--line)}
header h1{font-size:18px;margin:0;flex:1}
.dot{width:10px;height:10px;border-radius:50%;background:#ef4444}.dot.on{background:var(--ok)}
main{display:grid;grid-template-columns:minmax(0,1.6fr) minmax(280px,1fr);gap:14px;padding:14px;max-width:1200px;margin:auto}
@media(max-width:820px){main{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px}
.card h2{font-size:14px;color:var(--mu);margin:0 0 8px;font-weight:600}
.view{position:relative;background:#000;border-radius:8px;overflow:hidden;aspect-ratio:4/3}
.view img,.view canvas{position:absolute;inset:0;width:100%;height:100%}
.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:10px}
button,select{background:#223052;color:var(--tx);border:1px solid var(--line);border-radius:8px;padding:6px 12px;font:inherit}
button:hover{border-color:var(--ac);cursor:pointer}
input[type=range]{accent-color:var(--ac)}
label{color:var(--mu);font-size:13px}
.chips{display:flex;flex-wrap:wrap;gap:6px}
.chip{padding:3px 10px;border-radius:999px;border:1px solid var(--line);font-size:13px}
.chip b{color:var(--ac)}
.stat{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;text-align:center}
.stat div{background:#111a2c;border-radius:8px;padding:8px 4px}
.stat b{display:block;font-size:22px;color:var(--ac)}
.stat span{font-size:12px;color:var(--mu)}
.servo{display:flex;align-items:center;gap:12px}
.gauge{width:84px;height:48px;position:relative;overflow:hidden}
.gauge:before{content:"";position:absolute;left:0;top:0;width:84px;height:84px;border-radius:50%;border:6px solid #223052}
.needle{position:absolute;left:41px;bottom:0;width:2px;height:38px;background:var(--wa);transform-origin:bottom center;transition:transform .15s}
ul{list-style:none;padding:0;margin:0;max-height:220px;overflow:auto}
li{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid var(--line);font-size:14px}
li i{font-style:normal;color:var(--mu)}
.view.crowd{box-shadow:0 0 0 4px #ef4444 inset;animation:bl .6s infinite alternate}
@keyframes bl{to{box-shadow:0 0 0 4px transparent inset}}
input[type=number]{width:56px;background:#223052;color:var(--tx);border:1px solid var(--line);border-radius:8px;padding:5px}
.log li{font-size:13px}
</style></head><body>
<header><span class="dot" id="dot"></span><h1>YOLO 趣味辨識 · HUB-8735</h1><span id="fps" style="color:var(--mu)"></span></header>
<main>
<section class="card">
<h2>即時畫面</h2>
<div class="view" id="view"><img id="img" alt=""><canvas id="cv"></canvas></div>
<div class="row">
<label>信心度 ≥ <b id="thv">50</b>%</label><input type="range" id="th" min="0" max="100" value="50">
<label><input type="checkbox" id="lab" checked> 標籤</label>
<label><input type="checkbox" id="snd" checked> 音效</label>
<label>擁擠警示 ≥ <input type="number" id="crowd" min="1" max="14" value="3"> 人</label>
<button id="shot">拍照存檔</button>
</div>
</section>
<section>
<div class="card"><h2>目前畫面</h2>
<div class="stat"><div><b id="nNow">0</b><span>物體數</span></div><div><b id="nKind">0</b><span>種類</span></div><div><b id="nTot">0</b><span>累計次數</span></div></div>
<div class="chips" id="chips" style="margin-top:10px"></div></div>
<div class="card" style="margin-top:14px"><h2>伺服馬達（IO21）</h2>
<div class="servo"><div class="gauge"><div class="needle" id="nd"></div></div>
<div><div style="font-size:22px"><b id="svA">0°</b> <small style="color:var(--mu)" id="svT"></small></div><div id="svH" style="color:var(--mu)"></div></div></div>
<div class="row"><label>觸發類別</label><select id="cls"></select>
<button id="svK">測試 70°</button><button id="svL">測試 -20°</button></div>
<div style="color:var(--mu);font-size:13px;margin-top:6px">看到觸發類別 → 70°；消失 1.5 秒 → -20°</div></div>
<div class="card" style="margin-top:14px"><h2>人數紀錄</h2>
<div class="stat"><div><b id="pPeak">0</b><span>最高同時人數</span></div><div><b id="pIn">0</b><span>進場次數</span></div><div><b id="pOut">0</b><span>離場次數</span></div></div>
<ul class="log" id="log" style="margin-top:8px"></ul></div>
<div class="card" style="margin-top:14px"><h2>累計排行（FUNC 鍵清除）</h2><ul id="rank"></ul></div>
</section>
</main>
<script>
const NAMES="人,腳踏車,汽車,機車,飛機,公車,火車,卡車,船,紅綠燈,消防栓,停止標誌,停車收費表,長椅,鳥,貓,狗,馬,羊,牛,大象,熊,斑馬,長頸鹿,背包,雨傘,手提包,領帶,行李箱,飛盤,滑雪板,單板滑雪,球,風箏,球棒,棒球手套,滑板,衝浪板,網球拍,瓶子,酒杯,杯子,叉子,刀子,湯匙,碗,香蕉,蘋果,三明治,柳橙,花椰菜,紅蘿蔔,熱狗,披薩,甜甜圈,蛋糕,椅子,沙發,盆栽,床,餐桌,馬桶,電視,筆電,滑鼠,遙控器,鍵盤,手機,微波爐,烤箱,烤麵包機,水槽,冰箱,書,時鐘,花瓶,剪刀,泰迪熊,吹風機,牙刷".split(",");
const $=id=>document.getElementById(id);
const img=$('img'),cv=$('cv'),ctx=cv.getContext('2d');
let dets=[],tot=0,seen=new Array(80).fill(0),minScore=50;
function col(t){return 'hsl('+(t*47%360)+',85%,60%)'}
// 有趣的小圖示（沒列到的類別就不加）
const EMO={0:'🧍',1:'🚲',2:'🚗',3:'🏍️',5:'🚌',14:'🐦',15:'🐱',16:'🐶',24:'🎒',25:'☂️',32:'⚽',39:'🍼',41:'☕',46:'🍌',47:'🍎',49:'🍊',53:'🍕',55:'🎂',56:'🪑',59:'🛏️',62:'📺',63:'💻',64:'🖱️',66:'⌨️',67:'📱',73:'📖',74:'⏰',76:'✂️',77:'🧸',79:'🪥'};
const nm=t=>(EMO[t]?EMO[t]+' ':'')+NAMES[t];
// ---- 人數：進場 / 離場 / 最高 / 音效 / 擁擠警示 ----
let people=0,peak=0,pin=0,pout=0,logs=[];
function beep(f,d){
  if(!$('snd').checked)return;
  try{const a=beep.a||(beep.a=new (window.AudioContext||window.webkitAudioContext)());
  const o=a.createOscillator(),g=a.createGain();o.frequency.value=f;g.gain.value=.08;o.connect(g);g.connect(a.destination);o.start();o.stop(a.currentTime+d)}catch(e){}
}
function note(s){
  const t=new Date().toTimeString().slice(0,8);logs.unshift('<li><span>'+s+'</span><i>'+t+'</i></li>');logs=logs.slice(0,8);
  $('log').innerHTML=logs.join('');
}
function trackPeople(n){
  // 人數要連續 3 張都一樣才算數，避免 YOLO 偶爾漏一幀就被算成進出
  trackPeople.h=(trackPeople.h||[]).concat(n).slice(-3);
  const h=trackPeople.h;if(h.length<3||!h.every(v=>v===h[0])||h[0]===people)return;
  const d=h[0]-people;
  if(d>0){pin+=d;note('🟢 有人進來（現在 '+h[0]+' 人）');beep(880,.12)}
  else{pout-=d;note('🔴 有人離開（現在 '+h[0]+' 人）');beep(440,.12)}
  people=h[0];if(people>peak)peak=people;
  $('pPeak').textContent=peak;$('pIn').textContent=pin;$('pOut').textContent=pout;
  const c=people>=(+$('crowd').value||99);
  $('view').classList.toggle('crowd',c);
  if(c){note('⚠️ 太擁擠了！');beep(1200,.3)}
}
// 類別下拉
NAMES.forEach((n,i)=>{const o=document.createElement('option');o.value=i;o.textContent=n;$('cls').appendChild(o)});
$('cls').onchange=()=>fetch('/api/cmd?cls='+$('cls').value).catch(()=>{});
$('th').oninput=()=>{minScore=+$('th').value;$('thv').textContent=minScore;draw()};
$('svK').onclick=()=>fetch('/api/cmd?sv=1').catch(()=>{});
$('svL').onclick=()=>fetch('/api/cmd?sv=0').catch(()=>{});
$('lab').onchange=draw;
// 畫面：收到一張再要下一張
let n=0,alive=0;
function snap(){img.src='/snap?'+(n++)}
img.onload=()=>{alive=Date.now();setTimeout(snap,30)};
img.onerror=()=>setTimeout(snap,500);
snap();
function draw(){
  const w=cv.clientWidth,h=cv.clientHeight;cv.width=w;cv.height=h;ctx.clearRect(0,0,w,h);
  ctx.lineWidth=2;ctx.font='bold 14px "Microsoft JhengHei",sans-serif';ctx.textBaseline='top';
  const show=dets.filter(d=>d.s>=minScore);
  for(const d of show){
    const x=d.x1*w/1000,y=d.y1*h/1000,bw=(d.x2-d.x1)*w/1000,bh=(d.y2-d.y1)*h/1000,c=col(d.t);
    ctx.strokeStyle=c;ctx.strokeRect(x,y,bw,bh);
    if($('lab').checked){
      const s=NAMES[d.t]+' '+d.s+'%',tw=ctx.measureText(s).width+8,ty=y>20?y-20:y;
      ctx.fillStyle=c;ctx.fillRect(x,ty,tw,20);ctx.fillStyle='#000';ctx.fillText(s,x+4,ty+2);
    }
  }
  // 左上角：目前人數
  const np=show.filter(d=>d.t===0).length;
  const txt='🧍 '+np+' 人';ctx.font='bold 26px "Microsoft JhengHei",sans-serif';
  const pw=ctx.measureText(txt).width+22;
  ctx.fillStyle=np>=(+$('crowd').value||99)?'rgba(239,68,68,.85)':'rgba(15,20,32,.75)';
  ctx.fillRect(10,10,pw,40);ctx.fillStyle='#fff';ctx.textBaseline='middle';ctx.fillText(txt,21,31);ctx.textBaseline='top';
  // 面板
  const cnt={};show.forEach(d=>cnt[d.t]=(cnt[d.t]||0)+1);
  $('nNow').textContent=show.length;$('nKind').textContent=Object.keys(cnt).length;$('nTot').textContent=tot;
  $('chips').innerHTML=Object.keys(cnt).map(t=>'<span class="chip" style="border-color:'+col(t)+'">'+nm(t)+' <b>×'+cnt[t]+'</b></span>').join('')||'<span style="color:var(--mu)">沒看到物體</span>';
}
function rank(){
  const a=seen.map((v,i)=>[i,v]).filter(x=>x[1]>0).sort((p,q)=>q[1]-p[1]).slice(0,10);
  $('rank').innerHTML=a.map(x=>'<li><span>'+nm(x[0])+'</span><i>'+x[1]+' 次</i></li>').join('')||'<li><i>還沒有資料</i></li>';
}
// 事件：框
function connect(){
  const es=new EventSource('/events');
  es.onopen=()=>$('dot').classList.add('on');
  es.onerror=()=>{$('dot').classList.remove('on')};
  es.onmessage=e=>{
    const v=e.data.split(',').map(Number),k=v[0];dets=[];
    for(let i=0;i<k;i++){const o=1+i*6;dets.push({t:v[o],s:v[o+1],x1:v[o+2],y1:v[o+3],x2:v[o+4],y2:v[o+5]});}
    dets.filter(d=>d.s>=minScore).forEach(d=>{seen[d.t]++;tot++});
    trackPeople(dets.filter(d=>d.t===0&&d.s>=minScore).length);
    draw();
  };
  es.addEventListener('reset',()=>{seen.fill(0);tot=0;peak=pin=pout=0;logs=[];$('log').innerHTML='';$('pPeak').textContent=$('pIn').textContent=$('pOut').textContent=0;rank();draw()});
}
connect();setInterval(rank,1000);window.onresize=draw;
// 馬達狀態
async function st(){
  try{
    const j=await (await fetch('/api/status')).json();
    $('svA').textContent=j.servo+'°';$('svT').textContent='→ '+j.stgt+'°';
    $('nd').style.transform='rotate('+(j.servo-70)+'deg)';
    $('svH').textContent=j.hit?'目標在畫面中':'目標不在畫面';
    if(document.activeElement!==$('cls'))$('cls').value=j.cls;
    $('fps').textContent='YOLO '+j.fps+' 幀/秒';
  }catch(e){}
}
setInterval(st,1000);st();
$('shot').onclick=()=>{
  const c=document.createElement('canvas');c.width=img.naturalWidth||640;c.height=img.naturalHeight||480;
  const g=c.getContext('2d');g.drawImage(img,0,0,c.width,c.height);g.drawImage(cv,0,0,c.width,c.height);
  const a=document.createElement('a');a.href=c.toDataURL('image/jpeg',.9);a.download='yolo_'+Date.now()+'.jpg';a.click();
};
</script></body></html>)HTML";
