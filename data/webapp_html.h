#ifndef WEBAPP_HTML_H
#define WEBAPP_HTML_H

const char WEBAPP_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>ColorInk 局域网图库</title>
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
#preview{max-width:100%;max-height:65vh;border:1px solid var(--bd);border-radius:8px;background:#fff;image-rendering:pixelated;display:none;margin-top:12px}
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
details.cal{margin-top:12px;border-top:1px solid #eee;padding-top:10px}
details.cal summary{font-size:.82rem;color:var(--gy);cursor:pointer;list-style:none}
details.cal summary::-webkit-details-marker{display:none}
details.cal summary::before{content:'▸ ';font-size:.75rem}
details.cal[open] summary::before{content:'▾ '}
input[type=color]{width:34px;height:26px;padding:0;border:1px solid var(--bd);border-radius:6px;background:#fff;vertical-align:middle;margin-left:5px;cursor:pointer}
</style>
</head>
<body>
<div class="wrap">
  <h1>ColorInk 图库</h1>
  <div class="sub">选择本地图片 → 浏览器转换为四色墨水屏数据 → 上传并在屏幕显示。也可用 <b id="mdns">http://ColorInk.local</b> 访问本页。</div>

  <div class="grid">
    <!-- 转换与上传 -->
    <div class="card">
      <h2><span class="dot"></span>图片转换与上传</h2>
      <div id="drop">点击选择图片，或把图片拖到这里<br><span style="font-size:.78rem">支持 JPG / PNG / WebP，浏览器本地完成四色转换</span></div>
      <input type="file" id="file" accept="image/*" style="display:none">

      <canvas id="preview" width="552" height="768"></canvas>

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
        <label class="fl">抖动</label>
        <select id="dmode">
          <option value="bayer">有序 Bayer（锐利）</option>
          <option value="fs">扩散 FS（细腻）</option>
          <option value="atkinson">Atkinson（高对比）</option>
          <option value="none">无</option>
        </select>
        <label class="fl"><input type="checkbox" id="sharpen" checked> 锐化</label>
        <label class="fl"><input type="checkbox" id="vivid" checked> 鲜艳</label>
      </div>
      <div class="row">
        <input type="text" id="iname" placeholder="图片名称（可选）" style="flex:1;min-width:160px">
        <button id="upbtn" disabled>上传并显示</button>
      </div>
      <div class="err" id="perr"></div>
      <div class="legend">
        <span><i id="sw0"></i>黑</span>
        <span><i id="sw1"></i>白</span>
        <span><i id="sw2"></i>黄</span>
        <span><i id="sw3"></i>红</span>
        <span>552 × 768 · 四色 2bpp · 竖屏</span>
      </div>

      <details class="cal">
        <summary>色板校准（填实测面板色，预览即屏上效果）</summary>
        <div class="row">
          <label class="fl">黑<input type="color" id="pc0"></label>
          <label class="fl">白<input type="color" id="pc1"></label>
          <label class="fl">黄<input type="color" id="pc2"></label>
          <label class="fl">红<input type="color" id="pc3"></label>
          <button class="sm ghost" id="calshot">下载校准图</button>
          <button class="sm ghost" id="calreset">恢复默认</button>
        </div>
        <div class="fl" style="font-size:.78rem">
          流程：下载校准图 → 用本页上传到屏幕（抖动选"无"）→ 对屏幕拍照 → 用取色工具读取四个色块的
          RGB → 填回上面。填入后量化目标和预览同步更新，改完记得重新上传目标图片。
        </div>
      </details>
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
// 屏幕物理方向为竖屏：面板 552 列（横向）× 768 行（纵向）。
// 控制器按"552 行 × 每行 768 像素"扫描，即帧缓冲里一"行"对应屏幕的竖直方向。
// 因此画布用 552×768 竖屏排版，打包时转置写入设备帧缓冲。
const SW = 552, SH = 768;
// 面板调色板，索引 0..3 = 黑/白/黄/红，对应 2bpp 色码 00/01/10/11。
// 理论 sRGB 值与墨水屏实际反射率差很多（黄偏暗、红偏褐），所以这里允许填「实测色」：
// 下载校准图 → 上传到屏幕（抖动选"无"）→ 拍照取色 → 填回下面四个色块。
// 量化目标和预览画布用同一组值，因此预览所见 = 屏上效果。
const CODES = [0,1,2,3];
const DEF_PAL = ['#000000','#ffffff','#f0d600','#be201c'];
const PAL_KEY = 'inksight_pal';
let PAL = DEF_PAL.slice();
try {
  const saved = JSON.parse(localStorage.getItem(PAL_KEY)||'null');
  if (Array.isArray(saved) && saved.length===4 && saved.every(h=>/^#[0-9a-f]{6}$/i.test(h))) PAL = saved;
} catch(e){}
function hexToRgb(h){ return [parseInt(h.slice(1,3),16), parseInt(h.slice(3,5),16), parseInt(h.slice(5,7),16)]; }

const $ = id => document.getElementById(id);
const drop=$('drop'), fileIn=$('file'), preview=$('preview'), perr=$('perr');
let srcImage = null, packed = null;

function toast(msg, ms=2600){
  const t=$('toast'); t.textContent=msg; t.classList.add('show');
  clearTimeout(t._h); t._h=setTimeout(()=>t.classList.remove('show'), ms);
}
function sleep(ms){ return new Promise(r=>setTimeout(r,ms)); }

// ── 缩放绘制：把原图按旋转/填充模式放进 552×768 竖屏画布 ──
// 高质量降采样（参考 pica 的思路）：先分步减半（每步 ≤2×，浏览器高质量
// 重采样），避免几千像素的原图一步缩到 552 宽导致细节丢失、整体发糊。
function drawToWork(img){
  const rot = +$('rot').value, fit = $('fit').value;
  const work = document.createElement('canvas'); work.width=SW; work.height=SH;
  const wc = work.getContext('2d');
  wc.imageSmoothingEnabled = true; wc.imageSmoothingQuality = 'high';
  wc.fillStyle = '#ffffff'; wc.fillRect(0,0,SW,SH);

  const landscape = (rot===0 || rot===180);
  const tw = landscape ? SW : SH, th = landscape ? SH : SW;   // 中间画布尺寸

  // 先算最终绘制尺寸（分步减半不改变它），再据此决定减半到哪一步
  let src = img, cw = img.width, ch = img.height;
  const s0 = fit==='cover' ? Math.max(tw/cw, th/ch) : Math.min(tw/cw, th/ch);
  const dw0 = cw*s0, dh0 = ch*s0;
  while ((cw>>1) >= dw0 && (ch>>1) >= dh0) {
    const nc = document.createElement('canvas');
    nc.width = Math.max(1, cw>>1); nc.height = Math.max(1, ch>>1);
    const ncx = nc.getContext('2d');
    ncx.imageSmoothingEnabled = true; ncx.imageSmoothingQuality = 'high';
    ncx.drawImage(src, 0, 0, nc.width, nc.height);
    src = nc; cw = nc.width; ch = nc.height;
  }

  const tmp = document.createElement('canvas'); tmp.width=tw; tmp.height=th;
  const tc = tmp.getContext('2d');
  tc.imageSmoothingEnabled = true; tc.imageSmoothingQuality = 'high';
  tc.fillStyle = '#ffffff'; tc.fillRect(0,0,tw,th);
  const s = fit==='cover' ? Math.max(tw/cw, th/ch)
                          : Math.min(tw/cw, th/ch);
  const dw = cw*s, dh = ch*s;
  tc.drawImage(src, (tw-dw)/2, (th-dh)/2, dw, dh);

  wc.save();
  if (rot===0){ wc.drawImage(tmp,0,0); }
  else if (rot===90){ wc.translate(SW,0); wc.rotate(Math.PI/2); wc.drawImage(tmp,0,0); }
  else if (rot===180){ wc.translate(SW,SH); wc.rotate(Math.PI); wc.drawImage(tmp,0,0); }
  else { wc.translate(0,SH); wc.rotate(-Math.PI/2); wc.drawImage(tmp,0,0); }
  wc.restore();
  return work;
}

// ── 前处理 + 四色量化 → 2bpp 打包 ──
// 抖动方案参考开源实践：
//  - esp_epaper 组件：BWRY 四色面板推荐 ordered（Bayer）抖动，纹理规律、边缘锐利；
//  - epaper-dithering：Atkinson 只扩散 6/8 误差，对比更强，适合低色数墨水屏；
//    误差扩散配合蛇形（serpentine）扫描可消除 Floyd-Steinberg 常见的方向性条纹；
//  - 鲜艳化：墨水屏实测显色比理论 RGB 暗很多（esp32-photoframe MEASURED_PALETTE），
//    提饱和度/对比度后再映射四色，观感更接近原图。
const BAYER8 = [
  [ 0,32, 8,40, 2,34,10,42],
  [48,16,56,24,50,18,58,26],
  [12,44, 4,36,14,46, 6,38],
  [60,28,52,20,62,30,54,22],
  [ 3,35,11,43, 1,33, 9,41],
  [51,19,59,27,49,17,57,25],
  [15,47, 7,39,13,45, 5,37],
  [63,31,55,23,61,29,53,21],
];
const ATK = [[1,0,1/8],[2,0,1/8],[-1,1,1/8],[0,1,1/8],[1,1,1/8],[2,1,1/8]];
const FSK = [[1,0,7/16],[-1,1,3/16],[0,1,5/16],[1,1,1/16]];

// 鲜艳化：饱和度 ×1.25 + 对比度 ×1.1（Uint8ClampedArray 写入自动钳位）
function enhance(px){
  const sat=1.25, con=1.1;
  for (let i=0;i<px.length;i+=4){
    const lum = px[i]*0.299 + px[i+1]*0.587 + px[i+2]*0.114;
    px[i]   = (lum + (px[i]  -lum)*sat - 128)*con + 128;
    px[i+1] = (lum + (px[i+1]-lum)*sat - 128)*con + 128;
    px[i+2] = (lum + (px[i+2]-lum)*sat - 128)*con + 128;
  }
}

// 锐化：非锐化掩模 USM，v + 0.7*(v - 4邻域均值)，跳过边缘 1px
function sharpen(px,w,h){
  const src = new Uint8ClampedArray(px);
  const row = w*4;
  for (let y=1;y<h-1;y++){
    for (let x=1;x<w-1;x++){
      const p=(y*w+x)*4;
      for (let c=0;c<3;c++){
        const avg=(src[p-4+c]+src[p+4+c]+src[p-row+c]+src[p+row+c])/4;
        px[p+c]=src[p+c]+0.7*(src[p+c]-avg);
      }
    }
  }
}

// 最近色判别用 Oklab 感知距离，而不是加权 RGB：加权 RGB 的权重是手调折中，
// 遇到肤色/天空/橙色渐变容易把黄和红判错。Oklab（Ottosson）亮度+色度分离，
// 小调色板下选色明显更准；sRGB→线性用 256 项 LUT，避免逐像素 pow。
const LIN = new Float32Array(256);
for (let i=0;i<256;i++){ const c=i/255; LIN[i] = c<=0.04045 ? c/12.92 : Math.pow((c+0.055)/1.055, 2.4); }

// 返回 nearest 判据用的 Oklab 三元组，写入共享 tmp 避免逐像素分配
const okTmp = [0,0,0];
function srgbToOklab(r,g,b){
  const lr=LIN[r|0], lg=LIN[g|0], lb=LIN[b|0];
  const l = Math.cbrt(0.4122214708*lr + 0.5363325363*lg + 0.0514459929*lb);
  const m = Math.cbrt(0.2119034982*lr + 0.6806995451*lg + 0.1073969566*lb);
  const s = Math.cbrt(0.0883024619*lr + 0.2817188376*lg + 0.6299787005*lb);
  okTmp[0] = 0.2104542553*l + 0.7936177850*m - 0.0040720468*s;
  okTmp[1] = 1.9779984951*l - 2.4285922050*m + 0.4505937099*s;
  okTmp[2] = 0.0259040371*l + 0.7827717662*m - 0.8086757660*s;
  return okTmp;
}

function quantizeTo2bpp(work){
  const mode = $('dmode').value;
  const doSharp = $('sharpen').checked, doVivid = $('vivid').checked;
  const wc = work.getContext('2d');
  const img = wc.getImageData(0,0,SW,SH);
  const px = img.data;
  const w = SW, h = SH;
  if (doVivid) enhance(px);
  if (doSharp) sharpen(px,w,h);

  const diff = (mode==='fs' || mode==='atkinson');  // 误差扩散类才需要 err 缓冲
  const err = diff ? new Float32Array(w*h*3) : null;
  if (diff){
    for (let i=0,j=0;i<w*h;i++,j+=3){
      err[j]=px[i*4]; err[j+1]=px[i*4+1]; err[j+2]=px[i*4+2];
    }
  }
  const packedBuf = new Uint8Array(w*h/4);       // 4 像素/字节，MSB first
  const out = new ImageData(w,h);
  const oc = out.data;
  const pal = PAL.map(hexToRgb);
  const palOK = pal.map(c=>srgbToOklab(c[0],c[1],c[2]).slice());  // 每次量化只算 4 次
  const NE = pal.length;
  const cl = v => v<0?0:(v>255?255:v);
  const near = (r,g,b)=>{
    const q = srgbToOklab(cl(r),cl(g),cl(b));
    const q0=q[0], q1=q[1], q2=q[2];
    let best=0, bd=Infinity;
    for (let k=0;k<NE;k++){
      const t=palOK[k];
      const d0=q0-t[0], d1=q1-t[1], d2=q2-t[2];
      const d = d0*d0 + d1*d1 + d2*d2;      // Oklab 欧氏距离（感知均匀）
      if (d<bd){ bd=d; best=k; }
    }
    return best;
  };
  const kern = mode==='atkinson' ? ATK : FSK;
  for (let y=0;y<h;y++){
    const rtl = diff && (y&1);                    // 蛇形：偶数行 →，奇数行 ←
    for (let n=0;n<w;n++){
      const x = rtl ? w-1-n : n;
      const i=(y*w+x)*3, p4=(y*w+x)*4;
      let r,g,b;
      if (diff){ r=cl(err[i]); g=cl(err[i+1]); b=cl(err[i+2]); }
      else {
        r=px[p4]; g=px[p4+1]; b=px[p4+2];
        if (mode==='bayer'){
          const t=(BAYER8[y&7][x&7]+0.5)/64-0.5;  // 归一化到 ±0.5
          r+=t*64; g+=t*64; b+=t*64;              // ±32 级抖动幅度
        }
      }
      const k = near(r,g,b);
      oc[p4]=pal[k][0]; oc[p4+1]=pal[k][1]; oc[p4+2]=pal[k][2]; oc[p4+3]=255;
      // 竖屏画布 (x=列, y=行) → 设备帧缓冲 (行=x, 列=767-y)，
      // 与旧版横屏 UI 选"旋转90°"的打包结果一致（已在屏幕上验证为正立显示）
      const dx=(SH-1)-y, byteI=x*192+(dx>>2), shift=6-((dx&3)<<1);
      packedBuf[byteI] |= CODES[k] << shift;
      if (diff){
        const pr=r-pal[k][0], pg=g-pal[k][1], pb=b-pal[k][2];
        for (let m=0;m<kern.length;m++){
          const ox=kern[m][0], oy=kern[m][1], wt=kern[m][2];
          const nx = rtl ? x-ox : x+ox, ny = y+oy;   // 蛇形反向行：偏移取反
          if (nx<0||nx>=w||ny>=h) continue;
          const j=(ny*w+nx)*3;
          err[j]+=pr*wt; err[j+1]+=pg*wt; err[j+2]+=pb*wt;
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
['dmode','sharpen','vivid'].forEach(id=>$(id).addEventListener('change', process));

// ── 色板校准：面板实测色（localStorage 持久化，量化与预览共用） ──
function syncPal(){
  PAL.forEach((h,i)=>{ $('sw'+i).style.background = h; $('pc'+i).value = h; });
}
syncPal();
PAL.forEach((_,i)=>$('pc'+i).addEventListener('change', e=>{
  PAL[i] = e.target.value;
  try { localStorage.setItem(PAL_KEY, JSON.stringify(PAL)); } catch(ex){}
  syncPal(); process();
}));
$('calreset').addEventListener('click', ()=>{
  PAL = DEF_PAL.slice();
  try { localStorage.removeItem(PAL_KEY); } catch(ex){}
  syncPal(); process(); toast('已恢复默认色板');
});
// 生成四色全屏色块图，供上传到屏幕后拍照取色
$('calshot').addEventListener('click', ()=>{
  const c = document.createElement('canvas'); c.width=SW; c.height=SH;
  const x = c.getContext('2d');
  PAL.forEach((h,i)=>{ x.fillStyle=h; x.fillRect(0, Math.round(i*SH/4), SW, Math.round(SH/4)); });
  const a = document.createElement('a');
  a.href = c.toDataURL('image/png'); a.download = 'palette_cal.png'; a.click();
  toast('校准图已下载：上传到屏幕（抖动选"无"）后拍照取色', 7000);
});

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
    await loadImages(); await loadStatus();   // 列表先更新，无需等刷屏结束
    try {
      const d = await fetch('/api/display?id='+j.id, {method:'POST'});
      const dj = await d.json();
      if (!dj.ok){ toast(dj.msg || '显示请求失败', 6000); }
    } catch(e){ toast('显示请求失败：'+e.message, 6000); }
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
        <button class="sm ${im.current?'ghost':''}" data-show="${im.id}">显示</button>
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
  if (!confirm('设备将重启进入配网模式（热点 ColorInk-XXXX），确定？')) return;
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
