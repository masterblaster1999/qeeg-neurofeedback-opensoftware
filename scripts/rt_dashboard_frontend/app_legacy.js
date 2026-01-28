/* app_legacy.js

ES5-compatible dashboard frontend.

This script is loaded via <script nomodule> from index.html. It is intended to keep
basic real-time qEEG visualization working on older browsers/devices by avoiding
ES2015+ syntax (const/let/arrow/template strings/etc).

Transport strategy:
  - Prefer multiplexed SSE (/api/sse/stream) if EventSource is available and supported.
  - Fall back to per-topic SSE endpoints.
  - If EventSource is unavailable, fall back to polling snapshots (/api/snapshot) using XHR.
*/

(function(){
  'use strict';

  // Polyfills (minimal)
  if(!Math.log10){
    Math.log10 = function(x){ return Math.log(x) / Math.LN10; };
  }
  var _raf = window.requestAnimationFrame || function(fn){ return setTimeout(fn, 16); };

  function $(id){ return document.getElementById(id); }

  function getParam(name){
    var s = window.location.search || '';
    // Simple regex param parse (no URLSearchParams in old browsers)
    var re = new RegExp('[?&]' + name + '=([^&]+)');
    var m = re.exec(s);
    if(!m) return '';
    try {
      return decodeURIComponent(m[1].replace(/\+/g, ' '));
    } catch(e){
      return m[1];
    }
  }

  var TOKEN = getParam('token');

  // DOM
  var statusBox = $('status');
  var nfConn = $('nfConn');
  var bpConn = $('bpConn');
  var artConn = $('artConn');
  var metaConn = $('metaConn');
  var stateConn = $('stateConn');

  var runMetaStatus = $('runMetaStatus');
  var serverStats = $('serverStats');
  var runMetaDetails = $('runMetaDetails');
  var runMetaKv = $('runMetaKv');
  var runMetaRaw = $('runMetaRaw');
  var runMetaDownload = $('runMetaDownload');
  var runMetaRefresh = $('runMetaRefresh');
  var statsKv = $('statsKv');

  var downloadsDetails = $('downloadsDetails');
  var bundleDownload = $('bundleDownload');
  var filesRefresh = $('filesRefresh');
  var filesList = $('filesList');
  var filesHint = $('filesHint');

  var winSel = $('winSel');
  var pauseBtn = $('pauseBtn');

  var nfStats = $('nfStats');

  var nfCanvas = $('nfChart');
  var bpCanvas = $('bpChart');
  var topoCanvas = $('topoCanvas');

  var bandSel = $('bandSel');
  var chSel = $('chSel');
  var xformSel = $('xformSel');
  var scaleSel = $('scaleSel');
  var lblSel = $('lblSel');

  var vminEl = $('vmin');
  var vmaxEl = $('vmax');

  if(!nfCanvas || !bpCanvas || !topoCanvas){
    // Not on the expected page.
    return;
  }

  var nfCtx = nfCanvas.getContext('2d');
  var bpCtx = bpCanvas.getContext('2d');
  var topoCtx = topoCanvas.getContext('2d');

  function setBadge(el, txt, cls){
    if(!el) return;
    el.textContent = txt;
    el.className = 'badge' + (cls ? (' ' + cls) : '');
  }

  function showStatus(html){
    if(!statusBox) return;
    statusBox.style.display = 'block';
    statusBox.innerHTML = '<div class="warnbox">' + html + '</div>';
  }

  function hideStatus(){
    if(!statusBox) return;
    statusBox.style.display = 'none';
    statusBox.innerHTML = '';
  }

  function nowSec(){
    return (new Date()).getTime() / 1000.0;
  }

  function fmt(x){
    if(x === null || x === undefined || !isFinite(x)) return '—';
    var ax = Math.abs(x);
    if(ax >= 100) return x.toFixed(1);
    if(ax >= 10) return x.toFixed(2);
    return x.toFixed(3);
  }

  function fmtAgeSec(age){
    if(age === null || age === undefined || !isFinite(age)) return '—';
    if(age < 1.0) return '<1s';
    if(age < 60) return String(Math.round(age)) + 's';
    var m = Math.floor(age/60);
    var s = Math.round(age - 60*m);
    return String(m) + 'm ' + String(s) + 's';
  }

  // Client id for ui state sync
  var CLIENT_ID = '';
  try {
    CLIENT_ID = (window.localStorage && window.localStorage.getItem('qeeg_dash_client_id')) || '';
  } catch(e){ CLIENT_ID = ''; }
  if(!CLIENT_ID){
    CLIENT_ID = 'c_' + String(Math.floor(Math.random()*1e9)) + '_' + String(Date.now ? Date.now() : (new Date()).getTime());
    try {
      if(window.localStorage){ window.localStorage.setItem('qeeg_dash_client_id', CLIENT_ID); }
    } catch(e){}
  }
  function fmtBytes(n){
    if(n === null || n === undefined || !isFinite(n)) return '—';
    var units = ['B','KiB','MiB','GiB','TiB'];
    var v = Math.max(0, n);
    var u = 0;
    while(v >= 1024 && u < units.length-1){ v = v / 1024; u++; }
    if(u === 0) return String(Math.round(v)) + ' ' + units[u];
    var digs = (v >= 10) ? 1 : 2;
    return v.toFixed(digs) + ' ' + units[u];
  }


  var state = {
    winSec: 60,
    paused: false,
    applyingRemote: false,
    pendingUi: null,
    nf: {frames: [], lastT: -Infinity},
    bp: {meta: null, frames: [], lastT: -Infinity, rollingMin: null, rollingMax: null, tmpCanvas: null, tmpW: 0, tmpH: 0, electrodesPx: []},
    art: {frames: [], latest: null, lastT: -Infinity}
  };

  var runMetaETag = null;
  var statsTimer = null;

  var dirtyNf = true;
  var dirtyBp = true;
  var renderScheduled = false;

  function resizeCanvasTo(canvas){
    var dpr = window.devicePixelRatio || 1;
    var rect = canvas.getBoundingClientRect();
    var w = Math.max(1, Math.round(rect.width * dpr));
    var h = Math.max(1, Math.round(rect.height * dpr));
    if(canvas.width !== w || canvas.height !== h){
      canvas.width = w;
      canvas.height = h;
    }
  }

  function scheduleRender(){
    if(renderScheduled) return;
    renderScheduled = true;
    _raf(function(){
      renderScheduled = false;
      if(dirtyNf){
        dirtyNf = false;
        drawNfChart();
      }
      if(dirtyBp){
        dirtyBp = false;
        drawTopo();
        drawBandpowerSeries();
      }
    });
  }

  function updateRunMetaStatus(meta){
    if(!runMetaStatus) return;
    var rm = meta && meta.run_meta ? meta.run_meta : null;
    var st = (rm && rm.stat) ? rm.stat : (meta && meta.files_stat ? meta.files_stat.nf_run_meta : null);
    if(st && st.exists){
      var now = Date.now()/1000;
      var age = st.mtime_utc ? (now - st.mtime_utc) : null;
      runMetaStatus.textContent = 'present (age: ' + fmtAgeSec(age) + ')';
      if(runMetaStatus.classList) runMetaStatus.classList.remove('muted');
      if(rm && rm.summary){
        var order = ['Tool','Version','GitDescribe','TimestampLocal','protocol','metric_spec','band_spec','reward_direction','fs_hz','window_seconds','update_seconds','baseline_seconds','target_reward_rate','artifact_gate','qc_bad_channel_count','qc_bad_channels','biotrace_ui','export_derived_events'];
        renderKvGrid(runMetaKv, rm.summary, order);
      }
      if(rm && rm.parse_error){
        if(runMetaRaw) runMetaRaw.textContent = 'Parse error: ' + rm.parse_error;
      }
    } else {
      runMetaStatus.textContent = 'missing';
      if(runMetaStatus.classList) runMetaStatus.classList.add('muted');
      renderKvGrid(runMetaKv, null);
      if(runMetaRaw) runMetaRaw.textContent = '';
    }
    if(runMetaDownload && TOKEN){
      runMetaDownload.href = '/api/run_meta?token=' + encodeURIComponent(TOKEN) + '&format=raw';
    }
  }

  function xformValue(v){
    if(v === null || v === undefined || !isFinite(v)) return null;
    var mode = (xformSel && xformSel.value) ? xformSel.value : 'linear';
    if(mode === 'linear') return v;
    var eps = 1e-12;
    if(v <= eps) return null;
    if(mode === 'log10') return Math.log10(v);
    if(mode === 'db') return 10.0 * Math.log10(v);
    return v;
  }

  function escHtml(s){
    return String(s).replace(/[&<>"']/g, function(ch){
      return ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[ch] || ch);
    });
  }

  function renderKvGrid(el, obj, order){
    if(!el) return;
    if(!obj || (typeof obj !== 'object')){
      el.innerHTML = '<span class="muted">—</span>';
      return;
    }
    var keys = (order && order.length) ? order : Object.keys(obj);
    var parts = [];
    for(var i=0;i<keys.length;i++){
      var k = keys[i];
      if(!Object.prototype.hasOwnProperty.call(obj, k)) continue;
      var v = obj[k];
      if(v === null || v === undefined) v = '';
      if(typeof v === 'object'){
        try{ v = JSON.stringify(v); }catch(e){ v = String(v); }
      }
      parts.push('<span>' + escHtml(k) + '</span><b>' + escHtml(v) + '</b>');
    }
    el.innerHTML = parts.join('') || '<span class="muted">—</span>';
  }

  function updateFileStatus(meta){
    var fs = (meta && meta.files_stat) ? meta.files_stat : null;
    if(!fs) return;

    var now = nowSec();
    var rows = [];
    var anyMissing = false;
    var anyStale = false;

    function add(label, st){
      var ok = !!(st && st.exists);
      if(!ok) anyMissing = true;
      var age = (ok && st.mtime_utc) ? (now - st.mtime_utc) : null;
      if(ok && age !== null && age > 5) anyStale = true;
      var cls = ok ? ((age !== null && age > 5) ? 'warn' : 'good') : 'bad';
      var size = ok ? (st.size_bytes || 0) : '—';
      var html = '';
      html += '<div class="row" style="justify-content:space-between; margin:3px 0">';
      html += '<span><b>' + label + '</b> <span class="badge ' + cls + '" style="margin-left:8px">' + (ok ? 'ok' : 'missing') + '</span></span>';
      html += '<span class="small">age: ' + fmtAgeSec(age) + ' &nbsp; size: ' + size + '</span>';
      html += '</div>';
      rows.push(html);
    }

    add('nf_feedback.csv', fs.nf_feedback);
    add('bandpower_timeseries.csv', fs.bandpower_timeseries);
    add('artifact_gate_timeseries.csv', fs.artifact_gate_timeseries);

    if(anyMissing){
      showStatus('<div style="margin-bottom:6px"><b>Waiting on outputs…</b> (start qeeg_nf_cli, and consider <code>--flush-csv</code> for live updates)</div>' + rows.join(''));
    } else if(anyStale){
      showStatus('<div style="margin-bottom:6px"><b>Outputs look stale</b> (no new writes in &gt;5s). Is the session paused or finished?</div>' + rows.join(''));
    } else {
      hideStatus();
    }
  }

  function updateStats(){
    var frames = state.nf.frames;
    var f = frames.length ? frames[frames.length-1] : null;
    var a = state.art.latest;
    var parts = [];

    function add(k,v){ parts.push('<span>' + k + '</span><b>' + v + '</b>'); }

    if(f){
      add('t (s)', fmt(f.t));
      add('metric', fmt(f.metric));
      add('threshold', fmt(f.threshold));
      add('reward', (f.reward ? '1' : '0'));
      add('reward_rate', fmt(f.reward_rate));
      if(f.artifact_ready !== null && f.artifact_ready !== undefined) add('artifact_ready', String(f.artifact_ready));
      if(f.phase) add('phase', String(f.phase));
      if(f.bad_channels !== null && f.bad_channels !== undefined) add('bad_ch', String(f.bad_channels));
    } else {
      add('status', 'waiting for nf_feedback.csv');
    }

    if(a){
      add('artifact', a.bad ? '1' : '0');
      add('bad_ch', String(a.bad_channels || 0));
    }

    if(nfStats){ nfStats.innerHTML = '<div class="kv">' + parts.join('') + '</div>'; }
  }

  function unpackMsg(msg){
    if(msg && typeof msg === 'object' && msg.type === 'batch' && msg.frames && typeof msg.frames.length === 'number'){
      return {frames: msg.frames, reset: !!msg.reset};
    }
    return {frames: [msg], reset: false};
  }

  // ---------- Rendering ----------

  function drawNfChart(){
    resizeCanvasTo(nfCanvas);
    var w = nfCanvas.width;
    var h = nfCanvas.height;
    nfCtx.clearRect(0,0,w,h);
    nfCtx.fillStyle = '#071018';
    nfCtx.fillRect(0,0,w,h);

    var frames = state.nf.frames;
    if(!frames.length){
      nfCtx.fillStyle = '#9fb0c3';
      nfCtx.font = String(Math.round(12*(window.devicePixelRatio||1))) + 'px system-ui';
      nfCtx.fillText('Waiting for nf_feedback.csv …', 14*(window.devicePixelRatio||1), 24*(window.devicePixelRatio||1));
      return;
    }

    var tNow = frames[frames.length-1].t;
    var tMin = Math.max(frames[0].t, tNow - state.winSec);

    var vis = [];
    for(var i=0;i<frames.length;i++){
      var f = frames[i];
      if(f && f.t !== null && f.t !== undefined && isFinite(f.t) && f.t >= tMin){
        vis.push(f);
      }
    }
    if(vis.length < 2) return;

    var yMin = Infinity;
    var yMax = -Infinity;
    for(i=0;i<vis.length;i++){
      var ff = vis[i];
      var m = ff.metric;
      var th = ff.threshold;
      if(m !== null && m !== undefined && isFinite(m)){
        if(m < yMin) yMin = m;
        if(m > yMax) yMax = m;
      }
      if(th !== null && th !== undefined && isFinite(th)){
        if(th < yMin) yMin = th;
        if(th > yMax) yMax = th;
      }
    }
    if(!(yMax > yMin)){
      yMin = yMin - 1;
      yMax = yMin + 2;
    }
    var pad = 0.05*(yMax-yMin);
    yMin -= pad;
    yMax += pad;

    function x(t){ return (t - tMin) / (tNow - tMin) * (w-20) + 10; }
    function y(v){ return (h-20) - (v - yMin)/(yMax-yMin) * (h-30); }

    // Reward shading
    nfCtx.fillStyle = 'rgba(100, 210, 255, 0.08)';
    for(i=0;i<vis.length-1;i++){
      if(vis[i].reward){
        var x0 = x(vis[i].t);
        var x1 = x(vis[i+1].t);
        if(x1 > x0){ nfCtx.fillRect(x0, 10, Math.max(1, x1-x0), h-30); }
      }
    }

    // Artifact shading
    nfCtx.fillStyle = 'rgba(255, 107, 107, 0.10)';
    for(i=0;i<vis.length-1;i++){
      if(vis[i].artifact){
        x0 = x(vis[i].t);
        x1 = x(vis[i+1].t);
        if(x1 > x0){ nfCtx.fillRect(x0, 10, Math.max(1, x1-x0), h-30); }
      }
    }

    // Metric line
    nfCtx.strokeStyle = '#e8eef6';
    nfCtx.lineWidth = 2;
    nfCtx.beginPath();
    for(i=0;i<vis.length;i++){
      ff = vis[i];
      var xx = x(ff.t);
      var yy = y(ff.metric);
      if(i===0) nfCtx.moveTo(xx,yy);
      else nfCtx.lineTo(xx,yy);
    }
    nfCtx.stroke();

    // Threshold dashed
    nfCtx.strokeStyle = '#64d2ff';
    nfCtx.setLineDash([6,4]);
    nfCtx.lineWidth = 2;
    nfCtx.beginPath();
    for(i=0;i<vis.length;i++){
      ff = vis[i];
      xx = x(ff.t);
      yy = y(ff.threshold);
      if(i===0) nfCtx.moveTo(xx,yy);
      else nfCtx.lineTo(xx,yy);
    }
    nfCtx.stroke();
    nfCtx.setLineDash([]);

    // Labels
    nfCtx.fillStyle = '#9fb0c3';
    nfCtx.font = String(Math.round(11*(window.devicePixelRatio||1))) + 'px system-ui';
    nfCtx.fillText('t: ' + tMin.toFixed(1) + '–' + tNow.toFixed(1) + 's', 12, h-6);
    nfCtx.fillText('y: ' + fmt(yMin) + '–' + fmt(yMax), 12, 22);
  }

  function hslToRgb(h, s, l){
    // h in degrees
    h = ((h%360)+360)%360;
    s = Math.max(0, Math.min(1, s));
    l = Math.max(0, Math.min(1, l));
    var c = (1 - Math.abs(2*l - 1)) * s;
    var hp = h / 60;
    var x = c * (1 - Math.abs((hp % 2) - 1));
    var r=0,g=0,b=0;
    if(0<=hp && hp<1){ r=c; g=x; b=0; }
    else if(1<=hp && hp<2){ r=x; g=c; b=0; }
    else if(2<=hp && hp<3){ r=0; g=c; b=x; }
    else if(3<=hp && hp<4){ r=0; g=x; b=c; }
    else if(4<=hp && hp<5){ r=x; g=0; b=c; }
    else if(5<=hp && hp<6){ r=c; g=0; b=x; }
    var m = l - c/2;
    r = Math.round(255*(r+m));
    g = Math.round(255*(g+m));
    b = Math.round(255*(b+m));
    return [r,g,b];
  }

  function valueToRgb(v, vmin, vmax){
    if(v===null || v===undefined || !isFinite(v) || !(vmax>vmin)) return [42,52,64];
    var t = (v - vmin) / (vmax - vmin);
    if(t<0) t=0;
    if(t>1) t=1;
    var hue = (1 - t) * 240;
    return hslToRgb(hue, 0.90, 0.55);
  }

  function drawTopo(){
    var meta = state.bp.meta;
    if(!meta){
      resizeCanvasTo(topoCanvas);
      topoCtx.clearRect(0,0,topoCanvas.width, topoCanvas.height);
      topoCtx.fillStyle = '#9fb0c3';
      topoCtx.font = String(Math.round(12*(window.devicePixelRatio||1))) + 'px system-ui';
      topoCtx.fillText('Waiting for bandpower_timeseries.csv …', 14*(window.devicePixelRatio||1), 24*(window.devicePixelRatio||1));
      if(vminEl) vminEl.textContent = 'min: —';
      if(vmaxEl) vmaxEl.textContent = 'max: —';
      return;
    }

    var band = (bandSel && bandSel.value) ? bandSel.value : (meta.bands && meta.bands.length ? meta.bands[0] : '');
    var bIdx = 0;
    if(meta.bands){
      for(var bi=0;bi<meta.bands.length;bi++){
        if(meta.bands[bi] === band){ bIdx = bi; break; }
      }
    }

    var nCh = meta.channels ? meta.channels.length : 0;
    var latest = state.bp.frames.length ? state.bp.frames[state.bp.frames.length-1] : null;

    var vals = null;
    if(latest && latest.values && typeof latest.values.length === 'number' && meta.bands && meta.bands.length){
      var need = meta.bands.length * nCh;
      if(latest.values.length >= need){
        vals = [];
        for(var c=0;c<nCh;c++){
          vals.push(xformValue(latest.values[bIdx*nCh + c]));
        }
      }
    }

    var vmin = Infinity;
    var vmax = -Infinity;
    if(vals){
      for(var vi=0;vi<vals.length;vi++){
        var vv = vals[vi];
        if(vv !== null && vv !== undefined && isFinite(vv)){
          if(vv < vmin) vmin = vv;
          if(vv > vmax) vmax = vv;
        }
      }
    }
    if(!(vmax > vmin)){
      vmin = 0;
      vmax = 1;
    }

    if(scaleSel && scaleSel.value === 'fixed'){
      if(state.bp.rollingMin === null || state.bp.rollingMax === null){
        state.bp.rollingMin = vmin;
        state.bp.rollingMax = vmax;
      } else {
        state.bp.rollingMin = 0.98*state.bp.rollingMin + 0.02*vmin;
        state.bp.rollingMax = 0.98*state.bp.rollingMax + 0.02*vmax;
      }
      vmin = state.bp.rollingMin;
      vmax = state.bp.rollingMax;
    }

    if(vminEl) vminEl.textContent = 'min: ' + (isFinite(vmin) ? vmin.toFixed(3) : '—');
    if(vmaxEl) vmaxEl.textContent = 'max: ' + (isFinite(vmax) ? vmax.toFixed(3) : '—');

    resizeCanvasTo(topoCanvas);
    var w = topoCanvas.width;
    var h = topoCanvas.height;
    topoCtx.clearRect(0,0,w,h);

    var dpr = window.devicePixelRatio || 1;
    var cx = w/2;
    var cy = h/2 + 6*dpr;
    var R = Math.min(w, h) * 0.42;

    topoCtx.fillStyle = '#071018';
    topoCtx.fillRect(0,0,w,h);

    // Interpolate on a small grid to keep it fast.
    var grid = 120;
    var imgW = grid;
    var imgH = grid;
    var img = topoCtx.createImageData(imgW, imgH);
    var eps = 1e-6;
    var p = 2.0;

    var pts = [];
    if(vals && meta.positions){
      for(var i=0;i<nCh;i++){
        var pos = meta.positions[i];
        var v = vals[i];
        if(!pos) continue;
        if(v === null || v === undefined || !isFinite(v)) continue;
        pts.push({x: pos[0], y: pos[1], v: v});
      }
    }

    for(var gy=0; gy<imgH; gy++){
      for(var gx=0; gx<imgW; gx++){
        var xh = (gx/(imgW-1))*2 - 1;
        var yh = (gy/(imgH-1))*2 - 1;
        var rr = xh*xh + yh*yh;
        var idx = (gy*imgW + gx)*4;
        if(rr > 1.0){
          img.data[idx+3] = 0;
          continue;
        }
        if(!pts.length){
          img.data[idx+0]=42; img.data[idx+1]=52; img.data[idx+2]=64; img.data[idx+3]=255;
          continue;
        }
        var sw = 0;
        var sv = 0;
        for(var pi=0; pi<pts.length; pi++){
          var pt = pts[pi];
          var dx = xh - pt.x;
          var dy = yh - pt.y;
          var d2 = dx*dx + dy*dy;
          var wgt = 1.0 / (Math.pow(d2 + eps, p/2));
          sw += wgt;
          sv += wgt * pt.v;
        }
        var vv2 = (sw > 0) ? (sv/sw) : null;
        var rgb = valueToRgb(vv2, vmin, vmax);
        img.data[idx+0] = rgb[0];
        img.data[idx+1] = rgb[1];
        img.data[idx+2] = rgb[2];
        img.data[idx+3] = 255;
      }
    }

    topoCtx.save();
    topoCtx.beginPath();
    topoCtx.arc(cx, cy, R, 0, Math.PI*2);
    topoCtx.clip();
    topoCtx.imageSmoothingEnabled = true;

    var x0 = cx - R;
    var y0 = cy - R;
    if(!state.bp.tmpCanvas){ state.bp.tmpCanvas = document.createElement('canvas'); }
    var tmp = state.bp.tmpCanvas;
    if(state.bp.tmpW !== imgW || state.bp.tmpH !== imgH){ tmp.width = imgW; tmp.height = imgH; state.bp.tmpW = imgW; state.bp.tmpH = imgH; }
    tmp.getContext('2d').putImageData(img, 0, 0);
    topoCtx.drawImage(tmp, x0, y0, 2*R, 2*R);
    topoCtx.restore();

    topoCtx.strokeStyle = 'rgba(255,255,255,0.25)';
    topoCtx.lineWidth = 2*dpr;
    topoCtx.beginPath();
    topoCtx.arc(cx, cy, R, 0, Math.PI*2);
    topoCtx.stroke();

    // Simple nose + ears
    topoCtx.beginPath();
    topoCtx.moveTo(cx - 0.08*R, cy - 1.02*R);
    topoCtx.lineTo(cx, cy - 1.12*R);
    topoCtx.lineTo(cx + 0.08*R, cy - 1.02*R);
    topoCtx.stroke();

    topoCtx.beginPath();
    topoCtx.moveTo(cx - 1.02*R, cy - 0.18*R);
    topoCtx.quadraticCurveTo(cx - 1.12*R, cy, cx - 1.02*R, cy + 0.18*R);
    topoCtx.stroke();
    topoCtx.beginPath();
    topoCtx.moveTo(cx + 1.02*R, cy - 0.18*R);
    topoCtx.quadraticCurveTo(cx + 1.12*R, cy, cx + 1.02*R, cy + 0.18*R);
    topoCtx.stroke();

    var showLabels = (!lblSel || lblSel.value === 'on');
    state.bp.electrodesPx = [];
    topoCtx.font = String(Math.round(11*dpr)) + 'px system-ui';
    topoCtx.textAlign = 'center';
    topoCtx.textBaseline = 'middle';

    for(i=0;i<nCh;i++){
      pos = meta.positions ? meta.positions[i] : null;
      if(!pos) continue;
      var ex = cx + pos[0]*R;
      var ey = cy - pos[1]*R;
      state.bp.electrodesPx.push([ex, ey, i]);

      topoCtx.beginPath();
      topoCtx.arc(ex, ey, 7*dpr, 0, Math.PI*2);
      topoCtx.fillStyle = 'rgba(0,0,0,0.35)';
      topoCtx.fill();

      var src = (meta.positions_source && meta.positions_source[i]) ? meta.positions_source[i] : '';
      topoCtx.strokeStyle = (src === 'fallback') ? 'rgba(255,207,92,0.65)' : 'rgba(255,255,255,0.35)';
      topoCtx.lineWidth = 1.5*dpr;
      topoCtx.stroke();

      if(showLabels && meta.channels){
        topoCtx.fillStyle = 'rgba(255,255,255,0.78)';
        topoCtx.fillText(meta.channels[i], ex, ey);
      }
    }
  }

  function drawBandpowerSeries(){
    var meta = state.bp.meta;
    resizeCanvasTo(bpCanvas);
    var w = bpCanvas.width;
    var h = bpCanvas.height;
    bpCtx.clearRect(0,0,w,h);
    bpCtx.fillStyle = '#071018';
    bpCtx.fillRect(0,0,w,h);

    if(!meta || !state.bp.frames.length){
      bpCtx.fillStyle = '#9fb0c3';
      bpCtx.font = String(Math.round(12*(window.devicePixelRatio||1))) + 'px system-ui';
      bpCtx.fillText('Waiting for bandpower_timeseries.csv …', 14*(window.devicePixelRatio||1), 24*(window.devicePixelRatio||1));
      return;
    }

    var band = (bandSel && bandSel.value) ? bandSel.value : (meta.bands && meta.bands.length ? meta.bands[0] : '');
    var bIdx = 0;
    if(meta.bands){
      for(var bi=0;bi<meta.bands.length;bi++){
        if(meta.bands[bi] === band){ bIdx = bi; break; }
      }
    }

    var chName = (chSel && chSel.value) ? chSel.value : (meta.channels && meta.channels.length ? meta.channels[0] : '');
    var cIdx = 0;
    if(meta.channels){
      for(var ci=0;ci<meta.channels.length;ci++){
        if(meta.channels[ci] === chName){ cIdx = ci; break; }
      }
    }

    var nCh = meta.channels ? meta.channels.length : 0;
    var frames = state.bp.frames;
    var tNow = frames[frames.length-1].t;
    var tMin = Math.max(frames[0].t, tNow - state.winSec);

    var vis = [];
    for(var i=0;i<frames.length;i++){
      var f = frames[i];
      if(f.t >= tMin && f.values && f.values.length >= (meta.bands.length*nCh)){
        vis.push({t: f.t, v: xformValue(f.values[bIdx*nCh + cIdx])});
      }
    }
    if(vis.length < 2) return;

    var yMin = Infinity;
    var yMax = -Infinity;
    for(i=0;i<vis.length;i++){
      var pv = vis[i].v;
      if(pv !== null && pv !== undefined && isFinite(pv)){
        if(pv < yMin) yMin = pv;
        if(pv > yMax) yMax = pv;
      }
    }
    if(!(yMax > yMin)){
      yMax = yMin + 1;
    }
    var pad = 0.05*(yMax-yMin);
    yMin -= pad;
    yMax += pad;

    function x(t){ return (t - tMin) / (tNow - tMin) * (w-20) + 10; }
    function y(v){ return (h-20) - (v - yMin)/(yMax-yMin) * (h-30); }

    bpCtx.strokeStyle = 'rgba(255,255,255,0.06)';
    bpCtx.lineWidth = 1;
    for(i=0;i<=4;i++){
      var yy = 10 + i*(h-30)/4;
      bpCtx.beginPath();
      bpCtx.moveTo(10,yy);
      bpCtx.lineTo(w-10,yy);
      bpCtx.stroke();
    }

    bpCtx.strokeStyle = '#e8eef6';
    bpCtx.lineWidth = 2;
    bpCtx.beginPath();
    for(i=0;i<vis.length;i++){
      var pnt = vis[i];
      var xx = x(pnt.t);
      var yy2 = y(pnt.v);
      if(i===0) bpCtx.moveTo(xx,yy2);
      else bpCtx.lineTo(xx,yy2);
    }
    bpCtx.stroke();

    bpCtx.fillStyle = '#9fb0c3';
    bpCtx.font = String(Math.round(11*(window.devicePixelRatio||1))) + 'px system-ui';
    var xm = (xformSel && xformSel.value) ? xformSel.value : 'linear';
    var xLabel = (xm && xm !== 'linear') ? (' (' + xm + ')') : '';
    bpCtx.fillText(band + ':' + chName + xLabel + '  y: ' + fmt(yMin) + '–' + fmt(yMax), 12, 22);
    bpCtx.fillText('t: ' + tMin.toFixed(1) + '–' + tNow.toFixed(1) + 's', 12, h-6);
  }

  // ---------- Networking ----------

  function xhrJson(method, url, body, cb, headers){
    var xhr = new XMLHttpRequest();
    xhr.open(method, url, true);
    xhr.onreadystatechange = function(){
      if(xhr.readyState !== 4) return;
      // Treat 304 as "ok" (used with If-None-Match).
      var ok = ((xhr.status >= 200 && xhr.status < 300) || xhr.status === 304);
      var obj = null;
      if(ok && xhr.status !== 304){
        try {
          obj = JSON.parse(xhr.responseText || 'null');
        } catch(e){
          obj = null;
        }
      }
      cb(ok ? null : new Error('HTTP ' + String(xhr.status)), obj, xhr);
    };
    xhr.onerror = function(){
      cb(new Error('network'), null, xhr);
    };
    try {
      if(headers){
        for(var k in headers){
          if(Object.prototype.hasOwnProperty.call(headers, k)){
            try{ xhr.setRequestHeader(k, headers[k]); }catch(e){}
          }
        }
      }
      if(body !== null && body !== undefined){
        xhr.setRequestHeader('Content-Type', 'application/json');
        xhr.send(JSON.stringify(body));
      } else {
        xhr.send();
      }
    } catch(e){
      cb(e, null, xhr);
    }
  }

  // ---------- UI state sync ----------

  function uiStateFromControls(){
    return {
      win_sec: parseFloat(winSel && winSel.value ? winSel.value : '60') || 60,
      paused: !!state.paused,
      band: (bandSel && bandSel.value) ? bandSel.value : null,
      channel: (chSel && chSel.value) ? chSel.value : null,
      transform: (xformSel && xformSel.value) ? xformSel.value : 'linear',
      scale: (scaleSel && scaleSel.value) ? scaleSel.value : 'auto',
      labels: (lblSel && lblSel.value) ? lblSel.value : 'on',
      client_id: CLIENT_ID
    };
  }

  var pushTimer = null;
  function schedulePushUiState(){
    if(state.applyingRemote) return;
    if(!TOKEN) return;
    if(pushTimer) clearTimeout(pushTimer);
    pushTimer = setTimeout(function(){
      pushTimer = null;
      var body = uiStateFromControls();
      xhrJson('PUT', '/api/state?token=' + encodeURIComponent(TOKEN), body, function(){ /* ignore */ });
    }, 120);
  }

  function applyUiState(st){
    if(!st || typeof st !== 'object') return;
    state.applyingRemote = true;
    try {
      if(typeof st.win_sec === 'number' && isFinite(st.win_sec)){
        var v = Math.round(st.win_sec);
        if(winSel){
          // Only set if option exists
          var ok = false;
          for(var i=0;i<winSel.options.length;i++){
            if(parseInt(winSel.options[i].value, 10) === v){ ok = true; break; }
          }
          if(ok){ winSel.value = String(v); state.winSec = v; }
        }
      }
      if(typeof st.paused === 'boolean'){
        state.paused = st.paused;
        if(pauseBtn) pauseBtn.textContent = state.paused ? 'Resume' : 'Pause';
      }
      if(typeof st.transform === 'string'){
        if(xformSel && (st.transform === 'linear' || st.transform === 'log10' || st.transform === 'db')) xformSel.value = st.transform;
      }
      if(typeof st.scale === 'string'){
        if(scaleSel && (st.scale === 'auto' || st.scale === 'fixed')) scaleSel.value = st.scale;
      }
      if(typeof st.labels === 'string'){
        if(lblSel && (st.labels === 'on' || st.labels === 'off')) lblSel.value = st.labels;
      }

      if(!state.pendingUi) state.pendingUi = {};
      if(typeof st.band === 'string' && st.band) state.pendingUi.band = st.band;
      if(typeof st.channel === 'string' && st.channel) state.pendingUi.channel = st.channel;
    } finally {
      state.applyingRemote = false;
    }
  }

  function tryApplyPendingSelection(){
    if(!state.pendingUi) return;
    var meta = state.bp.meta;
    if(!meta) return;

    var p = state.pendingUi;
    var changed = false;
    if(p.band && meta.bands){
      for(var i=0;i<meta.bands.length;i++){
        if(meta.bands[i] === p.band){
          if(bandSel) bandSel.value = p.band;
          p.band = null;
          changed = true;
          break;
        }
      }
    }
    if(p.channel && meta.channels){
      for(i=0;i<meta.channels.length;i++){
        if(meta.channels[i] === p.channel){
          if(chSel) chSel.value = p.channel;
          p.channel = null;
          changed = true;
          break;
        }
      }
    }

    if(changed){ dirtyBp = true; scheduleRender(); }
  }

  // ---------- Meta handling ----------

  function applyMeta(meta){
    if(!meta || typeof meta !== 'object') return;
    updateFileStatus(meta);
    updateRunMetaStatus(meta);

    if(meta.bandpower && meta.bandpower.bands && meta.bandpower.channels){
      var bp = meta.bandpower;
      var prev = state.bp.meta;
      var prevBands = prev && prev.bands ? prev.bands.join('|') : '';
      var prevCh = prev && prev.channels ? prev.channels.join('|') : '';
      var newBands = bp.bands.join('|');
      var newCh = bp.channels.join('|');

      state.bp.meta = {
        bands: bp.bands,
        channels: bp.channels,
        positions: bp.positions,
        positions_source: bp.positions_source,
        fallback_positions_count: bp.fallback_positions_count
      };

      if(bandSel && (prevBands !== newBands)){
        bandSel.innerHTML = '';
        for(var i=0;i<bp.bands.length;i++){
          var opt = document.createElement('option');
          opt.value = bp.bands[i];
          opt.textContent = bp.bands[i];
          bandSel.appendChild(opt);
        }
      }

      if(chSel && (prevCh !== newCh)){
        chSel.innerHTML = '';
        for(i=0;i<bp.channels.length;i++){
          opt = document.createElement('option');
          opt.value = bp.channels[i];
          opt.textContent = bp.channels[i];
          chSel.appendChild(opt);
        }
      }

      tryApplyPendingSelection();
    }
  }

  // ---------- Message handlers ----------

  function handleStateMsg(raw){
    try {
      var un = unpackMsg(raw);
      for(var i=0;i<un.frames.length;i++){
        var st = un.frames[i];
        if(!st || typeof st !== 'object') continue;
        var cid = st.client_id ? String(st.client_id) : '';
        if(cid && cid === CLIENT_ID) continue;
        applyUiState(st);
      }
      dirtyNf = true;
      dirtyBp = true;
      scheduleRender();
      setBadge(stateConn, 'state: live', 'good');
    } catch(e){}
  }

  function handleMetaMsg(raw){
    try {
      var un = unpackMsg(raw);
      var dirty = false;
      for(var i=0;i<un.frames.length;i++){
        applyMeta(un.frames[i]);
        dirty = true;
      }
      if(dirty){ dirtyBp = true; scheduleRender(); }
      setBadge(metaConn, 'meta: live', 'good');
    } catch(e){}
  }

  function handleNfMsg(raw){
    if(state.paused) return;
    try {
      var un = unpackMsg(raw);
      if(un.reset){ state.nf.frames = []; state.nf.lastT = -Infinity; }
      for(var i=0;i<un.frames.length;i++){
        var f = un.frames[i];
        if(!f || typeof f !== 'object') continue;
        if(f.t !== null && f.t !== undefined && isFinite(f.t)){
          if(f.t <= state.nf.lastT) continue;
          state.nf.lastT = f.t;
        }
        state.nf.frames.push(f);
        if(f.artifact !== undefined){ state.art.latest = {bad: !!f.artifact, bad_channels: f.bad_channels || 0}; }
      }
      if(state.nf.frames.length > 20000){ state.nf.frames.splice(0, state.nf.frames.length-20000); }
      updateStats();
      dirtyNf = true;
      scheduleRender();
      setBadge(nfConn, 'nf: live', 'good');
    } catch(e){}
  }

  function handleBandpowerMsg(raw){
    if(state.paused) return;
    try {
      var un = unpackMsg(raw);
      if(un.reset){ state.bp.frames = []; state.bp.lastT = -Infinity; state.bp.rollingMin = null; state.bp.rollingMax = null; }
      for(var i=0;i<un.frames.length;i++){
        var f = un.frames[i];
        if(!f || typeof f !== 'object') continue;
        if(f.t !== null && f.t !== undefined && isFinite(f.t)){
          if(f.t <= state.bp.lastT) continue;
          state.bp.lastT = f.t;
        }
        state.bp.frames.push(f);
      }
      if(state.bp.frames.length > 20000){ state.bp.frames.splice(0, state.bp.frames.length-20000); }
      dirtyBp = true;
      scheduleRender();
      setBadge(bpConn, 'bandpower: live', 'good');
    } catch(e){}
  }

  function handleArtifactMsg(raw){
    if(state.paused) return;
    try {
      var un = unpackMsg(raw);
      if(un.reset){ state.art.frames = []; state.art.lastT = -Infinity; state.art.latest = null; }
      for(var i=0;i<un.frames.length;i++){
        var f = un.frames[i];
        if(!f || typeof f !== 'object') continue;
        if(f.t !== null && f.t !== undefined && isFinite(f.t)){
          if(f.t <= state.art.lastT) continue;
          state.art.lastT = f.t;
        }
        state.art.frames.push(f);
        state.art.latest = {bad: !!f.bad, bad_channels: f.bad_channels || 0};
      }
      if(state.art.frames.length > 40000){ state.art.frames.splice(0, state.art.frames.length-40000); }
      updateStats();
      setBadge(artConn, 'artifact: live', 'good');
    } catch(e){}
  }

  // ---------- Streaming setup ----------

  function connectStream(topics){
    if(!window.EventSource) return null;
    var t = (topics && topics.join) ? topics.join(',') : String(topics || '');
    var url = '/api/sse/stream?token=' + encodeURIComponent(TOKEN);
    if(t){ url += '&topics=' + encodeURIComponent(t); }
    return new EventSource(url);
  }

  function connectSSE(path, badgeEl, label){
    if(!window.EventSource){
      setBadge(badgeEl, label + ': no EventSource', 'warn');
      return null;
    }
    var url = path + '?token=' + encodeURIComponent(TOKEN);
    var es = new EventSource(url);
    setBadge(badgeEl, label + ': connecting', 'warn');
    es.onopen = function(){ setBadge(badgeEl, label + ': connected', 'good'); };
    es.onerror = function(){ setBadge(badgeEl, label + ': reconnecting', 'warn'); };
    return es;
  }

  function startStream(){
    var es = connectStream(['config','meta','state','nf','artifact','bandpower']);
    if(!es){ startPolling(); return; }

    setBadge(nfConn, 'nf: connecting', 'warn');
    setBadge(bpConn, 'bandpower: connecting', 'warn');
    setBadge(artConn, 'artifact: connecting', 'warn');
    setBadge(metaConn, 'meta: connecting', 'warn');
    setBadge(stateConn, 'state: connecting', 'warn');

    es.onopen = function(){
      // wait for data
    };

    es.onerror = function(){
      setBadge(nfConn, 'nf: reconnecting', 'warn');
      setBadge(bpConn, 'bandpower: reconnecting', 'warn');
      setBadge(artConn, 'artifact: reconnecting', 'warn');
      setBadge(metaConn, 'meta: reconnecting', 'warn');
      setBadge(stateConn, 'state: reconnecting', 'warn');
    };

    if(es.addEventListener){
      es.addEventListener('state', function(ev){ try{ handleStateMsg(JSON.parse(ev.data)); }catch(e){} });
      es.addEventListener('meta', function(ev){ try{ handleMetaMsg(JSON.parse(ev.data)); }catch(e){} });
      es.addEventListener('nf', function(ev){ try{ handleNfMsg(JSON.parse(ev.data)); }catch(e){} });
      es.addEventListener('bandpower', function(ev){ try{ handleBandpowerMsg(JSON.parse(ev.data)); }catch(e){} });
      es.addEventListener('artifact', function(ev){ try{ handleArtifactMsg(JSON.parse(ev.data)); }catch(e){} });
    } else {
      // Extremely old EventSource: fallback to legacy endpoints.
      startLegacy();
    }
  }

  function startLegacy(){
    if(!window.EventSource){ startPolling(); return; }

    var stateSse = connectSSE('/api/sse/state', stateConn, 'state');
    if(stateSse){ stateSse.onmessage = function(ev){ try{ handleStateMsg(JSON.parse(ev.data)); }catch(e){} }; }

    var metaSse = connectSSE('/api/sse/meta', metaConn, 'meta');
    if(metaSse){ metaSse.onmessage = function(ev){ try{ handleMetaMsg(JSON.parse(ev.data)); }catch(e){} }; }

    var nf = connectSSE('/api/sse/nf', nfConn, 'nf');
    if(nf){ nf.onmessage = function(ev){ try{ handleNfMsg(JSON.parse(ev.data)); }catch(e){} }; }

    var bp = connectSSE('/api/sse/bandpower', bpConn, 'bandpower');
    if(bp){ bp.onmessage = function(ev){ try{ handleBandpowerMsg(JSON.parse(ev.data)); }catch(e){} }; }

    var art = connectSSE('/api/sse/artifact', artConn, 'artifact');
    if(art){ art.onmessage = function(ev){ try{ handleArtifactMsg(JSON.parse(ev.data)); }catch(e){} }; }
  }

  function startPolling(){
    if(!TOKEN) return;

    setBadge(nfConn, 'nf: polling', 'warn');
    setBadge(bpConn, 'bandpower: polling', 'warn');
    setBadge(artConn, 'artifact: polling', 'warn');
    setBadge(metaConn, 'meta: polling', 'warn');
    setBadge(stateConn, 'state: polling', 'warn');

    var topics = ['config','meta','state','nf','artifact','bandpower'];
    var cur = {nf:0, bandpower:0, artifact:0, meta:0, state:0};
    var backoff = 0;

    function buildUrl(){
      var qp = [];
      qp.push('token=' + encodeURIComponent(TOKEN));
      qp.push('topics=' + encodeURIComponent(topics.join(',')));
      qp.push('wait=1.0');
      qp.push('limit=2500');
      qp.push('nf=' + encodeURIComponent(String(cur.nf||0)));
      qp.push('bandpower=' + encodeURIComponent(String(cur.bandpower||0)));
      qp.push('artifact=' + encodeURIComponent(String(cur.artifact||0)));
      qp.push('meta=' + encodeURIComponent(String(cur.meta||0)));
      qp.push('state=' + encodeURIComponent(String(cur.state||0)));
      return '/api/snapshot?' + qp.join('&');
    }

    function applyTopic(name, payload, handler){
      if(!payload || typeof payload !== 'object') return;
      if(typeof payload.cursor === 'number' && isFinite(payload.cursor)){
        cur[name] = payload.cursor;
      }
      if(payload.batch){ handler(payload.batch); }
    }

    function pollOnce(){
      xhrJson('GET', buildUrl(), null, function(err, resp){
        if(!err && resp){
          applyTopic('meta', resp.meta, handleMetaMsg);
          applyTopic('state', resp.state, handleStateMsg);
          applyTopic('nf', resp.nf, handleNfMsg);
          applyTopic('artifact', resp.artifact, handleArtifactMsg);
          applyTopic('bandpower', resp.bandpower, handleBandpowerMsg);
          backoff = 0;
        } else {
          backoff = Math.min(10000, backoff ? Math.round(backoff*1.5) : 500);
        }
        var delay = backoff || 50;
        setTimeout(pollOnce, delay);
      });
    }

    pollOnce();
  }

  // ---------- UI bindings ----------

  function ensureUiBindings(){
    if(pauseBtn){
      pauseBtn.onclick = function(){
        state.paused = !state.paused;
        pauseBtn.textContent = state.paused ? 'Resume' : 'Pause';
        schedulePushUiState();
        dirtyNf = true;
        dirtyBp = true;
        scheduleRender();
      };
    }

    if(winSel){
      winSel.onchange = function(){
        state.winSec = parseFloat(winSel.value || '60') || 60;
        schedulePushUiState();
        dirtyNf = true;
        scheduleRender();
      };
    }

    if(bandSel){ bandSel.onchange = function(){ dirtyBp = true; scheduleRender(); schedulePushUiState(); }; }
    if(chSel){ chSel.onchange = function(){ dirtyBp = true; scheduleRender(); schedulePushUiState(); }; }
    if(lblSel){ lblSel.onchange = function(){ dirtyBp = true; scheduleRender(); schedulePushUiState(); }; }
    if(xformSel){ xformSel.onchange = function(){ state.bp.rollingMin=null; state.bp.rollingMax=null; dirtyBp = true; scheduleRender(); schedulePushUiState(); }; }
    if(scaleSel){ scaleSel.onchange = function(){ state.bp.rollingMin=null; state.bp.rollingMax=null; dirtyBp = true; scheduleRender(); schedulePushUiState(); }; }

    // Click-to-select channel on topography
    topoCanvas.addEventListener('click', function(ev){
      var meta = state.bp.meta;
      if(!meta || !meta.channels || !meta.positions) return;

      var rect = topoCanvas.getBoundingClientRect();
      var dpr = window.devicePixelRatio || 1;
      var x = (ev.clientX - rect.left) * dpr;
      var y = (ev.clientY - rect.top) * dpr;

      var pts = state.bp.electrodesPx || [];
      var best = -1;
      var bestD = 1e30;
      for(var i=0;i<pts.length;i++){
        var p = pts[i];
        var dx = x - p[0];
        var dy = y - p[1];
        var d2 = dx*dx + dy*dy;
        if(d2 < bestD){ bestD = d2; best = p[2]; }
      }
      var thresh = (32*dpr) * (32*dpr);
      if(best >= 0 && bestD <= thresh){
        var name = meta.channels[best];
        if(name && chSel){
          chSel.value = name;
          dirtyBp = true;
          scheduleRender();
          schedulePushUiState();
        }
      }
    });
  }

  // ---------- Startup ----------

  // ------------------------ run meta + stats (legacy) ------------------------

  // ------------------------ files/downloads (legacy) ------------------------

  var filesLastLoad_utc = 0;

  function clearEl(el){
    if(!el) return;
    while(el.firstChild) el.removeChild(el.firstChild);
  }

  function renderFilesList(obj){
    if(!filesList) return;
    clearEl(filesList);

    if(bundleDownload && TOKEN){
      bundleDownload.href = '/api/bundle?token=' + encodeURIComponent(TOKEN);
    }

    var files = (obj && obj.files && obj.files.length) ? obj.files : [];
    var now = nowSec();

    if(filesHint){
      filesHint.textContent = (files.length === 0) ? 'No downloadable files found in outdir.' : '';
    }

    for(var i=0;i<files.length;i++){
      var f = files[i];
      if(!f || !f.name) continue;
      var name = String(f.name);

      var row = document.createElement('div');
      row.className = 'fileRow';

      var a = document.createElement('a');
      a.textContent = name;
      a.target = '_blank';
      a.rel = 'noopener';

      var baseUrl = (f.download_url || f.url) ? String(f.download_url || f.url) : ('/api/file?name=' + encodeURIComponent(name) + '&download=1');
      var href = baseUrl + (baseUrl.indexOf('?')>=0 ? '&' : '?') + 'token=' + encodeURIComponent(TOKEN);
      var downloadable = (typeof f.downloadable === 'boolean') ? f.downloadable : true;

      if(downloadable){
        a.href = href;
      } else {
        a.href = '#';
        a.style.opacity = '0.6';
        a.onclick = function(ev){ try{ if(ev && ev.preventDefault) ev.preventDefault(); }catch(e){} return false; };
      }
      row.appendChild(a);

      var meta = document.createElement('span');
      meta.className = 'fileMeta';
      var st = (f.stat && typeof f.stat === 'object') ? f.stat : {};
      var size = (st && isFinite(st.size_bytes)) ? st.size_bytes : null;
      var mtime = (st && isFinite(st.mtime_utc)) ? st.mtime_utc : null;
      var parts = [];
      if(size !== null) parts.push(fmtBytes(size));
      if(mtime !== null) parts.push('age ' + fmtAgeSec(now - mtime));
      if(f.category) parts.push(String(f.category));
      if(downloadable === false) parts.push('too large');
      meta.textContent = parts.join(' · ');
      row.appendChild(meta);

      filesList.appendChild(row);
    }
  }

  function loadFiles(force){
    if(!TOKEN) return;
    if(!filesList) return;
    var now = nowSec();
    if(!force && (now - filesLastLoad_utc) < 1.0 && filesList.childNodes && filesList.childNodes.length){
      return;
    }
    filesLastLoad_utc = now;
    if(filesHint) filesHint.textContent = 'Loading…';
    xhrJson('GET', '/api/files?token=' + encodeURIComponent(TOKEN), null, function(err, obj){
      if(err){
        if(filesHint) filesHint.textContent = 'Failed to load file list.';
        return;
      }
      renderFilesList(obj);
    });
  }

  function loadRunMeta(force){
    if(!TOKEN) return;
    if(!runMetaRaw && !runMetaKv && !runMetaStatus) return;
    var url = '/api/run_meta?token=' + encodeURIComponent(TOKEN);
    var headers = {};
    if(!force && runMetaETag) headers['If-None-Match'] = runMetaETag;
    xhrJson('GET', url, null, function(err, data, xhr){
      if(err){
        if(runMetaStatus) runMetaStatus.textContent = 'run meta: fetch failed';
        return;
      }
      if(xhr){
        var et = xhr.getResponseHeader ? xhr.getResponseHeader('ETag') : null;
        if(et) runMetaETag = et;
      }
      if(xhr && xhr.status === 304){
        return;
      }
      if(data && data.etag && !runMetaETag) runMetaETag = data.etag;
      if(data && data.data && typeof data.data === 'object'){
        var order = ['Tool','Version','GitDescribe','TimestampLocal','OutputDir','protocol','metric_spec','band_spec','reward_direction','fs_hz','window_seconds','update_seconds','baseline_seconds','target_reward_rate','artifact_gate','qc_bad_channel_count','qc_bad_channels','biotrace_ui','export_derived_events','derived_events_written'];
        renderKvGrid(runMetaKv, data.data, order);
        if(runMetaRaw) runMetaRaw.textContent = JSON.stringify(data.data, null, 2);
      } else {
        renderKvGrid(runMetaKv, null);
        if(runMetaRaw) runMetaRaw.textContent = (data && data.parse_error) ? ('Parse error: ' + data.parse_error) : '';
      }
      updateRunMetaStatus({run_meta: data});
    }, headers);
  }

  function loadStatsOnce(){
    if(!TOKEN) return;
    if(!serverStats && !statsKv) return;
    var url = '/api/stats?token=' + encodeURIComponent(TOKEN);
    xhrJson('GET', url, null, function(err, data){
      if(err){
        if(serverStats) serverStats.textContent = 'stats: fetch failed';
        return;
      }
      var up = (data && typeof data.uptime_sec === 'number') ? data.uptime_sec : null;
      var c = data && data.connections ? data.connections : {};
      if(serverStats){
        var viewers = (c && typeof c.stream === 'number') ? c.stream : null;
        var upStr = (up === null) ? '—' : fmtAgeSec(up);
        var vStr = (viewers === null) ? '—' : String(viewers);
        serverStats.textContent = 'uptime: ' + upStr + ', viewers: ' + vStr;
        if(serverStats.classList) serverStats.classList.remove('muted');
      }

      var flat = {};
      if(data){
        flat.frontend = data.frontend || '';
        if(up !== null) flat.uptime_sec = up.toFixed(1);
        if(data.server_instance_id) flat.server_instance_id = data.server_instance_id;
        var connKeys = ['stream','nf','bandpower','artifact','meta','state'];
        for(var i=0;i<connKeys.length;i++){
          var k = connKeys[i];
          if(c && typeof c[k] === 'number') flat['conn_' + k] = c[k];
        }
        var bufs = data.buffers || {};
        var bufKeys = ['nf','bandpower','artifact','meta','state'];
        for(var j=0;j<bufKeys.length;j++){
          var bk = bufKeys[j];
          if(bufs && bufs[bk]){
            flat['buf_' + bk + '_size'] = bufs[bk].size;
            flat['buf_' + bk + '_latest'] = bufs[bk].latest_seq;
          }
        }
      }
      renderKvGrid(statsKv, flat, Object.keys(flat));
    });
  }

  function startStatsPolling(){
    if(statsTimer) return;
    statsTimer = setInterval(loadStatsOnce, 2000);
    loadStatsOnce();
  }



  function start(){
    if(!TOKEN){
      showStatus('Missing token. Re-open using the printed URL (it includes <code>?token=…</code>).');
      return;
    }

    // Wire up Session panel controls (safe no-ops if elements missing).
    if(runMetaDownload){
      runMetaDownload.href = '/api/run_meta?token=' + encodeURIComponent(TOKEN) + '&format=raw';
    }
    if(runMetaRefresh){
      runMetaRefresh.onclick = function(){ loadRunMeta(true); };
    }
    if(runMetaDetails && runMetaDetails.addEventListener){
      runMetaDetails.addEventListener('toggle', function(){
        if(runMetaDetails.open) loadRunMeta(false);
      });
    }

    if(bundleDownload){
      bundleDownload.href = '/api/bundle?token=' + encodeURIComponent(TOKEN);
    }
    if(filesRefresh){
      filesRefresh.onclick = function(){ loadFiles(true); };
    }
    if(downloadsDetails && downloadsDetails.addEventListener){
      downloadsDetails.addEventListener('toggle', function(){
        if(downloadsDetails.open) loadFiles(false);
      });
    }
    // Prefetch once so the list is ready when the user opens the panel.
    loadFiles(false);

    ensureUiBindings();

    // Load state best-effort
    xhrJson('GET', '/api/state?token=' + encodeURIComponent(TOKEN), null, function(err, st){
      if(!err && st){
        applyUiState(st);
        dirtyNf = true;
        dirtyBp = true;
        scheduleRender();
      }
    });

    // Load meta once
    xhrJson('GET', '/api/meta?token=' + encodeURIComponent(TOKEN), null, function(err, meta){
      if(err || !meta){
        showStatus('Failed to load metadata from the server. Is the process still running?');
        setBadge(nfConn, 'nf: offline', 'bad');
        setBadge(bpConn, 'bandpower: offline', 'bad');
        setBadge(artConn, 'artifact: offline', 'bad');
        setBadge(metaConn, 'meta: offline', 'bad');
        setBadge(stateConn, 'state: offline', 'bad');
        return;
      }

      applyMeta(meta);
      dirtyBp = true;
      dirtyNf = true;
      scheduleRender();

      if(!window.EventSource){
        startPolling();
        return;
      }

      // Prefer multiplexed stream if supported
      xhrJson('GET', '/api/config?token=' + encodeURIComponent(TOKEN), null, function(_e, cfg){
        var supports = cfg && cfg.supports ? cfg.supports : {};
        if(supports && supports.stats){
          startStatsPolling();
        }
        if(supports && supports.sse_stream){
          startStream();
        } else {
          startLegacy();
        }
      });
    });
  }

  start();
})();
