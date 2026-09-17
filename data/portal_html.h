#ifndef PORTAL_HTML_H
#define PORTAL_HTML_H

const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>InkSight 配网</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{--bk:#1a1a1a;--gy:#8a8a84;--bd:#d8d8d0;--red:#b4342e;--f:-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC','Microsoft YaHei',sans-serif}
body{font-family:var(--f);background:linear-gradient(135deg,#f2f2ec,#e6e6df);color:var(--bk);line-height:1.6;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}
.card{background:#fff;border:1px solid var(--bd);border-radius:16px;box-shadow:0 6px 32px rgba(0,0,0,.07);width:100%;max-width:400px;padding:26px 22px}
h1{font-size:1.25rem;margin-bottom:2px}
.sub{color:var(--gy);font-size:.8rem;margin-bottom:16px}
.row{display:flex;gap:8px;align-items:center}
input[type=text],input[type=password]{flex:1;padding:10px 12px;border:1px solid var(--bd);border-radius:9px;font-size:.95rem;font-family:inherit;background:#fff;min-width:0}
input:focus{outline:none;border-color:var(--bk)}
button{font-family:inherit;cursor:pointer;border:1px solid var(--bk);background:var(--bk);color:#fff;padding:10px 16px;border-radius:9px;font-size:.9rem;transition:.15s}
button:disabled{opacity:.45;cursor:not-allowed}
button.ghost{background:#fff;color:var(--bk)}
button.danger{background:#fff;color:var(--red);border-color:var(--red)}
button.sm{padding:5px 11px;font-size:.8rem;border-radius:7px}
.list{margin-top:12px;border:1px solid #eee;border-radius:10px;overflow:hidden;max-height:260px;overflow-y:auto}
.item{display:flex;align-items:center;gap:8px;padding:10px 12px;border-bottom:1px solid #f0f0ea;cursor:pointer}
.item:last-child{border-bottom:none}
.item:hover{background:#f7f7f2}
.item.sel{background:#fffbe0}
.item .nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:.92rem}
.item .sig{font-size:.72rem;color:var(--gy);white-space:nowrap}
.lock{font-size:.8rem;color:var(--gy)}
.sect{margin-top:16px;font-size:.82rem;color:var(--gy);display:flex;align-items:center;gap:8px}
.sect::after{content:"";flex:1;height:1px;background:#eee}
.err{color:var(--red);font-size:.85rem;margin-top:8px;min-height:1.2em}
.okbox{display:none;text-align:center;padding:18px 6px}
.okbox h2{font-size:1.05rem;margin-bottom:8px}
.okbox p{color:var(--gy);font-size:.85rem;margin-bottom:6px}
.okbox b{color:var(--bk)}
.spin{display:inline-block;width:15px;height:15px;border:2px solid #ccc;border-top-color:var(--bk);border-radius:50%;animation:sp 1s linear infinite;vertical-align:-3px;margin-right:6px}
@keyframes sp{to{transform:rotate(360deg)}}
.hide{display:none!important}
</style>
</head>
<body>
<div class="card">
  <div id="setup">
    <h1>InkSight 配网</h1>
    <div class="sub" id="devinfo">正在读取设备信息…</div>

    <div class="row">
      <input type="text" id="ssid" placeholder="WiFi 名称" readonly>
      <button class="ghost" id="scanbtn" style="flex-shrink:0">扫描</button>
    </div>
    <div class="list" id="netlist" style="display:none"></div>

    <div class="row" style="margin-top:12px">
      <input type="password" id="pass" placeholder="WiFi 密码">
      <button id="connbtn" style="flex-shrink:0">连接</button>
    </div>
    <div class="err" id="err"></div>

    <div class="sect">已保存的网络</div>
    <div class="list" id="savedlist" style="max-height:180px"><div class="item" style="cursor:default;color:var(--gy)">加载中…</div></div>

    <div class="row" style="margin-top:14px">
      <button class="danger sm" id="restartbtn">重新启动设备</button>
      <span style="flex:1"></span>
      <span style="font-size:.74rem;color:var(--gy)">本热点无密码，仅用于配网</span>
    </div>
  </div>

  <div class="okbox" id="okbox">
    <h2>连接成功</h2>
    <p>设备将在 5 秒后自动重启并接入 WiFi。</p>
    <p>重启完成后，请用<b>同一 WiFi</b> 下的浏览器访问：</p>
    <p style="font-size:1.05rem"><b id="okurl">http://inksight.local</b></p>
    <p style="margin-top:8px;font-size:.78rem" id="okip"></p>
  </div>
</div>

<script>
"use strict";
var $=function(id){return document.getElementById(id)};
var selected='';

function esc(s){var d=document.createElement('span');d.textContent=s;return d.innerHTML}
function signal(rssi){
  if(rssi>=-55)return '▂▄▆█';
  if(rssi>=-67)return '▂▄▆_';
  if(rssi>=-78)return '▂▄__';
  return '▂___';
}

// ── 设备信息 ──
fetch('/info').then(function(r){return r.json()}).then(function(d){
  $('devinfo').textContent='MAC '+d.mac+'　电池 '+d.battery;
}).catch(function(){ $('devinfo').textContent='设备信息获取失败'; });

// ── 扫描 ──
function renderNets(d){
  var el=$('netlist');
  el.style.display='block';
  el.innerHTML=d.networks.map(function(n){
    return '<div class="item" data-ssid="'+esc(n.ssid)+'">'+
      '<span class="nm">'+esc(n.ssid)+'</span>'+
      '<span class="sig">'+signal(n.rssi)+'</span>'+
      (n.secure?'<span class="lock">🔒</span>':'<span class="lock" title="开放">开放</span>')+
      '</div>';
  }).join('')||'<div class="item" style="cursor:default;color:var(--gy)">未发现网络</div>';
  Array.prototype.forEach.call(el.querySelectorAll('.item'),function(it){
    it.addEventListener('click',function(){
      selected=it.getAttribute('data-ssid');
      $('ssid').value=selected;
      Array.prototype.forEach.call(el.querySelectorAll('.item'),function(x){x.classList.remove('sel')});
      it.classList.add('sel');
      $('pass').focus();
    });
  });
}
function scanDone(){
  $('scanbtn').disabled=false;$('scanbtn').textContent='扫描';
}
function fetchNets(url,onFail){
  fetch(url,{cache:'no-store'}).then(function(r){return r.json()}).then(function(d){
    renderNets(d);
  }).catch(function(){ if(onFail)onFail(); });
}
function scan(){
  $('scanbtn').disabled=true;$('scanbtn').textContent='扫描中…';
  $('err').textContent='';
  // 设备扫描时射频要切走约 2 秒，手机可能瞬间掉线：请求失败就等重连后直接取设备缓存的结果
  fetchNets('/scan?refresh=1',function(){
    setTimeout(function(){ fetchNets('/scan',function(){$('err').textContent='扫描失败，请重试';}); scanDone(); },3000);
  });
  setTimeout(scanDone,6000);
}
$('scanbtn').addEventListener('click',scan);
// 页面加载只读设备已缓存的结果，避免刚连上热点就触发一次扫描把手机踢掉
fetchNets('/scan');

// ── 已保存网络 ──
function loadSaved(){
  fetch('/wifi_list').then(function(r){return r.json()}).then(function(d){
    var el=$('savedlist');
    if(!d.networks.length){
      el.innerHTML='<div class="item" style="cursor:default;color:var(--gy)">暂无</div>';
      return;
    }
    el.innerHTML=d.networks.map(function(n){
      return '<div class="item" data-ssid="'+esc(n)+'">'+
        '<span class="nm">'+esc(n)+'</span>'+
        '<button class="sm ghost" data-conn="'+esc(n)+'">连接</button>'+
        '<button class="sm danger" data-del="'+esc(n)+'">删除</button>'+
        '</div>';
    }).join('');
    Array.prototype.forEach.call(el.querySelectorAll('[data-conn]'),function(b){
      b.addEventListener('click',function(e){e.stopPropagation();connectSaved(b.getAttribute('data-conn'))});
    });
    Array.prototype.forEach.call(el.querySelectorAll('[data-del]'),function(b){
      b.addEventListener('click',function(e){
        e.stopPropagation();
        var fd=new FormData();fd.append('ssid',b.getAttribute('data-del'));
        fetch('/delete_wifi',{method:'POST',body:fd}).then(loadSaved);
      });
    });
  }).catch(function(){ $('savedlist').innerHTML='<div class="item" style="cursor:default;color:var(--gy)">加载失败</div>'; });
}
loadSaved();

// ── 连接 ──
function busy(on,msg){
  $('connbtn').disabled=on;
  $('connbtn').innerHTML=on?'<span class="spin"></span>连接中':'连接';
  $('err').textContent=on?(msg||'正在连接，最长等待 15 秒…'):'';
}
function showOk(){
  $('setup').classList.add('hide');
  $('okbox').style.display='block';
}
function handleResult(d,ipLine){
  if(d.ok){ showOk(); return; }
  var map={NO_SSID:'找不到该网络',AUTH_FAIL:'密码错误',TIMEOUT:'连接超时，请重试',SAVED_CONNECT_FAILED:'连接失败，请重试或删除后重新添加',NOT_FOUND:'该网络不在保存列表中'};
  $('err').textContent=map[d.msg]||d.msg||'连接失败';
  busy(false);
  if(d.list){ loadSaved(); }
}
function connect(ssid,pass){
  busy(true);
  var fd=new FormData();fd.append('ssid',ssid);fd.append('pass',pass);
  fetch('/save_wifi',{method:'POST',body:fd}).then(function(r){return r.json()}).then(function(d){handleResult(d)}).
  catch(function(){ $('err').textContent='请求失败，请重试';busy(false); });
}
function connectSaved(ssid){
  busy(true);
  var fd=new FormData();fd.append('ssid',ssid);
  fetch('/connect_saved',{method:'POST',body:fd}).then(function(r){return r.json()}).then(function(d){handleResult(d)}).
  catch(function(){ $('err').textContent='请求失败，请重试';busy(false); });
}
$('connbtn').addEventListener('click',function(){
  var ssid=$('ssid').value.trim(),pass=$('pass').value;
  $('err').textContent='';
  if(!ssid){ $('err').textContent='请先扫描并选择一个网络'; return; }
  connect(ssid,pass);
});
$('pass').addEventListener('keydown',function(e){ if(e.key==='Enter')$('connbtn').click(); });

$('restartbtn').addEventListener('click',function(){
  if(!confirm('确定重启设备？'))return;
  fetch('/restart',{method:'POST'}).catch(function(){});
  $('err').textContent='设备重启中…';
});
</script>
</body>
</html>
)rawliteral";

#endif // PORTAL_HTML_H
