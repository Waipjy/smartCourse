#ifndef __FILTER_PAGE_H__
#define __FILTER_PAGE_H__

/*
  「手势滤镜」网页（板子上的 HTTP 伺服器回传这一整页）。

  所有画面处理都在浏览器里做（canvas），板子只负责：
    /snap    -> 一张 JPEG 快照（网页连续要，当作影像）
    /events  -> Server-Sent Events，每处理完一帧推送 21 个手部关键点
  网页拿到关键点后，用「拇指尖」和「食指尖」当对角线拉出一个矩形，在矩形里即时套滤镜。

  用 C++11 原始字串，里面的引号、反斜线都不用跳脱。
  要开发 / 调整这个页面，可以把 R"HTML( 和 )HTML" 中间的内容存成 .html 档，
  搭配一个会吐假关键点和假影像的本机伺服器在电脑上直接测试，不必每次都烧录。
*/

static const char FILTER_PAGE[] = R"HTML(<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>手勢濾鏡</title>
<style>
:root{--bg0:#0b1020;--bg1:#151b33;--glass:rgba(255,255,255,.06);--line:rgba(255,255,255,.12);--tx:#eef1fb;--mu:#98a2c3;--ac:#7c5cff;--ac2:#22d3ee;--ok:#34d399;--bad:#fb7185}
*{box-sizing:border-box}html,body{margin:0}
body{color:var(--tx);font:15px/1.5 system-ui,"Noto Sans TC","PingFang TC",sans-serif;min-height:100vh;padding:14px;
 background:radial-gradient(1200px 700px at 8% -10%,#2a1b6b 0,transparent 55%),radial-gradient(900px 600px at 105% 10%,#0b5a74 0,transparent 55%),linear-gradient(160deg,var(--bg0),var(--bg1))}
#app{max-width:1180px;margin:0 auto;display:grid;gap:14px}
header{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px}
.brand{display:flex;gap:10px;align-items:center}.logo{width:38px;height:38px;border-radius:12px;display:grid;place-items:center;font-size:20px;
 background:linear-gradient(135deg,var(--ac),var(--ac2));box-shadow:0 6px 20px rgba(124,92,255,.45)}
h1{font-size:19px;margin:0;letter-spacing:.5px}.sub{font-size:12px;color:var(--mu)}
.chips{display:flex;gap:6px;flex-wrap:wrap}.pill{font-size:12px;padding:3px 11px;border-radius:99px;background:var(--glass);border:1px solid var(--line);color:var(--mu)}
.pill.on{background:rgba(52,211,153,.16);border-color:rgba(52,211,153,.5);color:var(--ok)}.pill.off{background:rgba(251,113,133,.14);border-color:rgba(251,113,133,.5);color:var(--bad)}
.layout{display:grid;gap:14px;grid-template-columns:minmax(0,1fr)}
@media(min-width:960px){.layout{grid-template-columns:minmax(0,1.7fr) minmax(300px,1fr)}}
.card{background:var(--glass);border:1px solid var(--line);border-radius:16px;padding:14px;backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px)}
h2{font-size:13px;margin:0 0 10px;color:var(--mu);font-weight:600;letter-spacing:1px}
#stage{position:relative;border-radius:18px;overflow:hidden;background:#05070f;border:1px solid var(--line);box-shadow:0 18px 50px rgba(0,0,0,.45);touch-action:none;cursor:crosshair}
#view{display:block;width:100%;height:auto}
.tabs{position:absolute;left:12px;top:12px;display:flex;gap:6px;padding:4px;border-radius:12px;background:rgba(10,14,28,.55);border:1px solid var(--line);backdrop-filter:blur(8px)}
.tab{padding:6px 12px;border-radius:9px;font-size:13px;cursor:pointer;color:var(--mu);user-select:none}
.tab.on{background:linear-gradient(135deg,var(--ac),var(--ac2));color:#fff;box-shadow:0 4px 14px rgba(124,92,255,.5)}
#cd{position:absolute;inset:0;display:none;place-items:center;font-size:120px;font-weight:800;color:#fff;text-shadow:0 0 40px var(--ac2);pointer-events:none}
#flash{position:absolute;inset:0;background:#fff;opacity:0;pointer-events:none;transition:opacity .35s}
#hint{position:absolute;left:0;right:0;bottom:14px;display:flex;justify-content:center;pointer-events:none}
#hint span{padding:7px 16px;border-radius:99px;background:rgba(10,14,28,.62);border:1px solid var(--line);font-size:13px;backdrop-filter:blur(8px)}
.filters{display:grid;grid-template-columns:repeat(auto-fill,minmax(82px,1fr));gap:8px}
.f{padding:9px 4px;border-radius:12px;text-align:center;cursor:pointer;background:var(--glass);border:1px solid var(--line);font-size:12px;user-select:none;transition:.15s}
.f i{display:block;font-style:normal;font-size:22px;line-height:1.3}.f:hover{transform:translateY(-2px);border-color:var(--ac2)}
.f.on{background:linear-gradient(135deg,rgba(124,92,255,.35),rgba(34,211,238,.28));border-color:var(--ac2);box-shadow:0 0 18px rgba(34,211,238,.35)}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin:6px 0}
label{font-size:13px;color:var(--mu);display:flex;gap:6px;align-items:center}
select,button{font:inherit;font-size:13px;border-radius:10px;border:1px solid var(--line);background:var(--glass);color:var(--tx);padding:6px 11px;cursor:pointer}
button.p{background:linear-gradient(135deg,var(--ac),var(--ac2));border:0;color:#fff}
input[type=range]{accent-color:var(--ac2)}
#gal{display:grid;grid-template-columns:repeat(auto-fill,minmax(96px,1fr));gap:8px;min-height:20px}
#gal img{width:100%;border-radius:10px;border:1px solid var(--line);cursor:pointer;display:block}
.tip{font-size:13px;color:var(--mu)}.tip b{color:var(--tx)}ul{margin:4px 0;padding-left:18px}li{margin:3px 0}
#cam{position:fixed;left:0;top:0;width:2px;height:2px;opacity:.01;pointer-events:none}
#toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%);background:rgba(20,26,50,.92);border:1px solid var(--line);padding:8px 18px;border-radius:99px;opacity:0;transition:.2s;pointer-events:none;z-index:9}
#toast.show{opacity:1}
</style></head><body><div id="app">
<header><div class="brand"><div class="logo">✋</div><div><h1>手勢濾鏡</h1><div class="sub">拇指 ＋ 食指，拉出你的魔法取景框</div></div></div>
<div class="chips"><span id="cConn" class="pill">連線中…</span><span id="cHand" class="pill">未偵測到手</span><span id="cRate" class="pill">0 fps</span></div></header>
<div class="layout"><div>
<div id="stage"><canvas id="view" width="960" height="720"></canvas>
<div class="tabs"><div class="tab on" data-m="filter">🎞 濾鏡框</div><div class="tab" data-m="draw">🖌 彩繪</div></div>
<div id="cd"></div><div id="flash"></div><div id="hint"><span id="hintTx"></span></div></div>
<div class="card" style="margin-top:14px"><h2>相簿</h2><div id="gal"></div>
<div class="row"><button id="bClr">清除相簿</button><span class="tip">點圖片可下載。</span></div></div></div>
<aside style="display:grid;gap:14px;align-content:start">
<div class="card"><h2>濾鏡（點選，或用手指捏一下切換）</h2><div class="filters" id="fl"></div></div>
<div class="card"><h2>怎麼玩</h2><div class="tip"><ul>
<li><b>取景框</b>：拇指尖和食指尖當對角線，拉出矩形，框內即時套濾鏡。</li>
<li><b>換濾鏡</b>：拇指和食指「捏一下」再打開（或板子上按 FUNC 鍵）。</li>
<li><b>拍照</b>：框住不動約 1 秒，倒數後自動拍下框內畫面。</li>
<li><b>彩繪模式</b>：食指尖畫發光軌跡；捏一下換顏色，捏住不放 1 秒清空。</li>
<li>沒有手時也可以用滑鼠在畫面上拖曳拉出矩形試玩。</li></ul></div></div>
<div class="card"><h2>設定</h2>
<div class="row"><label><input type="checkbox" id="kMir"> 鏡像畫面</label><label><input type="checkbox" id="kSk" checked> 顯示手部骨架</label></div>
<div class="row"><label><input type="checkbox" id="kAuto" checked> 停住自動拍照</label><label><input type="checkbox" id="kSnd" checked> 音效</label></div>
<div class="row"><label>座標對應 <select id="sMap"><option value="crop">置中方形</option><option value="stretch">拉伸全幅</option></select></label></div>
<div class="row"><label>跟手平滑 <input type="range" id="rSm" min="4" max="40" value="22"></label></div>
<div class="tip">骨架和手指對不上時，換一下「座標對應」。</div></div>
</aside></div></div><img id="cam" alt=""><div id="toast"></div>
<script>
'use strict';
const $=i=>document.getElementById(i);
const view=$('view'),g=view.getContext('2d'),cam=$('cam'),stage=$('stage');
const clamp=(x,a,b)=>Math.max(a,Math.min(b,x));
let tt;function toast(m){const t=$('toast');t.textContent=m;t.className='show';clearTimeout(tt);tt=setTimeout(()=>t.className='',1800)}
const store={get(k,d){try{const v=localStorage.getItem('hf_'+k);return v===null?d:JSON.parse(v)}catch(e){return d}},set(k,v){try{localStorage.setItem('hf_'+k,JSON.stringify(v))}catch(e){}}};
const cfg={mirror:store.get('mirror',false),skel:store.get('skel',true),auto:store.get('auto',true),snd:store.get('snd',true),map:store.get('map','crop'),sm:store.get('sm',22),filter:store.get('filter','neon'),mode:'filter'};
$('kMir').checked=cfg.mirror;$('kSk').checked=cfg.skel;$('kAuto').checked=cfg.auto;$('kSnd').checked=cfg.snd;$('sMap').value=cfg.map;$('rSm').value=cfg.sm;
$('kMir').onchange=e=>{cfg.mirror=e.target.checked;store.set('mirror',cfg.mirror)};
$('kSk').onchange=e=>{cfg.skel=e.target.checked;store.set('skel',cfg.skel)};
$('kAuto').onchange=e=>{cfg.auto=e.target.checked;store.set('auto',cfg.auto)};
$('kSnd').onchange=e=>{cfg.snd=e.target.checked;store.set('snd',cfg.snd)};
$('sMap').onchange=e=>{cfg.map=e.target.value;store.set('map',cfg.map)};
$('rSm').oninput=e=>{cfg.sm=+e.target.value;store.set('sm',cfg.sm)};

/* ---- 音效 ---- */
let AC=null;
function beep(f,d,type,vol,slide){if(!cfg.snd)return;try{AC=AC||new(window.AudioContext||window.webkitAudioContext)();if(AC.state==='suspended')AC.resume();
 const o=AC.createOscillator(),gn=AC.createGain(),n=AC.currentTime;o.type=type||'sine';o.frequency.setValueAtTime(f,n);if(slide)o.frequency.exponentialRampToValueAtTime(slide,n+d);
 gn.gain.setValueAtTime(vol||.05,n);gn.gain.exponentialRampToValueAtTime(.0001,n+d);o.connect(gn);gn.connect(AC.destination);o.start();o.stop(n+d)}catch(e){}}

/* ---- 濾鏡（對一塊像素做處理；p 是 RGBA 陣列，w,h 是尺寸） ---- */
const lum=(r,g_,b)=>.299*r+.587*g_+.114*b;
function pal(t,stops){const n=stops.length-1,x=clamp(t,0,1)*n,i=Math.min(n-1,Math.floor(x)),f=x-i,a=stops[i],b=stops[i+1];return[a[0]+(b[0]-a[0])*f,a[1]+(b[1]-a[1])*f,a[2]+(b[2]-a[2])*f]}
const HEAT=[[10,0,40],[90,0,140],[220,30,60],[255,170,0],[255,255,200]];
const DUO=[[28,20,110],[255,70,160],[255,214,90]];
const FILTERS=[
 {id:'none',n:'原色',e:'🌈'},
 {id:'gray',n:'灰階',e:'🎞',px(p){for(let i=0;i<p.length;i+=4){const l=lum(p[i],p[i+1],p[i+2]);p[i]=p[i+1]=p[i+2]=l}}},
 {id:'sepia',n:'懷舊',e:'📜',px(p){for(let i=0;i<p.length;i+=4){const r=p[i],g_=p[i+1],b=p[i+2];p[i]=r*.393+g_*.769+b*.189;p[i+1]=r*.349+g_*.686+b*.168;p[i+2]=r*.272+g_*.534+b*.131}}},
 {id:'invert',n:'反轉',e:'🌗',px(p){for(let i=0;i<p.length;i+=4){p[i]=255-p[i];p[i+1]=255-p[i+1];p[i+2]=255-p[i+2]}}},
 {id:'bw',n:'黑白',e:'◐',px(p){for(let i=0;i<p.length;i+=4){const v=lum(p[i],p[i+1],p[i+2])>118?255:0;p[i]=p[i+1]=p[i+2]=v}}},
 {id:'heat',n:'熱感',e:'🔥',px(p){for(let i=0;i<p.length;i+=4){const c=pal(lum(p[i],p[i+1],p[i+2])/255,HEAT);p[i]=c[0];p[i+1]=c[1];p[i+2]=c[2]}}},
 {id:'duo',n:'雙色',e:'🎨',px(p){for(let i=0;i<p.length;i+=4){const c=pal(lum(p[i],p[i+1],p[i+2])/255,DUO);p[i]=c[0];p[i+1]=c[1];p[i+2]=c[2]}}},
 {id:'comic',n:'漫畫',e:'💥',px(p){for(let i=0;i<p.length;i+=4){for(let k=0;k<3;k++)p[i+k]=Math.round(p[i+k]/64)*64+20;}}},
 {id:'neon',n:'霓虹',e:'✨',px(p,w,h){const L=new Float32Array(w*h);for(let i=0;i<w*h;i++)L[i]=lum(p[i*4],p[i*4+1],p[i*4+2]);
  for(let y=1;y<h-1;y++)for(let x=1;x<w-1;x++){const i=y*w+x,gx=-L[i-w-1]-2*L[i-1]-L[i+w-1]+L[i-w+1]+2*L[i+1]+L[i+w+1],gy=-L[i-w-1]-2*L[i-w]-L[i-w+1]+L[i+w-1]+2*L[i+w]+L[i+w+1];
   const m=clamp(Math.hypot(gx,gy)/160,0,1),o=i*4,t=(x+y)/(w+h);p[o]=20+m*(120+135*t);p[o+1]=10+m*(255-120*t);p[o+2]=40+m*255;}}},
 {id:'pixel',n:'馬賽克',e:'🧱',scale:.07},
 {id:'blur',n:'模糊',e:'🌫',scale:.1,smooth:true},
 {id:'zoom',n:'放大',e:'🔍',zoom:1.8},
 {id:'flip',n:'翻轉',e:'🪞',flip:true}
];
const FMAP={};FILTERS.forEach(f=>FMAP[f.id]=f);
if(!FMAP[cfg.filter])cfg.filter='neon';
const work=document.createElement('canvas'),wg=work.getContext('2d',{willReadFrequently:true});
const tiny=document.createElement('canvas'),tg=tiny.getContext('2d');
// 把影像（img）中的矩形 r 套上濾鏡 f，畫到 target 的 (dx,dy) 位置，大小 r.w x r.h
function renderRegion(tc,f,r,dx,dy){
 const src=cam;tc.save();tc.imageSmoothingQuality='high';
 if(f.zoom){const zw=r.w/f.zoom,zh=r.h/f.zoom;tc.drawImage(src,r.x+(r.w-zw)/2,r.y+(r.h-zh)/2,zw,zh,dx,dy,r.w,r.h)}
 else if(f.flip){tc.translate(dx+r.w,dy);tc.scale(-1,1);tc.drawImage(src,r.x,r.y,r.w,r.h,0,0,r.w,r.h)}
 else if(f.scale){const tw=Math.max(2,Math.round(r.w*f.scale)),th=Math.max(2,Math.round(r.h*f.scale));tiny.width=tw;tiny.height=th;
  tg.drawImage(src,r.x,r.y,r.w,r.h,0,0,tw,th);tc.imageSmoothingEnabled=!!f.smooth;tc.drawImage(tiny,0,0,tw,th,dx,dy,r.w,r.h)}
 else if(f.px){const k=Math.min(1,360/Math.max(r.w,r.h)),ww=Math.max(2,Math.round(r.w*k)),hh=Math.max(2,Math.round(r.h*k));
  work.width=ww;work.height=hh;wg.drawImage(src,r.x,r.y,r.w,r.h,0,0,ww,hh);
  const im=wg.getImageData(0,0,ww,hh);f.px(im.data,ww,hh);wg.putImageData(im,0,0);tc.drawImage(work,0,0,ww,hh,dx,dy,r.w,r.h)}
 else tc.drawImage(src,r.x,r.y,r.w,r.h,dx,dy,r.w,r.h);
 tc.restore();
}
function buildFilterList(){$('fl').innerHTML=FILTERS.map(f=>'<div class="f'+(f.id===cfg.filter?' on':'')+'" data-f="'+f.id+'"><i>'+f.e+'</i>'+f.n+'</div>').join('')}
$('fl').onclick=e=>{const d=e.target.closest('.f');if(d)setFilter(d.dataset.f)};
function setFilter(id,quiet){cfg.filter=id;store.set('filter',id);buildFilterList();if(!quiet){beep(660,.08,'triangle',.05,990);toast(FMAP[id].e+' '+FMAP[id].n)}}
function cycleFilter(d){const i=FILTERS.findIndex(f=>f.id===cfg.filter);setFilter(FILTERS[(i+d+FILTERS.length)%FILTERS.length].id)}
buildFilterList();

/* ---- 模式分頁 ---- */
document.querySelectorAll('.tab').forEach(t=>t.onclick=()=>{cfg.mode=t.dataset.m;document.querySelectorAll('.tab').forEach(x=>x.classList.toggle('on',x===t));if(cfg.mode==='draw')trail=[];beep(500,.08,'triangle')});

/* ---- 手部資料 ---- */
// 關鍵點：0 手腕、4 拇指尖、8 食指尖、9 中指根。座標 0~1，對應 NN 影像的方形區域。
const HAND={found:false,t:0,u:new Float32Array(21),v:new Float32Array(21),msgs:0};
const P={a:{x:0,y:0},b:{x:0,y:0},palm:100,ready:false,pts:[]};  // 平滑後的（畫面像素）座標
let nw=640,nh=480;
function mapPt(u,v){const asp=nw/nh;let x=cfg.map==='crop'?.5+(u-.5)/asp:u;return[x*view.width,v*view.height]}
function onHand(found,u,v){HAND.msgs++;HAND.found=found;if(!found)return;HAND.t=performance.now();HAND.u.set(u);HAND.v.set(v)}

/* ---- 狀態 ---- */
let pinched=false,pinchT=0,lastPinchEnd=0,cdStart=0,lastCap=0,stillT0=0,stillRef=null,trail=[],hue=200,manual=null,drag=null;
const PINCH_ON=.38,PINCH_OFF=.62;
const gallery=[];
function shutter(rect){
 const c=document.createElement('canvas');c.width=Math.round(rect.w);c.height=Math.round(rect.h);
 renderRegion(c.getContext('2d'),FMAP[cfg.filter],rect,0,0);
 const url=c.toDataURL('image/jpeg',.9);gallery.unshift(url);if(gallery.length>12)gallery.pop();drawGallery();
 const fl=$('flash');fl.style.transition='none';fl.style.opacity=.85;requestAnimationFrame(()=>{fl.style.transition='opacity .35s';fl.style.opacity=0});
 beep(1200,.05,'square',.04);setTimeout(()=>beep(800,.12,'square',.04),60);toast('📸 已拍下！')}
function drawGallery(){$('gal').innerHTML=gallery.map((u,i)=>'<a href="'+u+'" download="hand-filter-'+(gallery.length-i)+'.jpg"><img src="'+u+'"></a>').join('')}
$('bClr').onclick=()=>{gallery.length=0;drawGallery()};

/* ---- 每幀更新 ---- */
function currentRect(){
 if(!HAND.found||performance.now()-HAND.t>600){if(manual)return manual;return null}
 if(pinched)return null;
 const x0=Math.min(P.a.x,P.b.x),y0=Math.min(P.a.y,P.b.y),x1=Math.max(P.a.x,P.b.x),y1=Math.max(P.a.y,P.b.y);
 const r={x:x0,y:y0,w:x1-x0,h:y1-y0};
 return(r.w<40||r.h<40)?null:r;
}
function updateHand(dt,now){
 const live=HAND.found&&now-HAND.t<600;
 if(!live){stillT0=0;pinched=false;return}
 const[a_x,a_y]=mapPt(HAND.u[4],HAND.v[4]),[b_x,b_y]=mapPt(HAND.u[8],HAND.v[8]);
 const k=1-Math.exp(-dt*cfg.sm);
 if(!P.ready){P.a.x=a_x;P.a.y=a_y;P.b.x=b_x;P.b.y=b_y;P.ready=true}
 P.a.x+=(a_x-P.a.x)*k;P.a.y+=(a_y-P.a.y)*k;P.b.x+=(b_x-P.b.x)*k;P.b.y+=(b_y-P.b.y)*k;
 const wx=HAND.u[0]-HAND.u[9],wy=HAND.v[0]-HAND.v[9],palm=Math.max(.05,Math.hypot(wx,wy))*view.height;P.palm=palm;
 const d=Math.hypot(P.a.x-P.b.x,P.a.y-P.b.y)/palm;
 // 捏合 = 點擊
 if(!pinched&&d<PINCH_ON){pinched=true;pinchT=now}
 else if(pinched&&d>PINCH_OFF){pinched=false;if(now-pinchT<1100&&now-lastPinchEnd>350){lastPinchEnd=now;if(cfg.mode==='filter')cycleFilter(1);else{hue=(hue+55)%360;beep(700,.06,'triangle')}}}
 if(pinched&&cfg.mode==='draw'&&now-pinchT>1000){trail=[];pinchT=now+99999;toast('已清空');beep(300,.15,'sawtooth',.04,120)}
}
function updateTrail(now){
 if(cfg.mode!=='draw'){trail=[];return}
 if(HAND.found&&now-HAND.t<600&&P.ready)trail.push({x:P.b.x,y:P.b.y,h:hue,t:now});
 while(trail.length&&now-trail[0].t>2600)trail.shift();
}
function updateCapture(now,rect){
 if(cfg.mode!=='filter'||!cfg.auto||!rect||drag||now-lastCap<2200){cdStart=0;stillT0=0;$('cd').style.display='none';return}
 if(!stillRef||Math.hypot(rect.x-stillRef.x,rect.y-stillRef.y)>16||Math.hypot(rect.w-stillRef.w,rect.h-stillRef.h)>24){stillRef={x:rect.x,y:rect.y,w:rect.w,h:rect.h};stillT0=now;cdStart=0;$('cd').style.display='none';return}
 if(now-stillT0<1000)return;
 if(!cdStart){cdStart=now}
 const n=3-Math.floor((now-cdStart)/650);
 if(n<=0){shutter(rect);lastCap=now;cdStart=0;stillT0=0;stillRef=null;$('cd').style.display='none'}
 else{const c=$('cd');if(c.textContent!=String(n)||c.style.display!=='grid'){c.textContent=n;c.style.display='grid';beep(900,.07,'sine',.05)}}
}

/* ---- 繪圖 ---- */
const BONES=[[0,1],[1,2],[2,3],[3,4],[0,5],[5,6],[6,7],[7,8],[5,9],[9,10],[10,11],[11,12],[9,13],[13,14],[14,15],[15,16],[13,17],[17,18],[18,19],[19,20],[0,17]];
function rr(c,x,y,w,h,r){c.beginPath();c.moveTo(x+r,y);c.arcTo(x+w,y,x+w,y+h,r);c.arcTo(x+w,y+h,x,y+h,r);c.arcTo(x,y+h,x,y,r);c.arcTo(x,y,x+w,y,r);c.closePath()}
function drawFrameDeco(r,f,now){
 const pulse=.6+.4*Math.sin(now/220);
 g.save();g.shadowColor='#22d3ee';g.shadowBlur=18;
 const gr=g.createLinearGradient(r.x,r.y,r.x+r.w,r.y+r.h);gr.addColorStop(0,'#7c5cff');gr.addColorStop(1,'#22d3ee');
 g.strokeStyle=gr;g.lineWidth=3;rr(g,r.x,r.y,r.w,r.h,12);g.stroke();g.shadowBlur=0;
 g.lineWidth=5;g.strokeStyle='#fff';const L=Math.min(26,r.w/3,r.h/3);
 [[r.x,r.y,1,1],[r.x+r.w,r.y,-1,1],[r.x,r.y+r.h,1,-1],[r.x+r.w,r.y+r.h,-1,-1]].forEach(([x,y,sx,sy])=>{g.beginPath();g.moveTo(x,y+sy*L);g.lineTo(x,y);g.lineTo(x+sx*L,y);g.stroke()});
 g.restore();
 [P.a,P.b].forEach((p,i)=>{if(manual&&!HAND.found)return;g.fillStyle=i?'#22d3ee':'#ffb454';g.beginPath();g.arc(p.x,p.y,7+3*pulse,0,7);g.fill();g.strokeStyle='#fff';g.lineWidth=2;g.stroke()});
}
function labelChip(r,f){
 const t=f.e+' '+f.n+'　'+Math.round(r.w)+'×'+Math.round(r.h);g.font='bold 15px system-ui,"Noto Sans TC"';
 const w=g.measureText(t).width+22,x=clamp(r.x,4,view.width-w-4),y=Math.max(30,r.y-8);
 g.fillStyle='rgba(10,14,28,.78)';rr(g,x,y-24,w,28,14);g.fill();g.fillStyle='#fff';g.fillText(t,x+11,y-5);
}
function drawSkeleton(){
 if(!cfg.skel||!HAND.found)return;
 g.lineWidth=2;g.strokeStyle='rgba(120,255,190,.55)';g.beginPath();
 for(const[a,b]of BONES){const p=mapPt(HAND.u[a],HAND.v[a]),q=mapPt(HAND.u[b],HAND.v[b]);g.moveTo(p[0],p[1]);g.lineTo(q[0],q[1])}g.stroke();
 for(let i=0;i<21;i++){const p=mapPt(HAND.u[i],HAND.v[i]);g.fillStyle=(i===4||i===8)?'#fff':'rgba(255,90,120,.85)';g.beginPath();g.arc(p[0],p[1],(i===4||i===8)?5:3,0,7);g.fill()}
}
function drawTrail(now){
 g.lineCap='round';g.lineJoin='round';
 for(let i=1;i<trail.length;i++){const a=trail[i-1],b=trail[i],age=(now-b.t)/2600;if(Math.hypot(a.x-b.x,a.y-b.y)>140)continue;
  g.strokeStyle='hsla('+b.h+',95%,62%,'+(1-age)+')';g.shadowColor='hsl('+b.h+',95%,60%)';g.shadowBlur=16;g.lineWidth=14*(1-age*.7)+2;
  g.beginPath();g.moveTo(a.x,a.y);g.lineTo(b.x,b.y);g.stroke()}
 g.shadowBlur=0;
 if(HAND.found&&P.ready){g.fillStyle='hsl('+hue+',95%,70%)';g.beginPath();g.arc(P.b.x,P.b.y,10,0,7);g.fill()}
}
function render(now){
 if(cam.naturalWidth&&(cam.naturalWidth!==nw||cam.naturalHeight!==nh)){nw=cam.naturalWidth;nh=cam.naturalHeight;view.width=Math.round(nw*1.5);view.height=Math.round(nh*1.5);}
 g.save();
 if(cfg.mirror){g.translate(view.width,0);g.scale(-1,1)}
 if(cam.naturalWidth)g.drawImage(cam,0,0,view.width,view.height);
 else{g.fillStyle='#0a0f1f';g.fillRect(0,0,view.width,view.height)}
 const sc=view.width/nw;let rect=null;
 if(cfg.mode==='filter'){
  const r=currentRect();
  if(r){rect={x:r.x,y:r.y,w:r.w,h:r.h};
   // 畫面上的矩形是放大過的座標，濾鏡取樣要換回影像原始尺寸
   const src={x:rect.x/sc,y:rect.y/sc,w:rect.w/sc,h:rect.h/sc};
   g.save();rr(g,rect.x,rect.y,rect.w,rect.h,12);g.clip();
   renderRegionScaled(g,FMAP[cfg.filter],src,rect);g.restore();drawFrameDeco(rect,FMAP[cfg.filter],now)}
 }else drawTrail(now);
 drawSkeleton();
 g.restore();
 // 文字不鏡像
 if(rect){const f=FMAP[cfg.filter];const rr2=cfg.mirror?{x:view.width-rect.x-rect.w,y:rect.y,w:rect.w,h:rect.h}:rect;labelChip(rr2,f)}
 updateCapture(now,rect?{x:rect.x/sc,y:rect.y/sc,w:rect.w/sc,h:rect.h/sc}:null);
 const hasHand=HAND.found&&now-HAND.t<600;
 $('hintTx').textContent=hasHand?(cfg.mode==='filter'?(pinched?'捏合中… 放開就換下一個濾鏡':'拇指尖 ↔ 食指尖拉出取景框；停住自動拍照'):'食指尖畫畫；捏一下換顏色，捏住不放清空')
  :(manual?'滑鼠拉出的取景框（有手時會自動改用手）':'把手伸進鏡頭，或用滑鼠在畫面上拖曳試玩');
}
// 矩形在畫面上的位置（放大座標）與影像取樣區域（原始座標）分開處理
function renderRegionScaled(c,f,src,dst){
 const sx=dst.w/src.w,sy=dst.h/src.h;c.save();c.translate(dst.x,dst.y);c.scale(sx,sy);renderRegion(c,f,src,0,0);c.restore();
}

/* ---- 滑鼠備援 ---- */
function evPos(e){const b=view.getBoundingClientRect();let x=(e.clientX-b.left)/b.width*view.width;if(cfg.mirror)x=view.width-x;return{x,y:(e.clientY-b.top)/b.height*view.height}}
stage.addEventListener('pointerdown',e=>{if(e.target.closest('.tabs'))return;drag=evPos(e);manual={x:drag.x,y:drag.y,w:1,h:1};P.a={...drag};P.b={...drag};stage.setPointerCapture(e.pointerId)});
stage.addEventListener('pointermove',e=>{if(!drag)return;const p=evPos(e);manual={x:Math.min(drag.x,p.x),y:Math.min(drag.y,p.y),w:Math.abs(p.x-drag.x),h:Math.abs(p.y-drag.y)};P.a={x:manual.x,y:manual.y};P.b={x:manual.x+manual.w,y:manual.y+manual.h}});
stage.addEventListener('pointerup',()=>{drag=null;if(manual&&(manual.w<40||manual.h<40))manual=null});
addEventListener('keydown',e=>{if(e.key==='ArrowRight')cycleFilter(1);else if(e.key==='ArrowLeft')cycleFilter(-1)});

/* ---- 連線 ---- */
let snapN=0,snapT=0;function nextSnap(){snapT=performance.now();cam.src='/snap?'+(snapN++)}
cam.onload=()=>setTimeout(nextSnap,Math.max(0,60-(performance.now()-snapT)));cam.onerror=()=>setTimeout(nextSnap,600);nextSnap();
function connect(){
 const es=new EventSource('/events');
 es.onopen=()=>{$('cConn').textContent='已連線';$('cConn').className='pill on'};
 es.onerror=()=>{$('cConn').textContent='重新連線中…';$('cConn').className='pill off'};
 es.onmessage=e=>{const p=e.data.split(',');if(p[0]==='0'){onHand(false);return}if(p.length<43)return;
  const u=new Float32Array(21),v=new Float32Array(21);for(let i=0;i<21;i++){u[i]=p[1+i]/1000;v[i]=p[22+i]/1000}onHand(true,u,v)};
 es.addEventListener('func',()=>{if(cfg.mode==='filter')cycleFilter(1);else{hue=(hue+55)%360}});
}
connect();
setInterval(()=>{$('cRate').textContent=HAND.msgs+' fps';HAND.msgs=0;
 const h=HAND.found&&performance.now()-HAND.t<600;$('cHand').textContent=h?'偵測到手':'未偵測到手';$('cHand').className='pill'+(h?' on':'')},1000);

let last=performance.now();
function loop(now){const dt=Math.min(.06,(now-last)/1000);last=now;updateHand(dt,now);updateTrail(now);render(now);requestAnimationFrame(loop)}
requestAnimationFrame(loop);
</script></body></html>)HTML";

#endif
