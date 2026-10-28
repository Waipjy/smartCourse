#ifndef __GAME_PAGE_H__
#define __GAME_PAGE_H__

/*
  「兩指跑酷」网页（板子上的 HTTP 伺服器回传这一整页）。

  游戏逻辑与画面全部在浏览器里执行（canvas），板子只负责两件事：
    /snap    -> 一张 JPEG 快照（网页连续要，当作画中画影像）
    /events  -> Server-Sent Events，每处理完一帧推送 21 个手部关键点

  用 C++11 原始字串，里面的引号、反斜线都不用跳脱。
  要开发 / 调整这个页面，可以把 R"HTML( 和 )HTML" 中间的内容存成 .html 档，
  搭配一个会吐假关键点的本机伺服器在电脑上直接测试，不必每次都烧录。
*/

static const char GAME_PAGE[] = R"HTML(<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>兩指跑酷</title>
<style>
:root{--bg:#eef2f6;--card:#fff;--tx:#1d2733;--mu:#6b7785;--ac:#1f7aec;--ok:#1b9e5a;--bad:#d64545;--bd:#d9e0e8}
@media(prefers-color-scheme:dark){:root{--bg:#0f141a;--card:#19212a;--tx:#e6ebf0;--mu:#8b97a5;--ac:#5aa2ff;--ok:#3ec27e;--bad:#ff6b6b;--bd:#2a3541}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.5 system-ui,"Noto Sans TC",sans-serif;padding:12px}
main{max-width:980px;margin:0 auto;display:grid;gap:10px}
header{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px}h1{font-size:19px;margin:0}
.chips{display:flex;gap:6px;flex-wrap:wrap}.pill{font-size:12px;padding:2px 10px;border-radius:99px;background:var(--bd);color:var(--mu)}
.pill.on{background:var(--ok);color:#fff}.pill.off{background:var(--bad);color:#fff}
#stage{position:relative;border-radius:14px;overflow:hidden;background:#000;aspect-ratio:16/9;box-shadow:0 4px 18px rgba(0,0,0,.25)}
#game{width:100%;height:100%;display:block}
#pip{position:absolute;right:10px;top:10px;width:26%;aspect-ratio:16/9;border:2px solid rgba(255,255,255,.8);border-radius:8px;overflow:hidden;background:#111;opacity:.95}
#pip img,#pip canvas{position:absolute;inset:0;width:100%;height:100%}
#pip img{object-fit:fill}
.bar{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
button{padding:8px 14px;border:0;border-radius:8px;background:var(--ac);color:#fff;font-size:14px;cursor:pointer}
button.ghost{background:var(--bd);color:var(--tx)}
label{font-size:13px;color:var(--mu);display:flex;gap:4px;align-items:center}
select{padding:5px;border-radius:6px;border:1px solid var(--bd);background:var(--card);color:var(--tx)}
details{background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:8px 12px;font-size:14px}
summary{cursor:pointer;color:var(--mu)}li{margin:3px 0}
#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);background:var(--tx);color:var(--bg);padding:8px 16px;border-radius:99px;opacity:0;transition:.2s;pointer-events:none;z-index:9}
#toast.show{opacity:.92}
</style></head><body><main>
<header><h1>🏃 兩指跑酷</h1>
<div class="chips"><span id="cConn" class="pill">連線中…</span><span id="cHand" class="pill">未偵測到手</span><span id="cRate" class="pill">0 fps</span></div></header>
<div id="stage"><canvas id="game" width="960" height="540"></canvas>
<div id="pip"><img id="cam" alt=""><canvas id="ov" width="320" height="180"></canvas></div></div>
<div class="bar"><button id="bStart">開始 / 重來</button><button id="bCal" class="ghost">校準桌面</button>
<label><input type="checkbox" id="kSnd" checked>音效</label>
<label><input type="checkbox" id="kPip" checked>鏡頭畫面</label>
<label><input type="checkbox" id="kMir">鏡像</label>
<label>手部座標對應 <select id="sMap"><option value="crop">置中方形</option><option value="stretch">拉伸全幅</option></select></label></div>
<details><summary>怎麼玩（點開）</summary><ul>
<li>鏡頭對著桌面前方，手放在桌上，比「YA」：<b style="color:#ff8a3d">食指</b>和<b style="color:#2fc9e8">中指</b>就是角色的兩條腿。</li>
<li><b>跑步</b>：兩指輪流「抬起 → 放下」像走路，越快跑越快。畫面左下的兩根長條就是指尖離桌面的高度。</li>
<li><b>跳躍</b>：兩指同時抬高，抬得越高跳得越高、停在空中越久。</li>
<li><b>衝刺</b>：把兩指張開成大「V」，消耗能量（吃金幣補充）；衝刺時無敵，還能撞碎障礙。</li>
<li>後面的怪物會一直追上來，停下來或撞到障礙就會被追上。</li>
<li>桌面高度會自動學習；若判斷不準，把兩指平放桌面按「校準桌面」。</li>
<li>沒有攝影機也能試玩：鍵盤 <b>F</b>＝食指、<b>J</b>＝中指（輪流按來跑）、<b>F+J</b>＝跳、<b>空白鍵</b>按住＝張開衝刺。</li></ul></details>
</main><div id="toast"></div>
<script>
'use strict';
const $=i=>document.getElementById(i);
const cv=$('game'),ctx=cv.getContext('2d'),ov=$('ov'),octx=ov.getContext('2d');
const W=960,H=540,GROUND=430,PXM=48,PXV=70,RX=300;
const C_IDX='#ff8a3d',C_MID='#2fc9e8';
const clamp=(x,a,b)=>Math.max(a,Math.min(b,x));
const rnd=(a,b)=>a+Math.random()*(b-a);
let tt;function toast(m){const t=$('toast');t.textContent=m;t.className='show';clearTimeout(tt);tt=setTimeout(()=>t.className='',2200)}

/* ---------- 設定（存在瀏覽器） ---------- */
const store={get(k,d){try{const v=localStorage.getItem('fr_'+k);return v===null?d:JSON.parse(v)}catch(e){return d}},
 set(k,v){try{localStorage.setItem('fr_'+k,JSON.stringify(v))}catch(e){}}};
const cfg={sound:store.get('sound',true),map:store.get('map','crop'),mirror:store.get('mirror',false),pip:store.get('pip',true),best:store.get('best',0)};
$('kSnd').checked=cfg.sound;$('kPip').checked=cfg.pip;$('kMir').checked=cfg.mirror;$('sMap').value=cfg.map;
$('kSnd').onchange=e=>{cfg.sound=e.target.checked;store.set('sound',cfg.sound)};
$('kPip').onchange=e=>{cfg.pip=e.target.checked;store.set('pip',cfg.pip);applyPip()};
$('kMir').onchange=e=>{cfg.mirror=e.target.checked;store.set('mirror',cfg.mirror)};
$('sMap').onchange=e=>{cfg.map=e.target.value;store.set('map',cfg.map)};
function applyPip(){$('pip').style.display=cfg.pip?'block':'none'}applyPip();

/* ---------- 音效 ---------- */
let AC=null;
function snd(f,d,type,vol,slide){if(!cfg.sound)return;try{AC=AC||new(window.AudioContext||window.webkitAudioContext)();
 if(AC.state==='suspended')AC.resume();const o=AC.createOscillator(),g=AC.createGain(),n=AC.currentTime;
 o.type=type||'square';o.frequency.setValueAtTime(f,n);if(slide)o.frequency.exponentialRampToValueAtTime(slide,n+d);
 g.gain.setValueAtTime(vol||.04,n);g.gain.exponentialRampToValueAtTime(.0001,n+d);o.connect(g);g.connect(AC.destination);o.start();o.stop(n+d)}catch(e){}}

/* ---------- 感測：手部關鍵點 -> 兩指與桌面的幾何量 ---------- */
// 關鍵點：0 手腕、5/9/13/17 各指根、8 食指尖、12 中指尖。座標 0~1，對應 NN 影像的方形區域。
const UP_ON=.28,UP_OFF=.12,SPREAD_ON=.62,SPREAD_OFF=.45;
const SEN={hand:false,t:0,u:new Float32Array(21),v:new Float32Array(21),scale:.2,desk:.8,deskLock:null,
 hist:[],target:[0,0],spread:0,calib:null,msgs:0,rate:0,last:0};
function onFrame(found,u,v){
 const now=performance.now();SEN.msgs++;SEN.last=now;
 if(!found){SEN.hand=false;return}
 SEN.hand=true;SEN.t=now;SEN.u.set(u);SEN.v.set(v);
 const sc=clamp(Math.hypot(u[0]-u[9],v[0]-v[9]),.06,.5);SEN.scale=sc;
 const ty=[v[8],v[12]],low=Math.max(ty[0],ty[1]);
 // 桌面高度：最近 3 秒指尖到過的最低位置（走路時指尖會一直碰到桌面）
 SEN.hist.push([now,low]);while(SEN.hist.length&&now-SEN.hist[0][0]>3000)SEN.hist.shift();
 let m=0;for(const h of SEN.hist)if(h[1]>m)m=h[1];SEN.desk=m;
 if(SEN.calib){SEN.calib.s.push(low);if(now-SEN.calib.t0>1000){
   SEN.deskLock=SEN.calib.s.reduce((a,b)=>a+b,0)/SEN.calib.s.length;SEN.calib=null;toast('桌面已校準 ✔')}}
 const desk=SEN.deskLock!==null?SEN.deskLock:SEN.desk;
 SEN.target=ty.map(y=>Math.max(0,(desk-y)/sc));      // 指尖離桌面的高度（以手掌長度為單位）
 SEN.spread=Math.hypot(u[8]-u[12],v[8]-v[12])/sc;    // 兩指尖距離（以手掌長度為單位）
 drawPip();
}
function calibrate(){
 if(!SEN.hand){toast('沒有偵測到手，請把手放進鏡頭');return}
 SEN.calib={t0:performance.now(),s:[]};toast('校準中… 兩指請平放在桌面 1 秒');
}
$('bCal').onclick=calibrate;

/* ---------- 鍵盤（沒有攝影機時的試玩 / 開發用） ---------- */
const KB={f:false,j:false,s:false,t:0};
addEventListener('keydown',e=>{const k=e.key.toLowerCase();let hit=true;
 if(k==='f')KB.f=true;else if(k==='j')KB.j=true;else if(k===' ')KB.s=true;else if(k==='r'||k==='enter')startGame();else hit=false;
 if(hit){KB.t=performance.now();e.preventDefault()}});
addEventListener('keyup',e=>{const k=e.key.toLowerCase();if(k==='f')KB.f=false;else if(k==='j')KB.j=false;else if(k===' ')KB.s=false});

/* ---------- 統一輸入（遊戲只看這裡） ---------- */
const IN={lift:[0,0],spread:0,src:'none'};
function pollInput(dt,now){
 let tl=[0,0],sp=0.3,src='none';
 if(SEN.hand&&now-SEN.t<500){tl=SEN.target;sp=SEN.spread;src='hand'}
 else if(KB.f||KB.j||KB.s||now-KB.t<1500){tl=[KB.f?.85:0,KB.j?.85:0];sp=KB.s?.95:.3;src='kbd'}
 const k=Math.min(1,dt*(src==='kbd'?22:16));
 IN.lift[0]+=(tl[0]-IN.lift[0])*k;IN.lift[1]+=(tl[1]-IN.lift[1])*k;IN.spread+=(sp-IN.spread)*k;IN.src=src;
}

/* ---------- 遊戲狀態 ---------- */
const G={state:'ready',t:0,dist:0,coins:0,smash:0,energy:40,v:0,gap:14,chaser:3,y:0,vy:0,ground:true,
 dash:0,spreadT:0,hit:0,shake:0,up:[false,false],both:false,foot:-1,steps:[],obs:[],pick:[],part:[],nextSpawn:12,cad:0,over:0};
const score=()=>Math.floor(G.dist)+G.coins*10+G.smash*5;
function startGame(){
 Object.assign(G,{state:'run',t:0,dist:0,coins:0,smash:0,energy:40,v:0,gap:14,chaser:3,y:0,vy:0,ground:true,dash:0,spreadT:0,
  hit:0,shake:0,foot:-1,steps:[],obs:[],pick:[],part:[],nextSpawn:12,cad:0});
 G.up=[IN.lift[0]>UP_ON,IN.lift[1]>UP_ON];G.both=G.up[0]&&G.up[1];snd(520,.12,'triangle',.05,880);
}
$('bStart').onclick=()=>{startGame();AC&&AC.resume&&AC.resume()};
function burst(x,y,n,col,sp){for(let i=0;i<n;i++)G.part.push({x,y,vx:rnd(-1,1)*sp,vy:rnd(-1.2,.2)*sp,l:rnd(.3,.7),c:col})}

function touchdown(i){
 if(G.state==='ready'){startGame();return}
 if(G.state!=='run')return;
 const now=performance.now();
 G.v=Math.min(9.5,G.v+(G.foot===i?.25:1.15));G.foot=i;G.steps.push(now);
 if(G.ground)burst(RX+(i?-6:6),GROUND,3,'#d8c9a3',90);
 snd(i?300:360,.04,'square',.02);
}
function spawn(){
 const x=G.dist+34,tm=G.t,r=Math.random();
 let type=r<.45?'cone':(r<.7?'crate':(tm>8?'pit':'cone'));
 const o={x,type,w:type==='pit'?3.4:(type==='crate'?1.5:.9),h:type==='crate'?1.6:(type==='cone'?1.05:0),hit:false};
 G.obs.push(o);
 if(Math.random()<.55){const hgt=o.type==='pit'?1.3:(o.h+.9);for(let i=0;i<5;i++)G.pick.push({x:x-1.1+i*(o.w+2.2)/4,y:hgt+Math.sin(i/4*Math.PI)*.7,got:false})}
 else if(Math.random()<.5){for(let i=0;i<3;i++)G.pick.push({x:x+5+i*1.1,y:.35,got:false})}
 G.nextSpawn=G.dist+rnd(15,24)-Math.min(6,tm*.06);
}
function stumble(){G.v*=.35;G.gap-=2.6;G.hit=.5;G.shake=.35;snd(140,.25,'sawtooth',.06,60)}
function gameOver(){G.state='over';G.over=performance.now();snd(300,.5,'sawtooth',.06,50);
 if(score()>cfg.best){cfg.best=score();store.set('best',cfg.best)}}

function update(dt){
 const now=performance.now();
 // 手指狀態（遲滯）+ 事件
 for(let i=0;i<2;i++){
  if(!G.up[i]&&IN.lift[i]>UP_ON)G.up[i]=true;
  else if(G.up[i]&&IN.lift[i]<UP_OFF){G.up[i]=false;touchdown(i)}
 }
 const both=G.up[0]&&G.up[1],jumpReq=both&&!G.both;G.both=both;
 const mean=(IN.lift[0]+IN.lift[1])/2;
 if(G.state==='over'&&both&&now-G.over>1500){startGame();return}
 if(G.state!=='run'){G.hit=Math.max(0,G.hit-dt);G.shake=Math.max(0,G.shake-dt);stepParts(dt);return}

 G.t+=dt;G.hit=Math.max(0,G.hit-dt);G.shake=Math.max(0,G.shake-dt);
 while(G.steps.length&&now-G.steps[0]>1200)G.steps.shift();G.cad=G.steps.length/1.2;
 G.v*=Math.exp(-.45*dt);if(G.v<.03)G.v=0;
 G.chaser=3+Math.min(4.8,G.t*.035);

 // 跳躍：兩指同時抬起；抬得越高，起跳越強、滯空越久
 if(jumpReq&&G.ground){G.vy=7.4+3*clamp(mean-.28,0,1);G.ground=false;snd(380,.18,'square',.04,760);burst(RX,GROUND,6,'#d8c9a3',110)}
 if(!G.ground){let g=22;if(G.vy>0&&mean>.5)g*=.62;G.vy-=g*dt;G.y+=G.vy*dt;
  if(G.y<=0){G.y=0;G.vy=0;G.ground=true;burst(RX,GROUND,6,'#d8c9a3',110)}}

 // 衝刺：兩指張成大 V
 if(IN.spread>SPREAD_ON&&G.dash<=0)G.spreadT+=dt;else if(IN.spread<SPREAD_OFF)G.spreadT=0;
 if(G.spreadT>.15&&G.energy>=25&&G.dash<=0){G.dash=1.4;G.energy-=25;G.spreadT=0;snd(300,.4,'sawtooth',.05,1100)}
 G.dash=Math.max(0,G.dash-dt);
 const eff=G.dash>0?Math.max(G.v,5.5)*1.6:G.v;
 G.dist+=eff*dt;G.gap=Math.min(28,G.gap+(eff-G.chaser)*dt);G.energy=Math.min(100,G.energy+eff*dt*.35);

 if(G.dist>G.nextSpawn)spawn();
 const px0=G.dist-.3,px1=G.dist+.3;
 for(const o of G.obs){
  if(o.hit)continue;
  if(px1>o.x&&px0<o.x+o.w){
   if(G.dash>0){o.hit=true;G.smash++;burst(RX+30,GROUND-30,14,'#ffb347',200);snd(200,.15,'square',.05,90);continue}
   if(o.type==='pit'){if(G.y<.15&&px0>o.x-.1&&px1<o.x+o.w+.1){o.hit=true;stumble()}}
   else if(G.y<o.h){o.hit=true;stumble()}
  }
 }
 for(const p of G.pick){if(p.got)continue;
  if(Math.abs(p.x-G.dist)<.7&&Math.abs(p.y-G.y-.8)<1){p.got=true;G.coins++;G.energy=Math.min(100,G.energy+8);
   burst(RX,GROUND-(p.y)*PXV,6,'#ffd84a',120);snd(900,.08,'square',.03,1400)}}
 G.obs=G.obs.filter(o=>o.x+o.w>G.dist-14);G.pick=G.pick.filter(p=>p.x>G.dist-14&&!p.got);
 stepParts(dt);
 if(G.gap<=0)gameOver();
}
function stepParts(dt){for(const p of G.part){p.x+=p.vx*dt;p.y+=p.vy*dt;p.vy+=380*dt;p.l-=dt}G.part=G.part.filter(p=>p.l>0)}

/* ---------- 繪圖 ---------- */
function hill(off,amp,base,col,k){ctx.fillStyle=col;ctx.beginPath();ctx.moveTo(0,H);
 for(let x=0;x<=W;x+=16)ctx.lineTo(x,base-amp*(.5+.5*Math.sin((x+off)*k)+.3*Math.sin((x+off)*k*2.3)));
 ctx.lineTo(W,H);ctx.closePath();ctx.fill()}
function drawBackground(){
 const sg=ctx.createLinearGradient(0,0,0,GROUND);sg.addColorStop(0,'#5db7f5');sg.addColorStop(1,'#ffe6a6');
 ctx.fillStyle=sg;ctx.fillRect(0,0,W,H);
 ctx.fillStyle='rgba(255,245,200,.95)';ctx.beginPath();ctx.arc(790,110,46,0,7);ctx.fill();
 const d=G.dist*PXM;
 ctx.fillStyle='rgba(255,255,255,.85)';
 for(let i=0;i<5;i++){const cx=((i*260-d*.08)%1300+1300)%1300-170,cy=70+(i*47)%90;
  ctx.beginPath();ctx.arc(cx,cy,24,0,7);ctx.arc(cx+28,cy+6,30,0,7);ctx.arc(cx+62,cy,22,0,7);ctx.fill()}
 hill(d*.12,90,GROUND-30,'#8cc9a5',.006);hill(d*.28,70,GROUND-5,'#5fae7c',.009);
 // 地面
 ctx.fillStyle='#7cc15a';ctx.fillRect(0,GROUND,W,14);ctx.fillStyle='#b98b56';ctx.fillRect(0,GROUND+14,W,H);
 ctx.fillStyle='rgba(0,0,0,.10)';
 for(let x=-((d)%90);x<W;x+=90)ctx.fillRect(x,GROUND+34,46,6);
 for(let x=-((d*1.0+45)%130);x<W;x+=130)ctx.fillRect(x,GROUND+62,60,6);
}
function sx(wx){return RX+(wx-G.dist)*PXM}
function drawObstacles(){
 for(const o of G.obs){
  if(o.hit&&o.type!=='pit')continue;
  const x=sx(o.x),w=o.w*PXM;if(x>W+60||x+w<-60)continue;
  if(o.type==='pit'){ctx.fillStyle='#3b2a1a';ctx.fillRect(x,GROUND,w,H-GROUND);
   ctx.fillStyle='#7cc15a';ctx.fillRect(x-4,GROUND,6,14);ctx.fillRect(x+w-2,GROUND,6,14)}
  else if(o.type==='crate'){const h=o.h*PXV;ctx.fillStyle='#c98a3c';ctx.fillRect(x,GROUND-h,w,h);
   ctx.strokeStyle='#7a4f1c';ctx.lineWidth=4;ctx.strokeRect(x+2,GROUND-h+2,w-4,h-4);
   ctx.beginPath();ctx.moveTo(x+4,GROUND-h+4);ctx.lineTo(x+w-4,GROUND-4);ctx.moveTo(x+w-4,GROUND-h+4);ctx.lineTo(x+4,GROUND-4);ctx.stroke()}
  else{const h=o.h*PXV;ctx.fillStyle='#ff6a3d';ctx.beginPath();ctx.moveTo(x,GROUND);ctx.lineTo(x+w/2,GROUND-h);ctx.lineTo(x+w,GROUND);ctx.closePath();ctx.fill();
   ctx.fillStyle='#fff';ctx.fillRect(x+w*.27,GROUND-h*.55,w*.46,7);ctx.fillRect(x+w*.36,GROUND-h*.3,w*.28,6)}
 }
 for(const p of G.pick){const x=sx(p.x),y=GROUND-p.y*PXV;if(x<-20||x>W+20)continue;
  const s=Math.abs(Math.cos(performance.now()/260+p.x));
  ctx.fillStyle='#ffd84a';ctx.beginPath();ctx.ellipse(x,y,11*s+3,12,0,0,7);ctx.fill();ctx.strokeStyle='#d99a00';ctx.lineWidth=2;ctx.stroke()}
}
function leg(hx,hy,l,col){
 const a=clamp(l,0,1.4),th=a*.95,sh=th-a*1.5,L=34;
 const kx=hx+Math.sin(th)*L,ky=hy+Math.cos(th)*L,fx=kx+Math.sin(sh)*L,fy=ky+Math.cos(sh)*L;
 ctx.lineCap='round';ctx.strokeStyle='#1e3a8a';ctx.lineWidth=10;ctx.beginPath();ctx.moveTo(hx,hy);ctx.lineTo(kx,ky);ctx.lineTo(fx,fy);ctx.stroke();
 ctx.fillStyle=col;ctx.beginPath();ctx.ellipse(fx+5,fy+2,13,7,0,0,7);ctx.fill();
}
function drawRunner(){
 const fy=GROUND-G.y*PXV,hipY=fy-66,x=RX;
 ctx.fillStyle='rgba(0,0,0,.18)';ctx.beginPath();ctx.ellipse(x,GROUND+8,26-G.y*10,6,0,0,7);ctx.fill();
 const flick=G.hit>0&&Math.floor(G.hit*20)%2===0;if(flick)ctx.globalAlpha=.4;
 if(G.dash>0){ctx.fillStyle='rgba(255,200,60,.35)';for(let i=1;i<5;i++){ctx.beginPath();ctx.ellipse(x-i*26,hipY-10,26,40,0,0,7);ctx.fill()}}
 leg(x,hipY,IN.lift[1],C_MID);                                   // 後腿＝中指
 const sw=(IN.lift[0]-IN.lift[1])*.8;                            // 手臂跟著兩腿反向擺
 ctx.strokeStyle='#ffe0bd';ctx.lineWidth=8;ctx.lineCap='round';
 ctx.beginPath();ctx.moveTo(x,hipY-34);ctx.lineTo(x-Math.sin(sw)*26,hipY-34+Math.cos(sw)*26);ctx.stroke();
 ctx.fillStyle='#3b82f6';ctx.beginPath();ctx.roundRect?ctx.roundRect(x-15,hipY-44,30,50,10):ctx.rect(x-15,hipY-44,30,50);ctx.fill();
 leg(x,hipY,IN.lift[0],C_IDX);                                   // 前腿＝食指
 ctx.strokeStyle='#ffe0bd';ctx.beginPath();ctx.moveTo(x,hipY-34);ctx.lineTo(x+Math.sin(sw)*26,hipY-34+Math.cos(sw)*26);ctx.stroke();
 ctx.fillStyle='#ffe0bd';ctx.beginPath();ctx.arc(x+2,hipY-62,17,0,7);ctx.fill();
 ctx.fillStyle='#4b2e1a';ctx.beginPath();ctx.arc(x,hipY-67,17,Math.PI,0);ctx.fill();
 ctx.fillStyle='#222';ctx.beginPath();ctx.arc(x+9,hipY-62,2.6,0,7);ctx.fill();
 ctx.globalAlpha=1;
}
function drawChaser(){
 const x=RX-G.gap*PXM,r=78;
 if(x<-r){ // 還在畫面外：左邊緣顯示警告
  const a=clamp(1-G.gap/14,.1,1);ctx.fillStyle='rgba(214,69,69,'+a*.5+')';ctx.fillRect(0,0,26,H);return}
 const t=performance.now()/120;
 ctx.fillStyle='#4a1f6e';ctx.beginPath();
 for(let i=0;i<14;i++){const ang=i/14*Math.PI*2,rr=r*(i%2?.82:1.08);ctx.lineTo(x+Math.cos(ang)*rr,GROUND-r+Math.sin(ang)*rr+Math.sin(t+i)*2)}
 ctx.closePath();ctx.fill();
 ctx.fillStyle='#fff';ctx.beginPath();ctx.arc(x+22,GROUND-r-14,15,0,7);ctx.arc(x+58,GROUND-r-14,15,0,7);ctx.fill();
 ctx.fillStyle='#d11';ctx.beginPath();ctx.arc(x+28,GROUND-r-12,6,0,7);ctx.arc(x+62,GROUND-r-12,6,0,7);ctx.fill();
 ctx.fillStyle='#200';ctx.beginPath();ctx.arc(x+40,GROUND-r+26,24,0,Math.PI);ctx.fill();
}
function drawParts(){for(const p of G.part){ctx.globalAlpha=clamp(p.l*2,0,1);ctx.fillStyle=p.c;ctx.fillRect(p.x-2,p.y-2,5,5)}ctx.globalAlpha=1}
function bar(x,y,w,h,f,col,bg){ctx.fillStyle=bg||'rgba(0,0,0,.35)';ctx.fillRect(x,y,w,h);ctx.fillStyle=col;ctx.fillRect(x,y,w*clamp(f,0,1),h)}
function txt(s,x,y,size,col,al){ctx.font='bold '+size+'px system-ui,"Noto Sans TC",sans-serif';ctx.fillStyle=col||'#fff';ctx.textAlign=al||'left';ctx.fillText(s,x,y)}
function drawHud(){
 ctx.save();ctx.shadowColor='rgba(0,0,0,.45)';ctx.shadowBlur=4;
 txt(score()+' 分',22,40,30);txt(Math.floor(G.dist)+' m　🪙 '+G.coins,22,68,18,'#fff');
 txt('最高 '+cfg.best,22,92,15,'rgba(255,255,255,.85)');
 ctx.restore();
 // 追擊者距離
 txt('怪物距離',W/2-90,26,14,'#fff','left');
 bar(W/2-90,32,180,12,G.gap/20,G.gap<4?'#ff5252':'#ffd84a');
 // 左下：兩指離桌面的高度（幾何量的即時顯示）
 const bx=24,by=H-24,bh=120;
 ctx.fillStyle='rgba(0,0,0,.35)';ctx.fillRect(bx-10,by-bh-30,150,bh+52);
 [[0,C_IDX,'食指'],[1,C_MID,'中指']].forEach(([i,c,n])=>{
  const x=bx+i*54;ctx.fillStyle='rgba(255,255,255,.18)';ctx.fillRect(x,by-bh,34,bh);
  const f=clamp(IN.lift[i]/1.2,0,1);ctx.fillStyle=c;ctx.fillRect(x,by-bh*f,34,bh*f);
  txt(n,x+17,by+16,13,'#fff','center')});
 ctx.fillStyle='rgba(255,255,255,.9)';
 [UP_ON].forEach(t=>{const y=by-bh*clamp(t/1.2,0,1);ctx.fillRect(bx-4,y,112,2)});
 txt('抬起線',bx+112,by-bh*UP_ON/1.2+4,10,'rgba(255,255,255,.9)','left');
 // 右下：能量 / 步頻
 const ex=W-230,ey=H-60;
 txt('能量　'+(G.energy>=25?'張開 V 衝刺！':'吃金幣補充'),ex,ey-8,14,'#fff');
 bar(ex,ey,200,14,G.energy/100,G.dash>0?'#ff9f1a':'#4cd964');
 txt('步頻 '+G.cad.toFixed(1)+' 步/秒　速度 '+G.v.toFixed(1)+' m/s',ex,ey+34,14,'#fff');
}
function drawOverlay(){
 if(G.state==='run')return;
 ctx.fillStyle='rgba(8,14,24,.62)';ctx.fillRect(0,0,W,H);
 if(G.state==='ready'){
  txt('兩指跑酷',W/2,170,60,'#fff','center');
  txt('食指＋中指當兩條腿：輪流抬起、放下就能跑',W/2,235,24,'#ffe9a8','center');
  txt('兩指一起抬高＝跳　　張成 V＝衝刺',W/2,272,22,'#fff','center');
  const hand=IN.src!=='none';
  txt(hand?'準備好了！放下一根手指（或按 F / J、點「開始」）就開跑':'把手放進鏡頭，比 YA 放在桌上（或按 F / J 用鍵盤試玩）',W/2,340,22,hand?'#9bffb0':'#ffb3b3','center');
 }else if(G.state==='over'){
  txt('被追上了！',W/2,170,56,'#ff8a8a','center');
  txt(score()+' 分　（'+Math.floor(G.dist)+' m，金幣 '+G.coins+'）',W/2,235,30,'#fff','center');
  txt(score()>=cfg.best&&score()>0?'🎉 新紀錄！':'最高 '+cfg.best,W/2,276,22,'#ffe9a8','center');
  txt('兩指同時抬高（或按 R、點「重來」）再跑一次',W/2,340,22,'#fff','center');
 }
}
function render(){
 ctx.save();
 if(G.shake>0)ctx.translate(rnd(-1,1)*G.shake*14,rnd(-1,1)*G.shake*10);
 drawBackground();drawObstacles();drawChaser();drawRunner();drawParts();
 ctx.restore();
 drawHud();drawOverlay();
}

/* ---------- 畫中畫：鏡頭畫面 + 手部骨架 + 桌面線 ---------- */
const BONES=[[0,1],[1,2],[2,3],[3,4],[0,5],[5,6],[6,7],[7,8],[5,9],[9,10],[10,11],[11,12],[9,13],[13,14],[14,15],[15,16],[13,17],[17,18],[18,19],[19,20],[0,17]];
function mapPt(u,v){ // NN 的方形區域 -> 鏡頭畫面座標（0~1）
 const asp=($('cam').naturalWidth&&$('cam').naturalHeight)?$('cam').naturalWidth/$('cam').naturalHeight:16/9;
 let x=cfg.map==='crop'?.5+(u-.5)/asp:u;if(cfg.mirror)x=1-x;return[x*ov.width,v*ov.height]}
function drawPip(){
 if(!cfg.pip)return;
 octx.clearRect(0,0,ov.width,ov.height);if(!SEN.hand)return;
 octx.lineWidth=2;octx.strokeStyle='rgba(120,255,160,.9)';octx.beginPath();
 for(const[a,b]of BONES){const p=mapPt(SEN.u[a],SEN.v[a]),q=mapPt(SEN.u[b],SEN.v[b]);octx.moveTo(p[0],p[1]);octx.lineTo(q[0],q[1])}
 octx.stroke();
 for(let i=0;i<21;i++){const p=mapPt(SEN.u[i],SEN.v[i]);octx.fillStyle='#ff4d4d';octx.fillRect(p[0]-1.5,p[1]-1.5,3,3)}
 [[8,C_IDX],[12,C_MID]].forEach(([i,c])=>{const p=mapPt(SEN.u[i],SEN.v[i]);octx.fillStyle=c;octx.beginPath();octx.arc(p[0],p[1],5,0,7);octx.fill()});
 const desk=SEN.deskLock!==null?SEN.deskLock:SEN.desk;octx.setLineDash([6,4]);octx.strokeStyle='#4cd964';octx.beginPath();
 octx.moveTo(0,desk*ov.height);octx.lineTo(ov.width,desk*ov.height);octx.stroke();octx.setLineDash([]);
}

/* ---------- 連線：影像 + 事件 ---------- */
const camEl=$('cam');let snapN=0,snapT=0;function nextSnap(){snapT=performance.now();camEl.src='/snap?'+(snapN++)}
camEl.onload=()=>setTimeout(nextSnap,Math.max(0,70-(performance.now()-snapT)));camEl.onerror=()=>setTimeout(nextSnap,600);nextSnap();
let es;
function connect(){
 es=new EventSource('/events');
 es.onopen=()=>{$('cConn').textContent='已連線';$('cConn').className='pill on'};
 es.onerror=()=>{$('cConn').textContent='連線中斷，重試中…';$('cConn').className='pill off'};
 es.onmessage=e=>{
  const p=e.data.split(',');
  if(p[0]==='0'){onFrame(false);return}
  if(p.length<43)return;
  const u=new Float32Array(21),v=new Float32Array(21);
  for(let i=0;i<21;i++){u[i]=p[1+i]/1000;v[i]=p[22+i]/1000}
  onFrame(true,u,v)};
 es.addEventListener('func',()=>calibrate());
}
connect();
setInterval(()=>{SEN.rate=SEN.msgs;SEN.msgs=0;$('cRate').textContent=SEN.rate+' fps';
 const h=SEN.hand&&performance.now()-SEN.t<600;$('cHand').textContent=h?'偵測到手':(IN.src==='kbd'?'鍵盤模式':'未偵測到手');
 $('cHand').className='pill'+(h||IN.src==='kbd'?' on':'')},1000);

/* ---------- 主迴圈 ---------- */
let last=performance.now();
function loop(now){const dt=Math.min(.05,(now-last)/1000);last=now;pollInput(dt,now);update(dt);render();requestAnimationFrame(loop)}
requestAnimationFrame(loop);
</script></body></html>)HTML";

#endif
