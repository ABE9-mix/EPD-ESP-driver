#pragma once

// ========== 主页面（多标签页） ==========
static const char PAGE_STATUS[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>EPD 墨水屏</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#eee;display:flex;justify-content:center;min-height:100vh;padding:0}
  .app{max-width:480px;width:100%;padding:0 16px 80px}
  h1{color:#e0a85c;font-size:20px;font-weight:500;padding:20px 0 4px;text-align:center}
  .sub{color:#888;font-size:13px;text-align:center;margin-bottom:16px}
  /* 标签栏 */
  .tabs{display:flex;background:#0d0d0d;border-radius:8px;overflow:hidden;margin-bottom:16px}
  .tab{flex:1;text-align:center;padding:10px 4px;cursor:pointer;color:#888;font-size:13px;transition:all .2s;border-bottom:2px solid transparent}
  .tab:hover{color:#ddd}
  .tab.active{color:#e0a85c;border-bottom-color:#e0a85c;background:#1a1a2e}
  /* 卡片 */
  .card{display:none;background:#16213e;border:1px solid #333;border-radius:12px;padding:20px}
  .card.active{display:block}
  label{display:block;margin:12px 0 4px;color:#aaa;font-size:13px}
  input,select{width:100%;padding:10px 12px;border:1px solid #333;border-radius:6px;background:#0d0d0d;color:#eee;font-size:15px}
  input:focus,select:focus{border-color:#e0a85c;outline:none}
  .btn{display:block;width:100%;padding:12px;background:#e0a85c;color:#1a1a2e;border:none;border-radius:6px;font-size:15px;font-weight:600;cursor:pointer;text-align:center;margin-top:14px}
  .btn:hover{background:#c8913e}
  .btn.sm{background:#333;color:#eee;font-weight:400;padding:8px;font-size:13px;margin-top:8px}
  .btn.sm:hover{background:#444}
  .btn.danger{background:#5a2020;color:#f88;font-weight:400}
  .btn.danger:hover{background:#7a2a2a}
  .btn:disabled{background:#555;cursor:not-allowed}
  .status{margin-top:12px;padding:10px;border-radius:6px;text-align:center;display:none;font-size:13px}
  .status.ok{display:block;background:#1a3a2e;color:#80a894;border:1px solid #2a5a3e}
  .status.err{display:block;background:#3a1a1a;color:#f88;border:1px solid #5a2a2a}
  .info-row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #222;font-size:14px}
  .info-row:last-child{border-bottom:none}
  .info-label{color:#888}
  .info-val{color:#eee}
  /* 上传区域 */
  .drop-zone{background:#0d0d0d;border:2px dashed #333;border-radius:8px;padding:30px 16px;text-align:center;cursor:pointer;transition:border-color .3s}
  .drop-zone.dragover{border-color:#e0a85c}
  .drop-zone svg{width:40px;height:40px;fill:#555;margin-bottom:8px}
  .drop-zone p{color:#888;font-size:14px}
  .drop-zone .hint{color:#555;font-size:12px;margin-top:6px}
  .preview{display:none;margin-top:12px;text-align:center}
  .preview img{max-width:100%;max-height:180px;border-radius:6px;border:1px solid #333}
  .progress{display:none;width:100%;height:4px;background:#333;border-radius:2px;margin-top:12px;overflow:hidden}
  .progress .bar{width:0%;height:100%;background:#e0a85c;transition:width .3s}
  .hint{color:#555;font-size:12px;margin-top:4px}
</style>
</head>
<body>
<div class="app">
<h1>EPD 墨水屏</h1>
<div class="sub" id="mdns">epd-weather.local</div>

<div class="tabs">
  <div class="tab active" data-tab="network" onclick="switchTab('network')">网络</div>
  <div class="tab" data-tab="weather" onclick="switchTab('weather')">天气</div>
  <div class="tab" data-tab="screen" onclick="switchTab('screen')">屏幕</div>
  <div class="tab" data-tab="upload" onclick="switchTab('upload')">图片</div>
</div>

<!-- ========== 网络标签 ========== -->
<div class="card active" id="tab-network">
  <label>WiFi 名称 (SSID)</label>
  <select id="ssid"><option value="">-- 请选择 --</option></select>
  <button class="btn sm" onclick="scanWifi()">重新扫描</button>
  <label>WiFi 密码</label>
  <input type="password" id="pass" placeholder="不填=无密码" autocomplete="off">
  <label>城市（天气用，英文）</label>
  <input type="text" id="city" placeholder="Beijing">
  <label>天气刷新间隔（分钟，最小 5）</label>
  <input type="number" id="refresh" min="5" max="180" value="30">
  <button class="btn" onclick="saveNetwork()">保存网络并重启</button>
  <button class="btn sm danger" onclick="resetAll()">重置所有设置</button>
  <div id="netStatus" class="status"></div>
</div>

<!-- ========== 天气标签 ========== -->
<div class="card" id="tab-weather">
  <label>城市（英文）</label>
  <input type="text" id="w_city" placeholder="Beijing">
  <label>刷新间隔（分钟）</label>
  <input type="number" id="w_refresh" min="5" max="180" value="30">
  <button class="btn" onclick="saveWeather()">保存天气设置</button>
  <button class="btn sm" onclick="refreshWeather()">立即刷新天气</button>
  <div id="weatherStatus" class="status"></div>
  <div style="margin-top:14px;background:#0d0d0d;border-radius:8px;padding:12px">
    <div class="info-row"><span class="info-label">当前温度</span><span class="info-val" id="s_temp">--</span></div>
    <div class="info-row"><span class="info-label">天气</span><span class="info-val" id="s_desc">--</span></div>
    <div class="info-row"><span class="info-label">上次更新</span><span class="info-val" id="s_wtime">--</span></div>
    <div class="info-row"><span class="info-label">城市</span><span class="info-val" id="s_city">--</span></div>
    <div class="info-row"><span class="info-label">刷新间隔</span><span class="info-val" id="s_refresh">--</span></div>
  </div>
</div>

<!-- ========== 屏幕标签 ========== -->
<div class="card" id="tab-screen">
  <div style="background:#0d0d0d;border-radius:8px;padding:12px;margin-bottom:12px">
    <div class="info-row"><span class="info-label">IP 地址</span><span class="info-val" id="s_ip">--</span></div>
    <div class="info-row"><span class="info-label">WiFi</span><span class="info-val" id="s_ssid">--</span></div>
    <div class="info-row"><span class="info-label">上次图片上传</span><span class="info-val" id="s_imgtime">--</span></div>
  </div>
  <button class="btn" onclick="doRefresh()">刷新屏幕</button>
  <button class="btn sm" onclick="doClear()">清屏</button>
  <div id="screenStatus" class="status"></div>
</div>

<!-- ========== 图片标签 ========== -->
<div class="card" id="tab-upload">
  <div class="drop-zone" id="dropZone">
    <svg viewBox="0 0 24 24"><path d="M19.35 10.04C18.67 6.59 15.64 4 12 4 9.11 4 6.6 5.64 5.35 8.04 2.34 8.36 0 10.91 0 14c0 3.31 2.69 6 6 6h13c2.76 0 5-2.24 5-5 0-2.64-2.05-4.78-4.65-4.96zM14 13v4h-4v-4H7l5-5 5 5h-3z"/></svg>
    <p>点击或拖拽图片到此处</p>
    <div class="hint">支持任意图片格式，浏览器端自动缩放为 212×104 三色</div>
  </div>
  <input type="file" id="fileInput" name="file" accept="image/*" style="display:none">
  <div class="preview" id="preview"><img id="previewImg"><p id="fileInfo" style="color:#888;font-size:12px;margin-top:4px"></p></div>
  <button class="btn" id="uploadBtn" disabled onclick="doUpload()">上传并显示</button>
  <div class="progress" id="progress"><div class="bar" id="progressBar"></div></div>
  <div id="uploadStatus" class="status"></div>
</div>

</div>

<script>
// ========== 标签切换 ==========
function switchTab(name){
  document.querySelectorAll('.tab').forEach(function(t){t.classList.remove('active');});
  document.querySelectorAll('.card').forEach(function(c){c.classList.remove('active');});
  document.querySelector('.tab[data-tab="'+name+'"]').classList.add('active');
  document.getElementById('tab-'+name).classList.add('active');
  if(name=='weather')loadWeatherStatus();
  if(name=='screen')loadScreenStatus();
}

// ========== 网络 ==========
function scanWifi(){
  var sel=document.getElementById('ssid');
  sel.innerHTML='<option value="">扫描中...</option>';
  fetch('/scan').then(function(r){return r.json();}).then(function(nets){
    sel.innerHTML='';
    if(!nets.length){sel.innerHTML='<option value="">未扫描到网络</option>';return;}
    nets.forEach(function(n){var o=document.createElement('option');o.value=n;o.textContent=n;sel.appendChild(o);});
  }).catch(function(){sel.innerHTML='<option value="">扫描失败</option>';});
}

function saveNetwork(){
  var ssid=document.getElementById('ssid').value;
  var pass=document.getElementById('pass').value;
  var city=document.getElementById('city').value;
  var refresh=document.getElementById('refresh').value;
  var st=document.getElementById('netStatus');
  st.className='status';st.style.display='none';
  if(!ssid){st.className='status err';st.style.display='block';st.textContent='请选择 WiFi';return;}
  fetch('/save',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)+'&city='+encodeURIComponent(city)+'&refresh='+refresh+'&weather=1&image=1'
  }).then(function(r){return r.text();}).then(function(msg){
    st.className='status ok';st.style.display='block';
    st.textContent=msg;setTimeout(function(){window.location.href='/';},3000);
  }).catch(function(e){
    st.className='status err';st.style.display='block';
    st.textContent=''+e;
  });
}

function resetAll(){
  if(!confirm('确定要重置所有设置吗？设备将重启'))return;
  fetch('/reset',{method:'POST'}).then(function(r){return r.text();}).then(function(msg){
    alert(msg);
  });
}

// ========== 天气 ==========
function loadWeatherStatus(){
  fetch('/status').then(function(r){return r.json();}).then(function(d){
    document.getElementById('s_temp').textContent=d.temp||'--';
    document.getElementById('s_desc').textContent=d.desc||'--';
    document.getElementById('s_wtime').textContent=d.weather_time||'--';
    document.getElementById('s_city').textContent=d.city||'--';
    document.getElementById('s_refresh').textContent=d.refresh+' 分钟';
    document.getElementById('w_city').value=d.city||'';
    document.getElementById('w_refresh').value=d.refresh||30;
  }).catch(function(){});
}

function saveWeather(){
  var city=document.getElementById('w_city').value;
  var refresh=document.getElementById('w_refresh').value;
  var st=document.getElementById('weatherStatus');
  st.className='status';st.style.display='none';
  fetch('/weather_save',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'city='+encodeURIComponent(city)+'&refresh='+refresh
  }).then(function(r){return r.text();}).then(function(msg){
    st.className='status ok';st.style.display='block';
    st.textContent=msg;
    loadWeatherStatus();
  }).catch(function(e){
    st.className='status err';st.style.display='block';
    st.textContent=''+e;
  });
}

function refreshWeather(){
  var st=document.getElementById('weatherStatus');
  st.className='status';st.style.display='none';
  fetch('/refresh',{method:'GET'}).then(function(r){return r.text();}).then(function(msg){
    st.className='status ok';st.style.display='block';
    st.textContent=msg;
    loadWeatherStatus();
  }).catch(function(e){
    st.className='status err';st.style.display='block';
    st.textContent=''+e;
  });
}

// ========== 屏幕 ==========
function loadScreenStatus(){
  fetch('/status').then(function(r){return r.json();}).then(function(d){
    document.getElementById('s_ip').textContent=d.ip||'--';
    document.getElementById('s_ssid').textContent=d.ssid||'--';
    document.getElementById('s_imgtime').textContent=d.image_time||'--';
  }).catch(function(){});
}

function doRefresh(){
  var st=document.getElementById('screenStatus');
  st.className='status';st.style.display='none';
  fetch('/refresh').then(function(r){return r.text();}).then(function(msg){
    st.className='status ok';st.style.display='block';
    st.textContent=msg;
  }).catch(function(e){
    st.className='status err';st.style.display='block';
    st.textContent=''+e;
  });
}

function doClear(){
  var st=document.getElementById('screenStatus');
  st.className='status';st.style.display='none';
  fetch('/clear').then(function(r){return r.text();}).then(function(msg){
    st.className='status ok';st.style.display='block';
    st.textContent=msg;
  }).catch(function(e){
    st.className='status err';st.style.display='block';
    st.textContent=''+e;
  });
}

// ========== 图片上传（客户端预处理） ==========
(function(){
  var dz=document.getElementById('dropZone');
  var fi=document.getElementById('fileInput');
  var pr=document.getElementById('preview');
  var pi=document.getElementById('previewImg');
  var finfo=document.getElementById('fileInfo');
  var btn=document.getElementById('uploadBtn');
  var st=document.getElementById('uploadStatus');
  var prog=document.getElementById('progress');
  var pbar=document.getElementById('progressBar');
  var selectedFile=null;
  var selectedDataUrl=null;
  dz.addEventListener('click',function(){fi.click();});
  dz.addEventListener('dragover',function(e){e.preventDefault();dz.classList.add('dragover');});
  dz.addEventListener('dragleave',function(){dz.classList.remove('dragover');});
  dz.addEventListener('drop',function(e){e.preventDefault();dz.classList.remove('dragover');if(e.dataTransfer.files.length)fi.files=e.dataTransfer.files;fileSelected();});
  fi.addEventListener('change',fileSelected);
  function fileSelected(){
    if(fi.files.length&&fi.files[0].size>0){
      var f=fi.files[0];
      if(f.size>5*1024*1024){showUploadStatus('图片超过 5MB 限制','err');return;}
      selectedFile=f;
      var reader=new FileReader();
      reader.onload=function(e){
        selectedDataUrl=e.target.result;
        pi.src=selectedDataUrl;
        pr.style.display='block';
        finfo.textContent=f.name+' ('+Math.round(f.size/1024)+'KB) → 将预处理为 212×104 三色';
        btn.disabled=false;
      };
      reader.readAsDataURL(f);
    }else{
      pr.style.display='none';
      btn.disabled=true;
      selectedFile=null;
      selectedDataUrl=null;
    }
  }
  window.doUpload=function(){
    if(!selectedDataUrl){showUploadStatus('请先选择文件','err');return;}
    st.className='status';st.style.display='none';
    prog.style.display='block';pbar.style.width='5%';
    btn.disabled=true;

    // 创建离屏 Canvas 进行预处理
    var canvas=document.createElement('canvas');
    canvas.width=212;
    canvas.height=104;
    var ctx=canvas.getContext('2d');
    // 填充白色背景
    ctx.fillStyle='#ffffff';
    ctx.fillRect(0,0,212,104);

    var img=new Image();
    img.onload=function(){
      // 计算居中裁剪：保持比例缩放至填满 212x104
      var targetRatio=212/104;
      var imgRatio=img.width/img.height;
      var sx,sy,srcW,srcH;
      if(imgRatio>targetRatio){
        // 图片更宽→裁剪两侧
        srcH=img.height;
        srcW=img.height*targetRatio;
        sx=(img.width-srcW)/2;
        sy=0;
      }else{
        // 图片更高→裁剪上下
        srcW=img.width;
        srcH=img.width/targetRatio;
        sx=0;
        sy=(img.height-srcH)/2;
      }
      ctx.drawImage(img,sx,sy,srcW,srcH,0,0,212,104);

      pbar.style.width='20%';

      // 读取像素数据
      var imageData=ctx.getImageData(0,0,212,104);
      var data=imageData.data;
      var W=212,H=104;
      var FSIZE=W*H/8; // 2756

      // 打包为 BW 和 RE 位面
      var bw=new Uint8Array(FSIZE);
      var re=new Uint8Array(FSIZE);

      for(var y=0;y<H;y++){
        for(var x=0;x<W;x++){
          var idx=(y*W+x)*4;
          var r=data[idx];
          var g=data[idx+1];
          var b=data[idx+2];

          // 三色转换：红 / 黑 / 白
          var color;
          if(r>150&&r>g*1.5&&r>b*1.5){
            color=2; // 红色
          }else{
            // 亮度阈值
            var brightness=(r*77+g*150+b*29)>>8;
            color=(brightness<128)?1:0; // 黑或白
          }

          var bitIdx=(y*W+x)%8;
          var byteIdx=Math.floor((y*W+x)/8);
          var mask=0x80>>bitIdx;

          if(color==1) bw[byteIdx]|=mask;
          else if(color==2) re[byteIdx]|=mask;
        }
      }

      pbar.style.width='40%';

      // 合并 BW + RE = 5512 字节
      var total=new Uint8Array(FSIZE*2);
      total.set(bw,0);
      total.set(re,FSIZE);

      // 创建 Blob 用于上传
      var blob=new Blob([total.buffer],{type:'application/octet-stream'});
      var fd=new FormData();
      fd.append('file',blob,'epd_raw.bin');

      pbar.style.width='50%';

      // 上传
      var xhr=new XMLHttpRequest();
      xhr.upload.onprogress=function(e){
        if(e.lengthComputable) pbar.style.width=50+Math.round(e.loaded/e.total*40)+'%';
      };
      xhr.onload=function(){
        pbar.style.width='100%';
        setTimeout(function(){prog.style.display='none';pbar.style.width='0%';},500);
        btn.disabled=false;
        if(xhr.status==200){
          showUploadStatus(xhr.responseText,'ok');
        }else{
          showUploadStatus(xhr.status+' '+xhr.responseText,'err');
        }
      };
      xhr.onerror=function(){
        prog.style.display='none';
        btn.disabled=false;
        showUploadStatus('网络错误，请重试','err');
      };
      xhr.open('POST','/upload');
      xhr.send(fd);
    };
    img.src=selectedDataUrl;
  };
  function showUploadStatus(msg,type){
    st.className='status '+type;st.style.display='block';
    st.textContent=(type=='ok'?'✓ ':'✗ ')+msg;
  }
})();

// ========== 初始化 ==========
scanWifi();
loadScreenStatus();
</script>
</body>
</html>
)rawliteral";

// ========== 配网页（AP 模式，简化版，仅网络配置） ==========
static const char PAGE_CONFIG[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>EPD 墨水屏 - 配置</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#eee;display:flex;justify-content:center;align-items:center;min-height:100vh;padding:16px}
  .card{background:#16213e;border:1px solid #333;border-radius:12px;padding:28px;max-width:420px;width:100%}
  h2{color:#e0a85c;margin-bottom:6px;font-weight:500}
  .sub{color:#888;font-size:13px;margin-bottom:20px}
  label{display:block;margin:12px 0 4px;color:#aaa;font-size:13px}
  input,select{width:100%;padding:10px 12px;border:1px solid #333;border-radius:6px;background:#0d0d0d;color:#eee;font-size:15px}
  input:focus,select:focus{border-color:#e0a85c;outline:none}
  .btn{width:100%;padding:12px;background:#e0a85c;color:#1a1a2e;border:none;border-radius:6px;font-size:16px;font-weight:600;cursor:pointer;margin-top:16px}
  .btn:hover{background:#c8913e}
  .status{margin-top:14px;padding:10px;border-radius:6px;text-align:center;display:none;font-size:14px}
  .status.ok{display:block;background:#1a3a2e;color:#80a894;border:1px solid #2a5a3e}
  .status.err{display:block;background:#3a1a1a;color:#f88;border:1px solid #5a2a2a}
  .hint{color:#555;font-size:12px;margin-top:4px}
</style>
</head>
<body>
<div class="card">
<h2>EPD 墨水屏配置</h2>
<div class="sub">连接 EPD-Config 热点 → 填写 WiFi 信息</div>
<form id="cfg" onsubmit="return submitCfg()">
<label>WiFi 名称 (SSID)</label>
<select id="ssid" required><option value="">-- 扫描中 --</option></select>
<label>WiFi 密码</label>
<input type="password" id="pass" placeholder="不填=无密码" autocomplete="off">
<button type="submit">保存并重启</button>
</form>
<div id="status" class="status"></div>
</div>
<script>
fetch('/scan').then(r=>r.json()).then(nets=>{
  var sel=document.getElementById('ssid');sel.innerHTML='';
  if(!nets.length){sel.innerHTML='<option value="">未扫描到网络</option>';return;}
  nets.forEach(function(n){var o=document.createElement('option');o.value=n;o.textContent=n;sel.appendChild(o);});
}).catch(function(){document.getElementById('ssid').innerHTML='<option value="">扫描失败</option>';});
function submitCfg(){
  var ssid=document.getElementById('ssid').value;
  var pass=document.getElementById('pass').value;
  var st=document.getElementById('status');
  st.className='status';st.style.display='none';
  fetch('/save',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)+'&city=Beijing&refresh=30&weather=1&image=1'
  }).then(function(r){return r.text();}).then(function(msg){
    st.className='status ok';st.style.display='block';
    st.textContent=msg;setTimeout(function(){window.location.href='/';},3000);
  }).catch(function(e){
    st.className='status err';st.style.display='block';
    st.textContent=''+e;
  });
  return false;
}
</script>
</body>
</html>
)rawliteral";

// 兼容旧定义（不上传页，已合并到主页面）
// upload page 不再需要单独定义