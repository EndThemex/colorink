#ifndef WEBAPP_HTML_H
#define WEBAPP_HTML_H

const char WEBAPP_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>InkSight 局域网图库</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{--bk:#1a1a1a;--gy:#777;--bg:#f2f2ec;--card:#fff;--bd:#d8d8d0;--acc:#1a1a1a;--red:#b4342e;--yl:#c9a800;--f:-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif}
body{font-family:var(--f);background:var(--bg);color:var(--bk);line-height:1.55;padding:20px 16px 60px}
.wrap{max-width:1080px;margin:0 auto}
h1{font-size:1.35rem;letter-spacing:.5px;margin-bottom:2px}
.sub{color:var(--gy);font-size:.82rem;margin-bottom:16px}
.sub b{color:var(--bk)}
.grid{display:grid;grid-template-columns:minmax(340px,1.15fr) minmax(300px,.85fr);gap:16px;align-items:start}
@media(max-width:860px){.grid{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--bd);border-radius:14px;padding:18px;box-shadow:0 2px 14px rgba(0,0,0,.05)}
.card h2{font-size:1rem;margin-bottom:12px;display:flex;align-items:center;gap:8px}
.card h2 .dot{width:9px;height:9px;border-radius:50%;background:var(--bk);display:inline-block}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:10px 0}
label.fl{font-size:.82rem;color:var(--gy)}
select,input[type=text]{padding:7px 10px;border:1px solid var(--bd);border-radius:8px;background:#fff;font-size:.9rem;font-family:inherit}
input[type=checkbox]{width:16px;height:16px;accent-color:#000}
button{font-family:inherit;cursor:pointer;border:1px solid var(--bk);background:var(--bk);color:#fff;padding:9px 18px;border-radius:9px;font-size:.92rem;transition:.15s}
button:disabled{opacity:.45;cursor:not-allowed}
button.ghost{background:#fff;color:var(--bk)}
button.danger{background:#fff;color:var(--red);border-color:var(--red)}
button.sm{padding:5px 12px;font-size:.82rem;border-radius:7px}
#drop{border:2px dashed var(--bd);border-radius:12px;padding:26px 14px;text-align:center;color:var(--gy);font-size:.9rem;cursor:pointer;transition:.15s}
#drop.hover{border-color:var(--bk);color:var(--bk);background:#f7f7f2}
#preview{width:100%;border:1px solid var(--bd);border-radius:8px;background:#fff;image-rendering:pixelated;display:none;margin-top:12px}
.legend{display:flex;gap:14px;font-size:.78rem;color:var(--gy);margin-top:8px;flex-wrap:wrap}
.legend i{display:inline-block;width:11px;height:11px;border:1px solid #ccc;border-radius:3px;margin-right:4px;vertical-align:-1px}
#imglist{list-style:none}
#imglist li{display:flex;align-items:center;gap:10px;padding:10px 6px;border-bottom:1px solid #eee}
#imglist li:last-child{border-bottom:none}
#imglist .nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:.92rem}
#imglist .meta{font-size:.75rem;color:var(--gy);white-space:nowrap}
.cur{background:#fffbe0;border:1px solid #e6d96a;border-radius:5px;font-size:.7rem;padding:1px 6px;color:#8a7a00;white-space:nowrap}
.empty{color:var(--gy);font-size:.88rem;padding:18px 4px;text-align:center}
#toast{position:fixed;left:50%;bottom:26px;transform:translateX(-50%);background:var(--bk);color:#fff;padding:10px 22px;border-radius:24px;font-size:.88rem;opacity:0;pointer-events:none;transition:.3s;max-width:86vw;text-align:center;z-index:9}
#toast.show{opacity:.94}
.stat{display:flex;gap:14px;flex-wrap:wrap;font-size:.78rem;color:var(--gy);margin-top:6px}
.stat b{color:var(--bk);font-weight:600}
.err{color:var(--red);font-size:.85rem;margin-top:8px;min-height:1.2em}
</style>
</head>
<body>
<div class="wrap">
  <h1>InkSight 局域网图库</h1>
  <div class="sub">选择本地图片 → 浏览器转换为四色墨水屏数据 → 上传并在屏幕显示。也可用 <b id="mdns">http://inksight.local</b> 访问本页。</div>

  <div class="grid">
    <!-- 转换与上传 -->
    <div class="card">
      <h2><span class="dot"></span>图片转换与上传</h2>
      <div id="drop">点击选择图片，或把图片拖到这里<br><span style="font-size:.78rem">支持 JPG / PNG / WebP，浏览器本地完成四色转换</span></div>
      <input type="file" id="file" accept="image/*" style="display:none">

      <canvas id="preview" width="768" height="552"></canvas>

      <div class="row">
        <label class="fl">旋转</label>
        <select id="rot">
          <option value="0">0°</option>
          <option value="90">90°</option>
          <option value="180">180°</option>
          <option value="270">270°</option>
        </select>
        <label class="fl">填充</label>
        <select id="fit">
          <option value="cover">铺满（裁边）</option>
          <option value="contain">完整（留白）</option>
        </select>
        <label class="fl"><input type="checkbox" id="dither" checked> 抖动</label>
      </div>
      <div class="row">
        <input type="text" id="iname" placeholder="图片名称（可选）" style="flex:1;min-width:160px">
        <button id="upbtn" disabled>上传并显示</button>
      </div>
      <div class="err" id="perr"></div>
      <div class="legend">
        <span><i style="background:#000"></i>黑</span>
        <span><i style="background:#fff"></i>白</span>
        <span><i style="background:#f0d600"></i>黄</span>
        <span><i style="background:#be201c"></i>红</span>
        <span>768 × 552 · 四色 2bpp</span>
      </div>
    </div>

    <!-- 图库 -->
    <div class="card">
      <h2><span class="dot"></span>设备图库 <span style="flex:1"></span>
        <button class="sm ghost" id="refresh">刷新</button></h2>
      <ul id="imglist"><li class="empty">加载中…</li></ul>
      <div class="row" style="margin-top:12px">
        <button class="sm ghost" id="clearbtn">屏幕清为全白</button>
        <span style="flex:1"></span>
        <button class="sm ghost" id="portalbtn">重新配网</button>
      </div>
      <div class="stat" id="stat"></div>
    </div>
  </div>
</div>
<div id="toast"></div>

<script>
"use strict";
const SW = 768, SH = 552;
const PALETTE = [
  {rgb:[0,0,0],       code:0},  // 黑 00
  {rgb:[255,255,255], code:1},  // 白 01
  {rgb:[240,214,0],   code:2},  // 黄 10
  {rgb:[190,32,28],   code:3},  // 红 11
];

const $ = id => document.getElementById(id);
const drop=$('drop'), fileIn=$('file'), preview=$('preview'), perr=$('perr');
let srcImage = null, packed = null;

function toast(msg, ms=2600){
  const t=$('toast'); t.textContent=msg; t.classList.add('show');
  clearTimeout(t._h); t._h=setTimeout(()=>t.classList.remove('show'), ms);
}
function sleep(ms){ return new Promise(r=>setTimeout(r,ms)); }

// ── 缩放绘制：把原图按旋转/填充模式放进 768x552 画布 ──
function drawToWork(img){
  const rot = +$('rot').value, fit = $('fit').value;
  const work = document.createElement('canvas'); work.width=SW; work.height=SH;
  const wc = work.getContext('2d');
  wc.fillStyle = '#ffffff'; wc.fillRect(0,0,SW,SH);

  const landscape = (rot===0 || rot===180);
  const tw = landscape ? SW : SH, th = landscape ? SH : SW;   // 中间画布尺寸
  const tmp = document.createElement('canvas'); tmp.width=tw; tmp.height=th;
  const tc = tmp.getContext('2d');
  tc.fillStyle = '#ffffff'; tc.fillRect(0,0,tw,th);
  const s = fit==='cover' ? Math.max(tw/img.width, th/img.height)
                          : Math.min(tw/img.width, th/img.height);
  const dw = img.width*s, dh = img.height*s;
  tc.drawImage(img, (tw-dw)/2, (th-dh)/2, dw, dh);

  wc.save();
  if (rot===0){ wc.drawImage(tmp,0,0); }
  else if (rot===90){ wc.translate(SW,0); wc.rotate(Math.PI/2); wc.drawImage(tmp,0,0); }
  else if (rot===180){ wc.translate(SW,SH); wc.rotate(Math.PI); wc.drawImage(tmp,0,0); }
  else { wc.translate(0,SH); wc.rotate(-Math.PI/2); wc.drawImage(tmp,0,0); }
  wc.restore();
  return work;
}

// ── 四色量化（可选 Floyd-Steinberg 抖动）→ 2bpp 打包 ──
function quantizeTo2bpp(work){
  const dither = $('dither').checked;
  const wc = work.getContext('2d');
  const img = wc.getImageData(0,0,SW,SH);
  const px = img.data;
  const w = SW, h = SH;
  const err = dither ? new Float32Array(w*h*3) : null;
  if (dither){
    for (let i=0,j=0;i<w*h;i++,j+=3){
      err[j]=px[i*4]; err[j+1]=px[i*4+1]; err[j+2]=px[i*4+2];
    }
  }
  const packedBuf = new Uint8Array(w*h/4);       // 4 像素/字节，MSB first
  const out = new ImageData(w,h);
  const oc = out.data;
  const pal = PALETTE.map(p=>p.rgb), codes = PALETTE.map(p=>p.code);
  const NE = pal.length;
  const near = (r,g,b)=>{
    let best=0, bd=Infinity;
    for (let k=0;k<NE;k++){
      const dr=r-pal[k][0], dg=g-pal[k][1], db=b-pal[k][2];
      const d = 2*dr*dr + 4*dg*dg + 3*db*db;
      if (d<bd){ bd=d; best=k; }
    }
    return best;
  };
  const cl = v => v<0?0:(v>255?255:v);
  for (let y=0;y<h;y++){
    for (let x=0;x<w;x++){
      const i=(y*w+x)*3, p4=(y*w+x)*4;
      let r,g,b;
      if (dither){ r=cl(err[i]); g=cl(err[i+1]); b=cl(err[i+2]); }
      else { r=px[p4]; g=px[p4+1]; b=px[p4+2]; }
      const k = near(r,g,b);
      oc[p4]=pal[k][0]; oc[p4+1]=pal[k][1]; oc[p4+2]=pal[k][2]; oc[p4+3]=255;
      const byteI=(y*w+x)>>2, shift=6-((x&3)<<1);
      packedBuf[byteI] |= codes[k] << shift;
      if (dither){
        const pr=r-pal[k][0], pg=g-pal[k][1], pb=b-pal[k][2];
        if (x+1<w){ const j=(y*w+x+1)*3; err[j]+=pr*7/16; err[j+1]+=pg*7/16; err[j+2]+=pb*7/16; }
        if (y+1<h){
          if (x>0){ const j=((y+1)*w+x-1)*3; err[j]+=pr*3/16; err[j+1]+=pg*3/16; err[j+2]+=pb*3/16; }
          const j2=((y+1)*w+x)*3; err[j2]+=pr*5/16; err[j2+1]+=pg*5/16; err[j2+2]+=pb*5/16;
          if (x+1<w){ const j3=((y+1)*w+x+1)*3; err[j3]+=pr/16; err[j3+1]+=pg/16; err[j3+2]+=pb/16; }
        }
      }
    }
  }
  const pc = preview.getContext('2d');
  pc.putImageData(out,0,0);
  return packedBuf;
}

function process(){
  perr.textContent='';
  if (!srcImage) return;
  try {
    const work = drawToWork(srcImage);
    packed = quantizeTo2bpp(work);
    preview.style.display='block';
    $('upbtn').disabled = false;
  } catch(e){ perr.textContent = '处理失败：' + e.message; }
}

function loadFile(file){
  if (!file || !file.type || !file.type.startsWith('image/')){ perr.textContent='请选择图片文件'; return; }
  const url = URL.createObjectURL(file);
  const img = new Image();
  img.onload = ()=>{
    srcImage = img;
    const base = (file.name||'').replace(/\.[^.]+$/,'');
    if (!$('iname').value) $('iname').value = base.slice(0,48);
    process();
    URL.revokeObjectURL(url);
  };
  img.onerror = ()=>{ perr.textContent='图片解码失败'; URL.revokeObjectURL(url); };
  img.src = url;
}

drop.addEventListener('click', ()=>fileIn.click());
fileIn.addEventListener('change', ()=>loadFile(fileIn.files[0]));
['dragover','dragenter'].forEach(ev=>drop.addEventListener(ev, e=>{e.preventDefault();drop.classList.add('hover');}));
['dragleave','dragend'].forEach(ev=>drop.addEventListener(ev, ()=>drop.classList.remove('hover')));
drop.addEventListener('drop', e=>{
  e.preventDefault(); drop.classList.remove('hover');
  loadFile(e.dataTransfer.files[0]);
});
$('rot').addEventListener('change', process);
$('fit').addEventListener('change', process);
$('dither').addEventListener('change', process);

// ── 上传并显示 ──
$('upbtn').addEventListener('click', async ()=>{
  if (!packed) return;
  $('upbtn').disabled = true; perr.textContent='';
  try {
    const fd = new FormData();
    fd.append('name', $('iname').value || 'image');
    fd.append('file', new Blob([packed.buffer], {type:'application/octet-stream'}), 'frame.raw');
    toast('上传中…');
    const r = await fetch('/api/image', {method:'POST', body:fd});
    const j = await r.json();
    if (!j.ok){ perr.textContent = j.msg || '上传失败'; $('upbtn').disabled=false; return; }
    toast('上传成功，屏幕刷新中（约 15 秒）…', 8000);
    await fetch('/api/display?id='+j.id, {method:'POST'}).catch(()=>{});
    await sleep(16000);
    await loadImages(); await loadStatus();
  } catch(e){ perr.textContent='上传失败：'+e.message; }
  $('upbtn').disabled = !packed;
});

// ── 图库列表 ──
function esc(s){ const d=document.createElement('span'); d.textContent=s; return d.innerHTML; }
function fmtKB(n){ return (n/1024).toFixed(0)+' KB'; }

async function loadImages(){
  try {
    const r = await fetch('/api/images');
    const j = await r.json();
    const ul = $('imglist');
    if (!j.images || !j.images.length){
      ul.innerHTML = '<li class="empty">还没有图片，上传一张试试</li>';
      return;
    }
    ul.innerHTML = j.images.map(im =>
      `<li>
        <span class="nm">${esc(im.name)}</span>
        <span class="meta">${fmtKB(im.size)}</span>
        ${im.current?'<span class="cur">屏幕显示中</span>':''}
        <button class="sm ${im.current?'ghost':''}" data-show="${im.id}" ${im.current?'disabled':''}>显示</button>
        <button class="sm danger" data-del="${im.id}">删除</button>
      </li>`).join('');
    ul.querySelectorAll('[data-show]').forEach(b=>b.addEventListener('click', ()=>showImage(+b.dataset.show)));
    ul.querySelectorAll('[data-del]').forEach(b=>b.addEventListener('click', ()=>delImage(+b.dataset.del)));
  } catch(e){
    $('imglist').innerHTML = '<li class="empty">加载失败：'+esc(e.message)+'</li>';
  }
}

async function showImage(id){
  toast('屏幕刷新中（约 15 秒）…', 8000);
  try {
    const r = await fetch('/api/display?id='+id, {method:'POST'});
    const j = await r.json();
    if (!j.ok){ toast(j.msg||'显示失败'); return; }
  } catch(e){ toast('请求失败：'+e.message); return; }
  await sleep(16000);
  await loadImages(); await loadStatus();
  toast('完成');
}

async function delImage(id){
  if (!confirm('确定删除这张图片？')) return;
  try {
    const r = await fetch('/api/image?id='+id, {method:'DELETE'});
    const j = await r.json();
    if (!j.ok){ toast(j.msg||'删除失败'); return; }
    toast('已删除');
  } catch(e){ toast('删除失败：'+e.message); }
  await loadImages(); await loadStatus();
}

$('clearbtn').addEventListener('click', async ()=>{
  toast('屏幕清空中（约 15 秒）…', 8000);
  await fetch('/api/clear', {method:'POST'}).catch(()=>{});
  await sleep(16000);
  await loadImages(); await loadStatus();
});

$('refresh').addEventListener('click', async ()=>{ await loadImages(); await loadStatus(); });
$('portalbtn').addEventListener('click', async ()=>{
  if (!confirm('设备将重启进入配网模式（热点 InkSight-XXXX），确定？')) return;
  await fetch('/restart', {method:'POST'}).catch(()=>{});
  toast('设备正在重启…');
});

async function loadStatus(){
  try {
    const r = await fetch('/api/status');
    const j = await r.json();
    $('stat').innerHTML =
      `<span>IP <b>${esc(j.ip)}</b></span>`+
      `<span>WiFi <b>${esc(j.ssid)}</b> (${j.rssi}dBm)</span>`+
      `<span>电池 <b>${esc(j.battery)}</b></span>`+
      `<span>存储 <b>${(j.free_fs/1024).toFixed(0)} KB</b> 可用</span>`+
      `<span>内存 <b>${(j.heap/1024).toFixed(0)} KB</b> 空闲</span>`;
  } catch(e){}
}

loadImages(); loadStatus();
</script>
</body>
</html>
)rawliteral";

#endif // WEBAPP_HTML_H
