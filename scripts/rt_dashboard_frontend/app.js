(() => {
  'use strict';

  const TOKEN = (window.location.search.match(/(?:\?|&)token=([^&]+)/) || [])[1] || '';
  const qs = (id) => document.getElementById(id);

  const statusBox = qs('status');
  function showStatus(msg){ statusBox.style.display='block'; statusBox.innerHTML = msg; }
  function hideStatus(){ statusBox.style.display='none'; statusBox.innerHTML = ''; }

  function setBadge(el, txt, cls){ el.textContent = txt; el.className = 'badge' + (cls?(' '+cls):''); }

  const nfConn = qs('nfConn');
  const bpConn = qs('bpConn');
  const artConn = qs('artConn');
  const metaConn = qs('metaConn');
  const stateConn = qs('stateConn');

  const runMetaStatus = qs('runMetaStatus');
  const serverStats = qs('serverStats');
  const runMetaDetails = qs('runMetaDetails');
  const runMetaKv = qs('runMetaKv');
  const runMetaRaw = qs('runMetaRaw');
  const runMetaDownload = qs('runMetaDownload');
  const runMetaRefresh = qs('runMetaRefresh');
  const statsKv = qs('statsKv');

  const winSel = qs('winSel');
  const pauseBtn = qs('pauseBtn');

  const nfStats = qs('nfStats');
  const canvas = qs('nfChart');
  const ctx = canvas.getContext('2d');

  const bandSel = qs('bandSel');
  const chSel = qs('chSel');
  const xformSel = qs('xformSel');
  const mapSel = qs('mapSel');
  const nfModeSel = qs('nfModeSel');
  const scaleSel = qs('scaleSel');
  const lblSel = qs('lblSel');
  const vminEl = qs('vmin');
  const vmaxEl = qs('vmax');

  const topoCanvas = qs('topoCanvas');
  const topoCtx = topoCanvas.getContext('2d');
  const bpCanvas = qs('bpChart');
  const bpCtx = bpCanvas.getContext('2d');

  const CLIENT_ID_KEY = 'qeeg_rt_dashboard_client_id';
  function newClientId(){
    return (Math.random().toString(36).slice(2) + Date.now().toString(36)).slice(0, 32);
  }
  const CLIENT_ID = (() => {
    try{
      const existing = localStorage.getItem(CLIENT_ID_KEY);
      if(existing && existing.length >= 6) return existing;
      const id = newClientId();
      localStorage.setItem(CLIENT_ID_KEY, id);
      return id;
    }catch(e){
      return newClientId();
    }
  })();

  const state = {
    paused: false,
    winSec: 60,
    pendingUi: null,
    applyingRemote: false,
    nf: {frames: [], lastT: -Infinity},
    bp: {meta: null, frames: [], lastT: -Infinity, rollingMin: null, rollingMax: null, tmpCanvas: null, tmpW: 0, tmpH: 0, electrodesPx: [], baseline: null, reference: null, baselineSec: 10.0},
    art: {frames: [], latest: null, lastT: -Infinity},
  };

  let runMetaETag = null;
  let statsTimer = null;

  function fmt(x){
    if(x===null || x===undefined || !Number.isFinite(x)) return '—';
    if(Math.abs(x) >= 100) return x.toFixed(1);
    if(Math.abs(x) >= 10) return x.toFixed(2);
    return x.toFixed(3);
  }

  function fmtAgeSec(age){
    if(age===null || age===undefined || !Number.isFinite(age)) return '—';
    if(age < 1.0) return '<1s';
    if(age < 60) return `${Math.round(age)}s`;
    const m = Math.floor(age/60); const s = Math.round(age - 60*m);
    return `${m}m ${s}s`;
  }

  function escHtml(s){
    return String(s).replace(/[&<>"']/g, (ch)=>({
      '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
    }[ch] || ch));
  }

  function renderKvGrid(el, obj, order){
    if(!el) return;
    if(!obj || typeof obj !== 'object'){
      el.innerHTML = '<span class="muted">—</span>';
      return;
    }
    const keys = Array.isArray(order) ? order : Object.keys(obj);
    const parts = [];
    for(const k of keys){
      if(!(k in obj)) continue;
      let v = obj[k];
      if(v === null || v === undefined) v = '';
      if(typeof v === 'object') v = JSON.stringify(v);
      parts.push(`<span>${escHtml(k)}</span><b>${escHtml(v)}</b>`);
    }
    el.innerHTML = parts.join('') || '<span class="muted">—</span>';
  }

  function updateFileStatus(meta){
    const fs = (meta && meta.files_stat) ? meta.files_stat : null;
    if(!fs){ return; }
    const now = Date.now() / 1000;
    const rows = [];
    let anyMissing = false;
    let anyStale = false;
    const add = (label, st) => {
      const ok = !!(st && st.exists);
      if(!ok) anyMissing = true;
      const age = (ok && st.mtime_utc) ? (now - st.mtime_utc) : null;
      if(ok && age !== null && age > 5) anyStale = true;
      const cls = ok ? (age !== null && age > 5 ? 'warn' : 'good') : 'bad';
      rows.push(`<div class="row" style="justify-content:space-between; margin:3px 0">
        <span><b>${label}</b> <span class="badge ${cls}" style="margin-left:8px">${ok?'ok':'missing'}</span></span>
        <span class="small">age: ${fmtAgeSec(age)} &nbsp; size: ${ok ? (st.size_bytes||0) : '—'}</span>
      </div>`);
    };
    add('nf_feedback.csv', fs.nf_feedback);
    add('bandpower_timeseries.csv', fs.bandpower_timeseries);
    add('artifact_gate_timeseries.csv', fs.artifact_gate_timeseries);

    if(anyMissing){
      showStatus(`<div style="margin-bottom:6px"><b>Waiting on outputs…</b> (start qeeg_nf_cli, and consider <code>--flush-csv</code> for live updates)</div>${rows.join('')}`);
    } else if(anyStale){
      showStatus(`<div style="margin-bottom:6px"><b>Outputs look stale</b> (no new writes in &gt;5s). Is the session paused or finished?</div>${rows.join('')}`);
    } else {
      hideStatus();
    }
  }

  function updateRunMetaStatus(meta){
    if(!runMetaStatus) return;
    const rm = meta && meta.run_meta ? meta.run_meta : null;
    const st = rm && rm.stat ? rm.stat : (meta && meta.files_stat ? meta.files_stat.nf_run_meta : null);
    if(st && st.exists){
      const now = Date.now()/1000;
      const age = (st.mtime_utc) ? (now - st.mtime_utc) : null;
      runMetaStatus.textContent = `present (age: ${fmtAgeSec(age)})`;
      runMetaStatus.classList.remove('muted');
      if(rm && rm.summary){
        const order = [
          'Tool','Version','GitDescribe','TimestampLocal','protocol','metric_spec','band_spec','reward_direction',
          'fs_hz','window_seconds','update_seconds','baseline_seconds','target_reward_rate','artifact_gate',
          'qc_bad_channel_count','qc_bad_channels','biotrace_ui','export_derived_events'
        ];
        renderKvGrid(runMetaKv, rm.summary, order);
        // Cache baseline duration for client-side z-scoring.
        const bs = rm.summary.baseline_seconds;
        if(typeof bs === 'number' && Number.isFinite(bs) && bs > 0){ state.bp.baselineSec = bs; }
      }
      if(rm && rm.parse_error){
        if(runMetaRaw) runMetaRaw.textContent = `Parse error: ${rm.parse_error}`;
      }
    } else {
      runMetaStatus.textContent = 'missing';
      runMetaStatus.classList.add('muted');
      renderKvGrid(runMetaKv, null);
      if(runMetaRaw) runMetaRaw.textContent = '';
    }
    if(runMetaDownload && TOKEN){
      runMetaDownload.href = `/api/run_meta?token=${encodeURIComponent(TOKEN)}&format=raw`;
    }
  }

  function xformValue(v){
    if(v===null || v===undefined || !Number.isFinite(v)) return null;
    const mode = (xformSel && xformSel.value) ? xformSel.value : 'linear';
    if(mode === 'linear') return v;
    const eps = 1e-12;
    if(v <= eps) return null;
    if(mode === 'log10') return Math.log10(v);
    if(mode === 'db') return 10.0 * Math.log10(v);
    return v;
  }

  function nfMode(){
    return (nfModeSel && nfModeSel.value) ? nfModeSel.value : 'raw';
  }

  function nfPick(f){
    const mode = nfMode();
    const raw = {metric: f.metric, threshold: f.threshold, label: 'raw'};
    if(mode === 'zbase'){
      const mz = (f.metric_z !== undefined) ? f.metric_z : null;
      const tz = (f.threshold_z !== undefined) ? f.threshold_z : null;
      if(mz !== null || tz !== null) return {metric: mz, threshold: tz, label: 'z (baseline)'};
    }
    if(mode === 'zref'){
      const mz = (f.metric_z_ref !== undefined) ? f.metric_z_ref : null;
      const tz = (f.threshold_z_ref !== undefined) ? f.threshold_z_ref : null;
      if(mz !== null || tz !== null) return {metric: mz, threshold: tz, label: 'z (reference)'};
    }
    return raw;
  }

  function mapMode(){
    return (mapSel && mapSel.value) ? mapSel.value : 'raw';
  }

  function ensureBaselineStats(nCols){
    if(state.bp.baseline && state.bp.baseline.n && state.bp.baseline.n.length === nCols) return;
    state.bp.baseline = {n: new Array(nCols).fill(0), mean: new Array(nCols).fill(0), m2: new Array(nCols).fill(0)};
  }

  function baselineUpdate(vals){
    if(!Array.isArray(vals)) return;
    ensureBaselineStats(vals.length);
    const b = state.bp.baseline;
    for(let i=0;i<vals.length;i++){
      const x = vals[i];
      if(x === null || x === undefined || !Number.isFinite(x)) continue;
      const n1 = b.n[i] + 1;
      b.n[i] = n1;
      const delta = x - b.mean[i];
      b.mean[i] += delta / n1;
      const delta2 = x - b.mean[i];
      b.m2[i] += delta * delta2;
    }
  }

  function baselineZ(i, x){
    const b = state.bp.baseline;
    if(!b || !b.n || i<0 || i>=b.n.length) return null;
    const n = b.n[i];
    if(n < 5) return null;
    const var_ = b.m2[i] / Math.max(1, (n - 1));
    const sd = Math.sqrt(var_);
    if(!(sd > 0) || !Number.isFinite(sd)) return null;
    return (x - b.mean[i]) / sd;
  }

  function referenceZ(i, x){
    const ref = state.bp.reference;
    if(!ref || !ref.stdevs || !ref.means) return null;
    if(i<0 || i>=ref.means.length) return null;
    const m = ref.means[i];
    const sd = ref.stdevs[i];
    if(m === null || m === undefined || sd === null || sd === undefined) return null;
    if(!Number.isFinite(m) || !Number.isFinite(sd) || !(sd > 0)) return null;
    return (x - m) / sd;
  }

  function bpDisplayValue(colIdx, rawVal){
    const mode = mapMode();
    if(rawVal === null || rawVal === undefined || !Number.isFinite(rawVal)) return null;
    if(mode === 'raw') return xformValue(rawVal);
    if(mode === 'zbase') return baselineZ(colIdx, rawVal);
    if(mode === 'zref') return referenceZ(colIdx, rawVal);
    return xformValue(rawVal);
  }

  async function loadReference(){
    if(!TOKEN) return;
    try{
      const r = await fetch(`/api/reference?token=${encodeURIComponent(TOKEN)}`);
      if(!r.ok) return;
      const obj = await r.json();
      if(obj && obj.aligned){
        state.bp.reference = {
          bands: obj.aligned.bands,
          channels: obj.aligned.channels,
          means: obj.aligned.means,
          stdevs: obj.aligned.stdevs,
          present: obj.aligned.present,
          meta: obj.meta || null,
        };
        dirtyBp = true;
        scheduleRender();
      }
    }catch(e){}
  }

  function updateStats(){
    const f = state.nf.frames.length ? state.nf.frames[state.nf.frames.length-1] : null;
    const a = state.art.latest;
    const parts = [];
    const add = (k,v) => parts.push(`<span>${k}</span><b>${v}</b>`);
    if(f){
      add('t (s)', fmt(f.t));
      const picked = nfPick(f);
      add('nf_mode', picked.label);
      add('metric', fmt(picked.metric));
      add('threshold', fmt(picked.threshold));
      add('reward', (f.reward? '1':'0'));
      add('reward_rate', fmt(f.reward_rate));
      if(f.artifact_ready!==null && f.artifact_ready!==undefined) add('artifact_ready', String(f.artifact_ready));
      if(f.phase) add('phase', String(f.phase));
      if(f.bad_channels!==null && f.bad_channels!==undefined) add('bad_ch', String(f.bad_channels));
    } else {
      add('status', 'waiting for nf_feedback.csv');
    }
    if(a){
      add('artifact', a.bad? '1':'0');
      add('artifact_bad_ch', String(a.bad_channels||0));
      if(a.ready!==null && a.ready!==undefined) add('artifact_ready', String(a.ready));
    }
    nfStats.innerHTML = parts.join('');
  }

  function resizeCanvas(){
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    const w = Math.max(300, Math.floor(rect.width * dpr));
    const h = Math.max(180, Math.floor(rect.height * dpr));
    if(canvas.width !== w || canvas.height !== h){
      canvas.width = w; canvas.height = h;
    }
  }

  function resizeCanvasTo(el){
    const dpr = window.devicePixelRatio || 1;
    const rect = el.getBoundingClientRect();
    const w = Math.max(320, Math.floor(rect.width * dpr));
    const h = Math.max(200, Math.floor(rect.height * dpr));
    if(el.width !== w || el.height !== h){
      el.width = w; el.height = h;
      return true;
    }
    return false;
  }

  function drawChart(){
    resizeCanvas();
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0,0,w,h);

    const frames = state.nf.frames;
    if(!frames.length){
      ctx.fillStyle = '#9fb0c3';
      ctx.font = `${Math.round(12*(window.devicePixelRatio||1))}px system-ui`;
      ctx.fillText('Waiting for nf_feedback.csv …', 14*(window.devicePixelRatio||1), 24*(window.devicePixelRatio||1));
      return;
    }

    const tNow = frames[frames.length-1].t;
    const tMin = Math.max(frames[0].t, tNow - state.winSec);

    const vis = [];
    for(let i=0;i<frames.length;i++){
      const f = frames[i];
      if(f.t >= tMin) vis.push(f);
    }
    if(vis.length<2) return;

    let yMin = Infinity, yMax = -Infinity;
    for(const f of vis){
      const pv = nfPick(f);
      if(Number.isFinite(pv.metric)){ yMin = Math.min(yMin, pv.metric); yMax = Math.max(yMax, pv.metric); }
      if(Number.isFinite(pv.threshold)){ yMin = Math.min(yMin, pv.threshold); yMax = Math.max(yMax, pv.threshold); }
    }
    if(!(yMax>yMin)) { yMax = yMin + 1; }
    const pad = 0.05*(yMax-yMin);
    yMin -= pad; yMax += pad;

    const x = (t) => (t - tMin) / (tNow - tMin) * (w-20) + 10;
    const y = (v) => (h-20) - (v - yMin)/(yMax-yMin) * (h-30);

    // Grid
    ctx.strokeStyle = 'rgba(255,255,255,0.06)';
    ctx.lineWidth = 1;
    for(let i=0;i<=4;i++){
      const yy = 10 + i*(h-30)/4;
      ctx.beginPath(); ctx.moveTo(10,yy); ctx.lineTo(w-10,yy); ctx.stroke();
    }

    // Reward shading
    for(let i=0;i<vis.length-1;i++){
      if(vis[i].reward){
        const x0 = x(vis[i].t);
        const x1 = x(vis[i+1].t);
        ctx.fillStyle = 'rgba(100, 210, 255, 0.08)';
        ctx.fillRect(x0, 10, Math.max(1, x1-x0), h-30);
      }
    }

    // Artifact shading (prefer artifact_gate_timeseries if present; fall back to nf frames)
    const art = state.art.frames;
    if(art && art.length >= 2){
      let startIdx = 0;
      while(startIdx+1 < art.length && art[startIdx+1].t < tMin) startIdx++;
      for(let i=startIdx; i<art.length-1; i++){
        const a0 = art[i], a1 = art[i+1];
        if(a0.t > tNow) break;
        if(a0.bad){
          const x0 = x(Math.max(a0.t, tMin));
          const x1 = x(Math.min(a1.t, tNow));
          if(x1 > x0){
            ctx.fillStyle = 'rgba(255, 107, 107, 0.10)';
            ctx.fillRect(x0, 10, Math.max(1, x1-x0), h-30);
          }
        }
      }
      const last = art[art.length-1];
      if(last && last.t <= tNow && last.bad){
        const x0 = x(Math.max(last.t, tMin));
        const x1 = x(tNow);
        if(x1 > x0){
          ctx.fillStyle = 'rgba(255, 107, 107, 0.10)';
          ctx.fillRect(x0, 10, Math.max(1, x1-x0), h-30);
        }
      }
    } else {
      for(let i=0;i<vis.length-1;i++){
        if(vis[i].artifact){
          const x0 = x(vis[i].t);
          const x1 = x(vis[i+1].t);
          ctx.fillStyle = 'rgba(255, 107, 107, 0.10)';
          ctx.fillRect(x0, 10, Math.max(1, x1-x0), h-30);
        }
      }
    }

    // Metric line
    ctx.strokeStyle = '#e8eef6';
    ctx.lineWidth = 2;
    ctx.beginPath();
    for(let i=0;i<vis.length;i++){
      const f = vis[i];
      const xx = x(f.t);
      const pv = nfPick(f);
      const yy = y(pv.metric);
      if(i===0) ctx.moveTo(xx,yy); else ctx.lineTo(xx,yy);
    }
    ctx.stroke();

    // Threshold line (dashed)
    ctx.strokeStyle = '#64d2ff';
    ctx.setLineDash([6,4]);
    ctx.lineWidth = 2;
    ctx.beginPath();
    for(let i=0;i<vis.length;i++){
      const f = vis[i];
      const xx = x(f.t);
      const pv = nfPick(f);
      const yy = y(pv.threshold);
      if(i===0) ctx.moveTo(xx,yy); else ctx.lineTo(xx,yy);
    }
    ctx.stroke();
    ctx.setLineDash([]);

    // Axis labels
    ctx.fillStyle = '#9fb0c3';
    ctx.font = `${Math.round(11*(window.devicePixelRatio||1))}px system-ui`;
    ctx.fillText(`t: ${tMin.toFixed(1)}–${tNow.toFixed(1)}s`, 12, h-6);
    ctx.fillText(`y: ${fmt(yMin)}–${fmt(yMax)}`, 12, 22);
  }

  function hslToRgb(h, s, l){
    h = ((h%360)+360)%360;
    s = Math.max(0, Math.min(1, s));
    l = Math.max(0, Math.min(1, l));
    const c = (1 - Math.abs(2*l - 1)) * s;
    const hp = h / 60;
    const x = c * (1 - Math.abs((hp % 2) - 1));
    let r=0,g=0,b=0;
    if(0<=hp && hp<1){ r=c; g=x; b=0; }
    else if(1<=hp && hp<2){ r=x; g=c; b=0; }
    else if(2<=hp && hp<3){ r=0; g=c; b=x; }
    else if(3<=hp && hp<4){ r=0; g=x; b=c; }
    else if(4<=hp && hp<5){ r=x; g=0; b=c; }
    else if(5<=hp && hp<6){ r=c; g=0; b=x; }
    const m = l - c/2;
    r = Math.round(255*(r+m)); g=Math.round(255*(g+m)); b=Math.round(255*(b+m));
    return [r,g,b];
  }

  function valueToRgb(v, vmin, vmax){
    if(v===null || v===undefined || !Number.isFinite(v) || !(vmax>vmin)) return [42,52,64];
    let t = (v - vmin) / (vmax - vmin);
    if(t<0) t=0; if(t>1) t=1;
    const hue = (1 - t) * 240;
    return hslToRgb(hue, 0.90, 0.55);
  }

  function drawTopo(){
    const meta = state.bp.meta;
    if(!meta){
      resizeCanvasTo(topoCanvas);
      topoCtx.clearRect(0,0,topoCanvas.width, topoCanvas.height);
      topoCtx.fillStyle = '#9fb0c3';
      topoCtx.font = `${Math.round(12*(window.devicePixelRatio||1))}px system-ui`;
      topoCtx.fillText('Waiting for bandpower_timeseries.csv …', 14*(window.devicePixelRatio||1), 24*(window.devicePixelRatio||1));
      vminEl.textContent = 'min: —';
      vmaxEl.textContent = 'max: —';
      return;
    }

    const band = bandSel.value || (meta.bands[0] || '');
    const bIdx = Math.max(0, meta.bands.indexOf(band));
    const nCh = meta.channels.length;
    const latest = state.bp.frames.length ? state.bp.frames[state.bp.frames.length-1] : null;

    let vals = null;
    if(latest && Array.isArray(latest.values) && latest.values.length >= (meta.bands.length*nCh)){
      vals = [];
      for(let c=0;c<nCh;c++){
        const col = bIdx*nCh + c;
        vals.push(bpDisplayValue(col, latest.values[col]));
      }
    }

    let vmin = Infinity, vmax = -Infinity;
    if(vals){
      for(const v of vals){ if(v!==null && v!==undefined && Number.isFinite(v)){ vmin=Math.min(vmin,v); vmax=Math.max(vmax,v);} }
    }
    if(!(vmax>vmin)) { vmin = 0; vmax = 1; }

    if(scaleSel.value === 'fixed'){
      if(state.bp.rollingMin===null || state.bp.rollingMax===null){
        state.bp.rollingMin = vmin; state.bp.rollingMax = vmax;
      } else {
        state.bp.rollingMin = 0.98*state.bp.rollingMin + 0.02*vmin;
        state.bp.rollingMax = 0.98*state.bp.rollingMax + 0.02*vmax;
      }
      vmin = state.bp.rollingMin;
      vmax = state.bp.rollingMax;
    }

    vminEl.textContent = `min: ${Number.isFinite(vmin)?vmin.toFixed(3):'—'}`;
    vmaxEl.textContent = `max: ${Number.isFinite(vmax)?vmax.toFixed(3):'—'}`;

    resizeCanvasTo(topoCanvas);
    const w = topoCanvas.width, h = topoCanvas.height;
    topoCtx.clearRect(0,0,w,h);

    const dpr = window.devicePixelRatio || 1;
    const cx = w/2, cy = h/2 + 6*dpr;
    const R = Math.min(w, h) * 0.42;

    topoCtx.fillStyle = '#071018';
    topoCtx.fillRect(0,0,w,h);

    const grid = 120;
    const imgW = grid, imgH = grid;
    const img = topoCtx.createImageData(imgW, imgH);
    const eps = 1e-6;
    const p = 2.0;

    const pts = [];
    if(vals){
      for(let i=0;i<nCh;i++){
        const pos = meta.positions[i];
        const v = vals[i];
        if(!pos) continue;
        if(v===null || v===undefined || !Number.isFinite(v)) continue;
        pts.push({x:pos[0], y:pos[1], v});
      }
    }

    for(let gy=0; gy<imgH; gy++){
      for(let gx=0; gx<imgW; gx++){
        const xh = (gx/(imgW-1))*2 - 1;
        const yh = (gy/(imgH-1))*2 - 1;
        const rr = xh*xh + yh*yh;
        const idx = (gy*imgW + gx)*4;
        if(rr > 1.0){
          img.data[idx+3] = 0;
          continue;
        }
        if(!pts.length){
          img.data[idx+0] = 42; img.data[idx+1] = 52; img.data[idx+2] = 64; img.data[idx+3] = 255;
          continue;
        }
        let sw = 0, sv = 0;
        for(const pt of pts){
          const dx = xh - pt.x;
          const dy = yh - pt.y;
          const d2 = dx*dx + dy*dy;
          const wgt = 1.0 / (Math.pow(d2 + eps, p/2));
          sw += wgt;
          sv += wgt * pt.v;
        }
        const vv = (sw>0) ? (sv/sw) : null;
        const [r,g,b] = valueToRgb(vv, vmin, vmax);
        img.data[idx+0]=r; img.data[idx+1]=g; img.data[idx+2]=b; img.data[idx+3]=255;
      }
    }

    topoCtx.save();
    topoCtx.beginPath();
    topoCtx.arc(cx, cy, R, 0, Math.PI*2);
    topoCtx.clip();
    topoCtx.imageSmoothingEnabled = true;
    const x0 = cx - R, y0 = cy - R;
    if(!state.bp.tmpCanvas){ state.bp.tmpCanvas = document.createElement('canvas'); }
    const tmp = state.bp.tmpCanvas;
    if(state.bp.tmpW !== imgW || state.bp.tmpH !== imgH){ tmp.width = imgW; tmp.height = imgH; state.bp.tmpW = imgW; state.bp.tmpH = imgH; }
    tmp.getContext('2d').putImageData(img, 0, 0);
    topoCtx.drawImage(tmp, x0, y0, 2*R, 2*R);
    topoCtx.restore();

    topoCtx.strokeStyle = 'rgba(255,255,255,0.25)';
    topoCtx.lineWidth = 2*dpr;
    topoCtx.beginPath();
    topoCtx.arc(cx, cy, R, 0, Math.PI*2);
    topoCtx.stroke();

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

    const showLabels = (lblSel.value || 'on') === 'on';
    state.bp.electrodesPx = [];
    topoCtx.font = `${Math.round(11*dpr)}px system-ui`;
    topoCtx.textAlign = 'center';
    topoCtx.textBaseline = 'middle';
    for(let i=0;i<nCh;i++){
      const pos = meta.positions[i];
      if(!pos) continue;
      const ex = cx + pos[0]*R;
      const ey = cy - pos[1]*R;
      state.bp.electrodesPx.push([ex, ey, i]);
      topoCtx.beginPath();
      topoCtx.arc(ex, ey, 7*dpr, 0, Math.PI*2);
      topoCtx.fillStyle = 'rgba(0,0,0,0.35)';
      topoCtx.fill();
      const src = (meta.positions_source && meta.positions_source[i]) ? meta.positions_source[i] : '';
      topoCtx.strokeStyle = (src === 'fallback') ? 'rgba(255,207,92,0.65)' : 'rgba(255,255,255,0.35)';
      topoCtx.lineWidth = 1.5*dpr;
      topoCtx.stroke();
      if(showLabels){
        topoCtx.fillStyle = 'rgba(255,255,255,0.78)';
        topoCtx.fillText(meta.channels[i], ex, ey);
      }
    }
  }

  function drawBandpowerSeries(){
    const meta = state.bp.meta;
    resizeCanvasTo(bpCanvas);
    const w = bpCanvas.width, h = bpCanvas.height;
    bpCtx.clearRect(0,0,w,h);
    bpCtx.fillStyle = '#071018';
    bpCtx.fillRect(0,0,w,h);

    if(!meta || !state.bp.frames.length){
      bpCtx.fillStyle = '#9fb0c3';
      bpCtx.font = `${Math.round(12*(window.devicePixelRatio||1))}px system-ui`;
      bpCtx.fillText('Waiting for bandpower_timeseries.csv …', 14*(window.devicePixelRatio||1), 24*(window.devicePixelRatio||1));
      return;
    }

    const band = bandSel.value || (meta.bands[0] || '');
    const bIdx = Math.max(0, meta.bands.indexOf(band));
    const chName = chSel.value || (meta.channels[0] || '');
    const cIdx = Math.max(0, meta.channels.indexOf(chName));
    const nCh = meta.channels.length;

    const frames = state.bp.frames;
    const tNow = frames[frames.length-1].t;
    const tMin = Math.max(frames[0].t, tNow - state.winSec);
    const vis = [];
    for(const f of frames){
      if(f.t >= tMin && f.values && f.values.length >= meta.bands.length*nCh){
        const col = bIdx*nCh + cIdx;
        vis.push({t:f.t, v:bpDisplayValue(col, f.values[col])});
      }
    }
    if(vis.length < 2) return;

    let yMin = Infinity, yMax = -Infinity;
    for(const p of vis){
      if(p.v!==null && p.v!==undefined && Number.isFinite(p.v)){
        yMin = Math.min(yMin, p.v);
        yMax = Math.max(yMax, p.v);
      }
    }
    if(!(yMax>yMin)){ yMax = yMin + 1; }
    const pad = 0.05*(yMax-yMin);
    yMin -= pad; yMax += pad;

    const x = (t) => (t - tMin) / (tNow - tMin) * (w-20) + 10;
    const y = (v) => (h-20) - (v - yMin)/(yMax-yMin) * (h-30);

    bpCtx.strokeStyle = 'rgba(255,255,255,0.06)';
    bpCtx.lineWidth = 1;
    for(let i=0;i<=4;i++){
      const yy = 10 + i*(h-30)/4;
      bpCtx.beginPath(); bpCtx.moveTo(10,yy); bpCtx.lineTo(w-10,yy); bpCtx.stroke();
    }

    bpCtx.strokeStyle = '#e8eef6';
    bpCtx.lineWidth = 2;
    bpCtx.beginPath();
    for(let i=0;i<vis.length;i++){
      const p = vis[i];
      const xx = x(p.t);
      const yy = y(p.v);
      if(i===0) bpCtx.moveTo(xx,yy); else bpCtx.lineTo(xx,yy);
    }
    bpCtx.stroke();

    bpCtx.fillStyle = '#9fb0c3';
    bpCtx.font = `${Math.round(11*(window.devicePixelRatio||1))}px system-ui`;
    const xm = (xformSel && xformSel.value) ? xformSel.value : 'linear';
    const xLabel = (xm && xm !== 'linear') ? ` (${xm})` : '';
    bpCtx.fillText(`${band}:${chName}${xLabel}  y: ${fmt(yMin)}–${fmt(yMax)}`, 12, 22);
    bpCtx.fillText(`t: ${tMin.toFixed(1)}–${tNow.toFixed(1)}s`, 12, h-6);
  }

  function connectSSE(path, badgeEl, label){
    if(!window.EventSource){
      setBadge(badgeEl, `${label}: no EventSource`, 'warn');
      return null;
    }
    const url = `${path}?token=${encodeURIComponent(TOKEN)}`;
    const es = new EventSource(url);
    setBadge(badgeEl, `${label}: connecting`, 'warn');
    es.onopen = () => { setBadge(badgeEl, `${label}: connected`, 'good'); };
    // Browsers automatically reconnect EventSource streams.
    es.onerror = () => { setBadge(badgeEl, `${label}: reconnecting`, 'warn'); };
    return es;
  }

  function connectStream(topics, hz){
    if(!window.EventSource){
      return null;
    }
    const t = Array.isArray(topics) ? topics.filter(Boolean).join(',') : String(topics||'');
    const qp = [];
    if(t){ qp.push(`topics=${encodeURIComponent(t)}`); }
    if(Number.isFinite(hz) && hz > 0){ qp.push(`hz=${encodeURIComponent(String(hz))}`); }
    const q = qp.length ? ('&' + qp.join('&')) : '';
    const url = `/api/sse/stream?token=${encodeURIComponent(TOKEN)}${q}`;
    return new EventSource(url);
  }

  function unpackMsg(msg){
    if(msg && typeof msg === 'object' && msg.type === 'batch' && Array.isArray(msg.frames)){
      return {frames: msg.frames, reset: !!msg.reset};
    }
    return {frames: [msg], reset: false};
  }

  // ---------------- UI state sync (frontend <-> backend) ----------------

  function uiStateFromControls(){
    return {
      win_sec: parseFloat(winSel.value || '60') || 60,
      paused: !!state.paused,
      band: (bandSel && bandSel.value) ? bandSel.value : null,
      channel: (chSel && chSel.value) ? chSel.value : null,
      transform: (xformSel && xformSel.value) ? xformSel.value : 'linear',
      scale: (scaleSel && scaleSel.value) ? scaleSel.value : 'auto',
      labels: (lblSel && lblSel.value) ? lblSel.value : 'on',
      nf_mode: (nfModeSel && nfModeSel.value) ? nfModeSel.value : 'raw',
      map_mode: (mapSel && mapSel.value) ? mapSel.value : 'raw',
      client_id: CLIENT_ID,
    };
  }

  let pushTimer = null;
  function schedulePushUiState(){
    if(state.applyingRemote) return;
    if(!TOKEN) return;
    if(pushTimer) clearTimeout(pushTimer);
    pushTimer = setTimeout(() => {
      pushTimer = null;
      const body = uiStateFromControls();
      fetch(`/api/state?token=${encodeURIComponent(TOKEN)}` , {
        method: 'PUT',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(body)
      }).catch(() => {});
    }, 120);
  }

  function applyUiState(st){
    if(!st || typeof st !== 'object') return;
    state.applyingRemote = true;
    try{
      if(typeof st.win_sec === 'number' && Number.isFinite(st.win_sec)){
        const v = Math.round(st.win_sec);
        // If the select doesn't have this value, keep existing.
        const opt = Array.from(winSel.options).find(o => parseInt(o.value,10) === v);
        if(opt){ winSel.value = String(v); state.winSec = v; }
      }
      if(typeof st.paused === 'boolean'){
        state.paused = st.paused;
        pauseBtn.textContent = state.paused ? 'Resume' : 'Pause';
      }
      if(typeof st.transform === 'string'){
        if(['linear','log10','db'].includes(st.transform)) xformSel.value = st.transform;
      }
      if(typeof st.scale === 'string'){
        if(['auto','fixed'].includes(st.scale)) scaleSel.value = st.scale;
      }
      if(typeof st.labels === 'string'){
        if(['on','off'].includes(st.labels)) lblSel.value = st.labels;
      }
      if(typeof st.nf_mode === 'string'){
        if(nfModeSel && ['raw','zbase','zref'].includes(st.nf_mode)) nfModeSel.value = st.nf_mode;
      }
      if(typeof st.map_mode === 'string'){
        if(mapSel && ['raw','zbase','zref'].includes(st.map_mode)) mapSel.value = st.map_mode;
      }
      // band/channel depend on meta; stash if not yet available.
      state.pendingUi = state.pendingUi || {};
      if(typeof st.band === 'string' && st.band) state.pendingUi.band = st.band;
      if(typeof st.channel === 'string' && st.channel) state.pendingUi.channel = st.channel;
      dirtyNf = true;
      dirtyBp = true;
      scheduleRender();
    } finally {
      state.applyingRemote = false;
    }
  }

  function tryApplyPendingSelection(){
    if(!state.pendingUi) return;
    const meta = state.bp.meta;
    if(!meta) return;
    const p = state.pendingUi;
    let changed = false;
    if(p.band && meta.bands && meta.bands.includes(p.band)){
      bandSel.value = p.band;
      p.band = null;
      changed = true;
    }
    if(p.channel && meta.channels && meta.channels.includes(p.channel)){
      chSel.value = p.channel;
      p.channel = null;
      changed = true;
    }
    if(changed){ drawTopo(); drawBandpowerSeries(); }
  }

  // ---------------- start + streaming ----------------

  pauseBtn.onclick = () => {
    state.paused = !state.paused;
    pauseBtn.textContent = state.paused ? 'Resume' : 'Pause';
    schedulePushUiState();
    dirtyNf = true;
    dirtyBp = true;
    scheduleRender();
  };

  winSel.onchange = () => {
    state.winSec = parseFloat(winSel.value||'60') || 60;
    schedulePushUiState();
    dirtyNf = true;
    scheduleRender();
  };

  bandSel.onchange = () => { dirtyBp = true; scheduleRender(); schedulePushUiState(); };
  chSel.onchange = () => { dirtyBp = true; scheduleRender(); schedulePushUiState(); };
  lblSel.onchange = () => { dirtyBp = true; scheduleRender(); schedulePushUiState(); };
  xformSel.onchange = () => { state.bp.rollingMin = null; state.bp.rollingMax = null; dirtyBp = true; scheduleRender(); schedulePushUiState(); };
  if(mapSel){
    mapSel.onchange = () => {
      state.bp.rollingMin = null; state.bp.rollingMax = null;
      if(mapSel.value === 'zref' && !state.bp.reference){ loadReference(); }
      dirtyBp = true; scheduleRender(); schedulePushUiState();
    };
  }
  if(nfModeSel){
    nfModeSel.onchange = () => { dirtyNf = true; updateStats(); scheduleRender(); schedulePushUiState(); };
  }
  scaleSel.onchange = () => { state.bp.rollingMin = null; state.bp.rollingMax = null; dirtyBp = true; scheduleRender(); schedulePushUiState(); };

  function applyMeta(meta){
    if(!meta || typeof meta !== 'object') return;
    updateFileStatus(meta);
    updateRunMetaStatus(meta);
    if(meta.bandpower && meta.bandpower.bands && meta.bandpower.channels){
      const bp = meta.bandpower;
      const prev = state.bp.meta;
      const prevBands = prev && prev.bands ? prev.bands.join('|') : '';
      const prevCh = prev && prev.channels ? prev.channels.join('|') : '';
      const newBands = bp.bands.join('|');
      const newCh = bp.channels.join('|');
      const bandWas = bandSel.value;
      const chWas = chSel.value;
      state.bp.meta = bp;
      if(newBands !== prevBands || newCh !== prevCh){ state.bp.baseline = null; state.bp.reference = null; }
      if(newBands !== prevBands){
        bandSel.innerHTML = '';
        for(const b of bp.bands){
          const opt = document.createElement('option');
          opt.value = b; opt.textContent = b;
          bandSel.appendChild(opt);
        }
      }
      if(newCh !== prevCh){
        chSel.innerHTML = '';
        for(const c of bp.channels){
          const opt = document.createElement('option');
          opt.value = c; opt.textContent = c;
          chSel.appendChild(opt);
        }
      }
      if(bandWas && bp.bands.includes(bandWas)) bandSel.value = bandWas;
      if(chWas && bp.channels.includes(chWas)) chSel.value = chWas;
      tryApplyPendingSelection();
    }
    // Load normative reference (if provided by qeeg_nf_cli).
    const refStat = meta.files_stat && meta.files_stat.reference_used;
    if(refStat && refStat.exists && !state.bp.reference){ loadReference(); }
  }

  // ---------------- rendering + event handlers ----------------

  let uiBound = false;
  let renderPending = false;
  let dirtyNf = true;
  let dirtyBp = true;

  function scheduleRender(){
    if(renderPending) return;
    renderPending = true;
    requestAnimationFrame(() => {
      renderPending = false;
      if(dirtyNf){ drawChart(); dirtyNf = false; }
      if(dirtyBp){ drawTopo(); drawBandpowerSeries(); dirtyBp = false; }
    });
  }

  function markAllReconnect(txt){
    setBadge(nfConn, `nf: ${txt}`, 'warn');
    setBadge(bpConn, `bandpower: ${txt}`, 'warn');
    setBadge(artConn, `artifact: ${txt}`, 'warn');
    setBadge(metaConn, `meta: ${txt}`, 'warn');
    setBadge(stateConn, `state: ${txt}`, 'warn');
  }

  function markLive(el, label){
    setBadge(el, `${label}: live`, 'good');
  }

  function handleStateMsg(raw){
    try{
      const un = unpackMsg(raw);
      for(const st of un.frames){
        const cid = st && st.client_id ? String(st.client_id) : '';
        if(cid && cid === CLIENT_ID) continue;
        applyUiState(st);
      }
      dirtyNf = true;
      dirtyBp = true;
      scheduleRender();
      markLive(stateConn, 'state');
    }catch(e){}
  }

  function handleMetaMsg(raw){
    try{
      const un = unpackMsg(raw);
      let dirty = false;
      for(const m of un.frames){ applyMeta(m); dirty = true; }
      if(dirty){
        dirtyBp = true;
        scheduleRender();
      }
      markLive(metaConn, 'meta');
    }catch(e){}
  }

  function handleNfMsg(raw){
    if(state.paused) return;
    try{
      const un = unpackMsg(raw);
      if(un.reset){ state.nf.frames = []; state.nf.lastT = -Infinity; }
      for(const f of un.frames){
        if(!f || typeof f !== 'object') continue;
        if(f.t !== null && f.t !== undefined && Number.isFinite(f.t)){
          if(f.t <= state.nf.lastT) continue;
          state.nf.lastT = f.t;
        }
        state.nf.frames.push(f);
        if(f.artifact !== undefined) state.art.latest = {bad: !!f.artifact, bad_channels: f.bad_channels||0};
      }
      if(state.nf.frames.length > 20000) state.nf.frames.splice(0, state.nf.frames.length-20000);
      updateStats();
      dirtyNf = true;
      scheduleRender();
      markLive(nfConn, 'nf');
    }catch(e){}
  }

  function handleBandpowerMsg(raw){
    if(state.paused) return;
    try{
      const un = unpackMsg(raw);
      if(un.reset){ state.bp.frames = []; state.bp.lastT = -Infinity; state.bp.rollingMin = null; state.bp.rollingMax = null; state.bp.baseline = null; }
      for(const f of un.frames){
        if(!f || typeof f !== 'object') continue;
        if(f.t !== null && f.t !== undefined && Number.isFinite(f.t)){
          if(f.t <= state.bp.lastT) continue;
          state.bp.lastT = f.t;
        }
        state.bp.frames.push(f);
        // Collect baseline stats (client-side) for z-score mapping.
        if(f && Array.isArray(f.values) && typeof f.t === 'number' && Number.isFinite(f.t)){
          const badNow = state.art.latest && state.art.latest.bad;
          if(!badNow && f.t <= (state.bp.baselineSec || 0)) baselineUpdate(f.values);
        }
      }
      if(state.bp.frames.length > 20000) state.bp.frames.splice(0, state.bp.frames.length-20000);
      dirtyBp = true;
      scheduleRender();
      markLive(bpConn, 'bandpower');
    }catch(e){}
  }

  function handleArtifactMsg(raw){
    if(state.paused) return;
    try{
      const un = unpackMsg(raw);
      if(un.reset){ state.art.frames = []; state.art.lastT = -Infinity; state.art.latest = null; }
      for(const f of un.frames){
        if(!f || typeof f !== 'object') continue;
        if(f.t !== null && f.t !== undefined && Number.isFinite(f.t)){
          if(f.t <= state.art.lastT) continue;
          state.art.lastT = f.t;
        }
        state.art.frames.push(f);
        state.art.latest = f;
      }
      if(state.art.frames.length > 40000) state.art.frames.splice(0, state.art.frames.length-40000);
      updateStats();
      dirtyNf = true;
      scheduleRender();
      markLive(artConn, 'artifact');
    }catch(e){}
  }

  function ensureUiBindings(){
    if(uiBound) return;
    uiBound = true;

    window.addEventListener('resize', () => {
      dirtyNf = true;
      dirtyBp = true;
      scheduleRender();
    }, {passive:true});

    topoCanvas.addEventListener('click', (ev) => {
      const meta = state.bp.meta;
      if(!meta || !meta.channels || !meta.positions) return;
      const rect = topoCanvas.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      const x = (ev.clientX - rect.left) * dpr;
      const y = (ev.clientY - rect.top) * dpr;
      const pts = state.bp.electrodesPx || [];
      let best = -1;
      let bestD = Infinity;
      for(const p of pts){
        const dx = x - p[0];
        const dy = y - p[1];
        const d2 = dx*dx + dy*dy;
        if(d2 < bestD){ bestD = d2; best = p[2]; }
      }
      const thresh = (32*dpr) * (32*dpr);
      if(best >= 0 && bestD <= thresh){
        const name = meta.channels[best];
        if(name){
          chSel.value = name;
          dirtyBp = true;
          scheduleRender();
          schedulePushUiState();
        }
      }
    });
  }

  // ---------------- start + streaming ----------------

  function startStream(){
    const es = connectStream(['config','meta','state','nf','artifact','bandpower']);
    if(!es){
      // Fallback to polling mode if EventSource is unavailable.
      startPolling();
      return;
    }

    markAllReconnect('connecting');

    es.onopen = () => {
      // Wait until first event per topic before marking green.
      setBadge(nfConn, 'nf: connected', 'warn');
      setBadge(bpConn, 'bandpower: connected', 'warn');
      setBadge(artConn, 'artifact: connected', 'warn');
      setBadge(metaConn, 'meta: connected', 'warn');
      setBadge(stateConn, 'state: connected', 'warn');
    };

    es.onerror = () => {
      markAllReconnect('reconnecting');
    };

    es.addEventListener('state', (ev) => {
      try{ handleStateMsg(JSON.parse(ev.data)); }catch(e){}
    });
    es.addEventListener('meta', (ev) => {
      try{ handleMetaMsg(JSON.parse(ev.data)); }catch(e){}
    });
    es.addEventListener('nf', (ev) => {
      try{ handleNfMsg(JSON.parse(ev.data)); }catch(e){}
    });
    es.addEventListener('bandpower', (ev) => {
      try{ handleBandpowerMsg(JSON.parse(ev.data)); }catch(e){}
    });
    es.addEventListener('artifact', (ev) => {
      try{ handleArtifactMsg(JSON.parse(ev.data)); }catch(e){}
    });
  }

  function startLegacy(){
    if(!window.EventSource){
      startPolling();
      return;
    }
    // UI state sync
    const stateSse = connectSSE('/api/sse/state', stateConn, 'state');
    if(stateSse){
      stateSse.onmessage = (ev) => {
        try{ handleStateMsg(JSON.parse(ev.data)); }catch(e){}
      };
    } else {
      setBadge(stateConn, 'state: off', 'warn');
    }

    // Meta updates (SSE preferred; polling fallback)
    const metaSse = connectSSE('/api/sse/meta', metaConn, 'meta');
    if(metaSse){
      metaSse.onmessage = (ev) => {
        try{ handleMetaMsg(JSON.parse(ev.data)); }catch(e){}
      };
    } else {
      setBadge(metaConn, 'meta: polling', 'warn');
      setInterval(() => {
        fetch(`/api/meta?token=${encodeURIComponent(TOKEN)}`)
          .then(r => r.ok ? r.json() : null)
          .then(m => { if(m) handleMetaMsg(m); })
          .catch(() => {});
      }, 2000);
    }

    // Time-series streams
    const nf = connectSSE('/api/sse/nf', nfConn, 'nf');
    if(nf){
      nf.onmessage = (ev) => {
        try{ handleNfMsg(JSON.parse(ev.data)); }catch(e){}
      };
    }

    const bp = connectSSE('/api/sse/bandpower', bpConn, 'bandpower');
    if(bp){
      bp.onmessage = (ev) => {
        try{ handleBandpowerMsg(JSON.parse(ev.data)); }catch(e){}
      };
    }

    const art = connectSSE('/api/sse/artifact', artConn, 'artifact');
    if(art){
      art.onmessage = (ev) => {
        try{ handleArtifactMsg(JSON.parse(ev.data)); }catch(e){}
      };
    }
  }


  function startPolling(){
    // Long-polling fallback using /api/snapshot (for environments without SSE/EventSource).
    if(!TOKEN){ return; }

    setBadge(nfConn, 'nf: polling', 'warn');
    setBadge(bpConn, 'bandpower: polling', 'warn');
    setBadge(artConn, 'artifact: polling', 'warn');
    setBadge(metaConn, 'meta: polling', 'warn');
    setBadge(stateConn, 'state: polling', 'warn');

    const topics = ['config','meta','state','nf','artifact','bandpower'];
    const cur = {nf:0, bandpower:0, artifact:0, meta:0, state:0};
    let inflight = false;
    let backoffMs = 0;

    function buildUrl(){
      const qp = [];
      qp.push(`token=${encodeURIComponent(TOKEN)}`);
      qp.push(`topics=${encodeURIComponent(topics.join(','))}`);
      qp.push(`wait=1.0`);
      qp.push(`limit=2500`);
      qp.push(`nf=${encodeURIComponent(String(cur.nf||0))}`);
      qp.push(`bandpower=${encodeURIComponent(String(cur.bandpower||0))}`);
      qp.push(`artifact=${encodeURIComponent(String(cur.artifact||0))}`);
      qp.push(`meta=${encodeURIComponent(String(cur.meta||0))}`);
      qp.push(`state=${encodeURIComponent(String(cur.state||0))}`);
      return `/api/snapshot?${qp.join('&')}`;
    }

    function applyTopic(topic, payload, handler){
      if(!payload || typeof payload !== 'object') return;
      if(typeof payload.cursor === 'number' && Number.isFinite(payload.cursor)){
        cur[topic] = payload.cursor;
      }
      if(payload.batch){
        handler(payload.batch);
      }
    }

    function pollOnce(){
      if(inflight) return;
      inflight = true;
      fetch(buildUrl())
        .then(r => r.ok ? r.json() : null)
        .then(resp => {
          if(resp){
            // cursors are updated inside applyTopic; handlers manage rendering.
            applyTopic('meta', resp.meta, handleMetaMsg);
            applyTopic('state', resp.state, handleStateMsg);
            applyTopic('nf', resp.nf, handleNfMsg);
            applyTopic('artifact', resp.artifact, handleArtifactMsg);
            applyTopic('bandpower', resp.bandpower, handleBandpowerMsg);
            // Mark as live even if no frames arrived (helps UX).
            setBadge(nfConn, 'nf: polling', 'warn');
            setBadge(bpConn, 'bandpower: polling', 'warn');
            setBadge(artConn, 'artifact: polling', 'warn');
            setBadge(metaConn, 'meta: polling', 'warn');
            setBadge(stateConn, 'state: polling', 'warn');
            backoffMs = 0;
          }
        })
        .catch(() => {
          backoffMs = Math.min(10000, (backoffMs ? Math.round(backoffMs*1.5) : 500));
        })
        .finally(() => {
          inflight = false;
          const delay = backoffMs || 50;
          setTimeout(pollOnce, delay);
        });
    }

    pollOnce();
  }

  // ------------------------ run meta + stats ------------------------

  async function loadRunMeta(force=false){
    if(!TOKEN) return;
    if(!runMetaRaw && !runMetaKv && !runMetaStatus) return;
    const url = `/api/run_meta?token=${encodeURIComponent(TOKEN)}`;
    const headers = {};
    if(!force && runMetaETag) headers['If-None-Match'] = runMetaETag;
    let resp;
    try{
      resp = await fetch(url, {headers});
    }catch(e){
      if(runMetaStatus) runMetaStatus.textContent = 'run meta: fetch failed';
      if(runMetaRaw) runMetaRaw.textContent = String(e);
      return;
    }
    if(resp.status === 304){
      return;
    }
    if(!resp.ok){
      if(runMetaStatus) runMetaStatus.textContent = `run meta: ${resp.status}`;
      if(runMetaRaw) runMetaRaw.textContent = await resp.text().catch(()=> '');
      return;
    }
    const et = resp.headers.get('ETag');
    if(et) runMetaETag = et;

    let data;
    try{
      data = await resp.json();
    }catch(e){
      if(runMetaStatus) runMetaStatus.textContent = 'run meta: bad JSON';
      if(runMetaRaw) runMetaRaw.textContent = String(e);
      return;
    }

    if(data && data.etag && !runMetaETag) runMetaETag = data.etag;

    const st = data && data.stat ? data.stat : null;
    if(st && st.exists){
      const now = Date.now()/1000;
      const age = st.mtime_utc ? (now - st.mtime_utc) : null;
      if(runMetaStatus){
        runMetaStatus.textContent = `present (age: ${fmtAgeSec(age)})`;
        runMetaStatus.classList.remove('muted');
      }
    }else{
      if(runMetaStatus){
        runMetaStatus.textContent = 'missing';
        runMetaStatus.classList.add('muted');
      }
    }

    // Render a compact view + the full JSON (pretty-printed).
    if(data && data.data && typeof data.data === 'object'){
      const order = [
        'Tool','Version','GitDescribe','TimestampLocal','OutputDir','protocol','metric_spec','band_spec','reward_direction',
        'fs_hz','window_seconds','update_seconds','baseline_seconds','target_reward_rate','artifact_gate',
        'qc_bad_channel_count','qc_bad_channels','biotrace_ui','export_derived_events','derived_events_written'
      ];
      renderKvGrid(runMetaKv, data.data, order);
      if(runMetaRaw) runMetaRaw.textContent = JSON.stringify(data.data, null, 2);
    }else{
      renderKvGrid(runMetaKv, null);
      if(runMetaRaw) runMetaRaw.textContent = (data && data.parse_error) ? `Parse error: ${data.parse_error}` : '';
    }

    if(runMetaDownload && TOKEN){
      runMetaDownload.href = `/api/run_meta?token=${encodeURIComponent(TOKEN)}&format=raw`;
    }
  }

  async function loadStatsOnce(){
    if(!TOKEN) return;
    if(!serverStats && !statsKv) return;
    const url = `/api/stats?token=${encodeURIComponent(TOKEN)}`;
    let resp;
    try{
      resp = await fetch(url, {cache:'no-store'});
    }catch(e){
      if(serverStats) serverStats.textContent = 'stats: fetch failed';
      return;
    }
    if(!resp.ok){
      if(serverStats) serverStats.textContent = `stats: ${resp.status}`;
      return;
    }
    let data;
    try{
      data = await resp.json();
    }catch(e){
      if(serverStats) serverStats.textContent = 'stats: bad JSON';
      return;
    }

    const up = (data && typeof data.uptime_sec === 'number') ? data.uptime_sec : null;
    const c = data && data.connections ? data.connections : {};
    if(serverStats){
      const viewers = (c && typeof c.stream === 'number') ? c.stream : null;
      const upStr = (up === null) ? '—' : fmtAgeSec(up);
      const vStr = (viewers === null) ? '—' : String(viewers);
      serverStats.textContent = `uptime: ${upStr}, viewers: ${vStr}`;
      serverStats.classList.remove('muted');
    }

    // Flatten for display.
    const flat = {};
    if(data){
      flat.frontend = data.frontend || '';
      if(up !== null) flat.uptime_sec = up.toFixed(1);
      if(data.server_instance_id) flat.server_instance_id = data.server_instance_id;
      const connKeys = ['stream','nf','bandpower','artifact','meta','state'];
      for(const k of connKeys){
        if(c && typeof c[k] === 'number') flat[`conn_${k}`] = c[k];
      }
      const bufs = data.buffers || {};
      for(const k of ['nf','bandpower','artifact','meta','state']){
        if(bufs && bufs[k]){
          flat[`buf_${k}_size`] = bufs[k].size;
          flat[`buf_${k}_latest`] = bufs[k].latest_seq;
        }
      }
    }
    const order = Object.keys(flat);
    renderKvGrid(statsKv, flat, order);
  }

  function startStatsPolling(){
    if(statsTimer) return;
    statsTimer = setInterval(()=>{ loadStatsOnce(); }, 2000);
    loadStatsOnce();
  }



  function start(){
    if(!TOKEN){
      showStatus('Missing token. Re-open using the printed URL (it includes <code>?token=…</code>).');
      return;
    }

    // Wire up Session panel controls (safe no-ops if elements missing).
    if(runMetaDownload){
      runMetaDownload.href = `/api/run_meta?token=${encodeURIComponent(TOKEN)}&format=raw`;
    }
    if(runMetaRefresh){
      runMetaRefresh.addEventListener('click', ()=>{ loadRunMeta(true); });
    }
    if(runMetaDetails){
      runMetaDetails.addEventListener('toggle', ()=>{
        if(runMetaDetails.open) loadRunMeta(false);
      });
    }

    ensureUiBindings();
    markAllReconnect('…');

    // Load server state (best-effort) and apply early.
    fetch(`/api/state?token=${encodeURIComponent(TOKEN)}`)
      .then(r => r.ok ? r.json() : null)
      .then(st => {
        if(st){
          applyUiState(st);
          dirtyNf = true;
          dirtyBp = true;
          scheduleRender();
        }
      })
      .catch(() => {});

    // Load meta once to populate band/channel selects, then pick streaming mode.
    fetch(`/api/meta?token=${encodeURIComponent(TOKEN)}`)
      .then(r => r.ok ? r.json() : Promise.reject(r.status))
      .then(meta => {
        applyMeta(meta);
        dirtyBp = true;
        dirtyNf = true;
        scheduleRender();

        // If EventSource is unavailable, fall back to polling snapshots.
        if(!window.EventSource){
          startPolling();
          return;
        }

        // Prefer multiplexed stream if the server supports it.
        fetch(`/api/config?token=${encodeURIComponent(TOKEN)}`)
          .then(r => r.ok ? r.json() : null)
          .then(cfg => {
            const supports = cfg && cfg.supports ? cfg.supports : {};
            if(supports && supports.stats){
              startStatsPolling();
            }
            if(supports && supports.sse_stream){
              startStream();
            } else {
              startLegacy();
            }
          })
          .catch(() => startLegacy());
      })
      .catch(err => {
        showStatus('Failed to load metadata from the server. Is the process still running?');
        setBadge(nfConn, 'nf: offline', 'bad');
        setBadge(bpConn, 'bandpower: offline', 'bad');
        setBadge(artConn, 'artifact: offline', 'bad');
        setBadge(metaConn, 'meta: offline', 'bad');
        setBadge(stateConn, 'state: offline', 'bad');
        console.error(err);
      });
  }

  start();
})();
