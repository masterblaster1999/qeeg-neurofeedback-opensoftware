(() => {
  'use strict';

  const TOKEN = (window.location.search.match(/(?:\?|&)token=([^&]+)/) || [])[1] || '';

  // If the token is present in the URL, drop it from the address bar.
  // The server sets a SameSite=Strict cookie so reloads can still authenticate without keeping
  // the token in browser history.
  if (TOKEN && window.history && window.history.replaceState) {
    try {
      const clean = window.location.pathname + window.location.hash;
      window.history.replaceState(null, '', clean);
    } catch (e) {
      // ignore
    }
  }
  function withToken(url){
    // Prefer cookie-based auth when the token is not in the URL.
    if(!TOKEN) return url;
    if(/(?:^|[?&])token=/.test(url)) return url;
    const sep = (url.indexOf('?') >= 0) ? '&' : '?';
    return url + sep + 'token=' + encodeURIComponent(TOKEN);
  }

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

  const downloadsDetails = qs('downloadsDetails');
  const bundleDownload = qs('bundleDownload');
  const filesRefresh = qs('filesRefresh');
  const filesList = qs('filesList');
  const filesHint = qs('filesHint');

  const reportsDetails = qs('reportsDetails');
  const reportsGenNf = qs('reportsGenNf');
  const reportsGenDash = qs('reportsGenDash');
  const reportsGenBundle = qs('reportsGenBundle');
  const reportsForce = qs('reportsForce');
  const reportsRefresh = qs('reportsRefresh');
  const reportsStatus = qs('reportsStatus');
  const reportsList = qs('reportsList');

  const winSel = qs('winSel');
  const pauseBtn = qs('pauseBtn');

  const nfStats = qs('nfStats');
  const canvas = qs('nfChart');
  const ctx = canvas.getContext('2d');

  const bandSel = qs('bandSel');
  const chSel = qs('chSel');
  const xformSel = qs('xformSel');
  const mapSel = qs('mapSel');
  const topoSrcSel = qs('topoSrcSel');
  const nfModeSel = qs('nfModeSel');
  const scaleSel = qs('scaleSel');
  const lblSel = qs('lblSel');
  const vminEl = qs('vmin');
  const vmaxEl = qs('vmax');
  const hmVminEl = qs('hmVmin');
  const hmVmaxEl = qs('hmVmax');

  const topoCanvas = qs('topoCanvas');
  const topoCtx = topoCanvas.getContext('2d');
  const bpCanvas = qs('bpChart');
  const bpCtx = bpCanvas.getContext('2d');
  const bpHeatCanvas = qs('bpHeatmap');
  const bpHeatCtx = bpHeatCanvas ? bpHeatCanvas.getContext('2d') : null;

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
    serverMeta: null,
    nf: {frames: [], lastT: -Infinity},
    bp: {meta: null, frames: [], lastT: -Infinity, rollingMin: null, rollingMax: null, heatRollingMin: null, heatRollingMax: null, tmpCanvas: null, tmpW: 0, tmpH: 0, electrodesPx: [], baseline: null, reference: null, baselineSec: 10.0, topomapCfg: null, cliTopomap: {etag: null, url: null, img: null, inflight: false, lastOk_utc: 0}},
    art: {frames: [], latest: null, lastT: -Infinity},
  };

  let runMetaETag = null;
  let statsTimer = null;
  let topoPollTimer = null;

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

  function fmtBytes(n){
    if(n===null || n===undefined || !Number.isFinite(n)) return '—';
    const units = ['B','KiB','MiB','GiB','TiB'];
    let v = Math.max(0, n);
    let u = 0;
    while(v >= 1024 && u < units.length-1){ v /= 1024; u++; }
    if(u === 0) return `${Math.round(v)} ${units[u]}`;
    const digs = (v >= 10) ? 1 : 2;
    return `${v.toFixed(digs)} ${units[u]}`;
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
    const add = (label, st, optional=false) => {
      const ok = !!(st && st.exists);
      if(!ok && !optional) anyMissing = true;
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
    add('artifact_gate_timeseries.csv', fs.artifact_gate_timeseries, true);
    add('nf_topomap_latest.bmp', fs.nf_topomap_latest, true);

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
          'fs_hz','window_seconds','update_seconds','baseline_seconds','baseline_quantile_used','target_reward_rate','artifact_gate',
          'topomap_latest','topomap_mode','topomap_band','topomap_every','topomap_grid','topomap_annotate','topomap_vmin','topomap_vmax',
          'qc_bad_channel_count','qc_bad_channels','biotrace_ui','export_derived_events'
        ];
        renderKvGrid(runMetaKv, rm.summary, order);
        // Cache baseline duration for client-side z-scoring.
        const bs = rm.summary.baseline_seconds;
        if(typeof bs === 'number' && Number.isFinite(bs) && bs > 0){ state.bp.baselineSec = bs; }
        // Try to load server-computed baseline (if available) so zbase works across reloads.
        if(typeof bs === 'number' && Number.isFinite(bs) && bs > 0){
          if(!state.bp.baseline || !state.bp.baseline.ready){ loadBaseline(); }
        }
        // Cache topomap config (if qeeg_nf_cli is writing nf_topomap_latest.bmp).
        state.bp.topomapCfg = {
          latest: rm.summary.topomap_latest,
          band: rm.summary.topomap_band,
          mode: rm.summary.topomap_mode,
          every: rm.summary.topomap_every,
          grid: rm.summary.topomap_grid,
          annotate: rm.summary.topomap_annotate,
          vmin: rm.summary.topomap_vmin,
          vmax: rm.summary.topomap_vmax,
        };
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
    if(runMetaDownload){
      runMetaDownload.href = withToken('/api/run_meta?format=raw');
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

  function topoSourceSel(){
    return (topoSrcSel && topoSrcSel.value) ? topoSrcSel.value : 'auto';
  }

  function resolveTopoSource(fullMeta){
    const sel = topoSourceSel();
    if(sel === 'browser') return 'browser';
    if(sel === 'cli') return 'cli';
    // auto: use CLI-rendered BMP if present; otherwise fallback to browser-rendered topomap
    const fs = (fullMeta && fullMeta.files_stat) ? fullMeta.files_stat : null;
    const st = fs ? (fs.nf_topomap_latest || fs.topomap_latest || null) : null;
    if(st && st.exists && (st.size_bytes || 0) > 0) return 'cli';
    return 'browser';
  }

  function updateTopoPolling(){
    const want = resolveTopoSource(state.serverMeta);
    const enabled = (want === 'cli') && !state.paused;
    if(!enabled){
      if(topoPollTimer){
        clearInterval(topoPollTimer);
        topoPollTimer = null;
      }
      return;
    }
    if(!topoPollTimer){
      topoPollTimer = setInterval(() => { pollCliTopomapOnce(); }, 500);
    }
    // kick immediately
    pollCliTopomapOnce();
  }

  async function pollCliTopomapOnce(){
    const cli = state.bp.cliTopomap || (state.bp.cliTopomap = {etag:null,url:null,img:null,inflight:false,lastOk_utc:0});
    if(cli.inflight) return;
    cli.inflight = true;
    try{
      const url = withToken('/api/topomap_latest');
      const headers = {};
      if(cli.etag) headers['If-None-Match'] = cli.etag;
      const r = await fetch(url, {headers, cache: 'no-cache'});
      if(r.status === 304){ return; }
      if(!r.ok){ return; }
      const et = r.headers.get('ETag');
      const blob = await r.blob();
      if(!blob || blob.size <= 0) return;
      const objUrl = URL.createObjectURL(blob);
      const img = new Image();
      img.onload = () => {
        if(cli.url) URL.revokeObjectURL(cli.url);
        cli.url = objUrl;
        cli.img = img;
        if(et) cli.etag = et;
        cli.lastOk_utc = Date.now()/1000;
        dirtyBp = true;
        scheduleRender();
      };
      img.onerror = () => { try{ URL.revokeObjectURL(objUrl); }catch(e){} };
      img.src = objUrl;
    }catch(e){
      // ignore
    }finally{
      cli.inflight = false;
    }
  }

  function ensureBaselineStats(nCols){
    const b = state.bp.baseline;
    if(b && b.nCols === nCols) return;
    const samples = new Array(nCols);
    for(let i=0;i<nCols;i++) samples[i] = [];
    state.bp.baseline = {
      method: 'median_mad',
      nCols,
      samples,
      median: new Array(nCols).fill(NaN),
      scale: new Array(nCols).fill(NaN),
      finalized: false,
      ready: false,
      nFrames: 0,
    };
  }

  function _median(arr){
    if(!Array.isArray(arr) || !arr.length) return NaN;
    const tmp = arr.slice().sort((a,b)=>a-b);
    const m = Math.floor(tmp.length/2);
    if(tmp.length % 2) return tmp[m];
    return 0.5*(tmp[m-1] + tmp[m]);
  }

  function _meanStd(arr){
    let n=0, mean=0, m2=0;
    for(const x of arr){
      if(x === null || x === undefined || !Number.isFinite(x)) continue;
      n++;
      const d = x - mean;
      mean += d / n;
      const d2 = x - mean;
      m2 += d * d2;
    }
    if(n < 2) return {mean: mean, sd: NaN};
    const var_ = m2 / Math.max(1, (n - 1));
    const sd = Math.sqrt(var_);
    return {mean, sd};
  }

  function baselineUpdate(vals){
    if(!Array.isArray(vals)) return;
    ensureBaselineStats(vals.length);
    const b = state.bp.baseline;
    if(!b || b.finalized) return;
    b.nFrames = (b.nFrames||0) + 1;
    for(let i=0;i<vals.length;i++){
      const x = vals[i];
      if(x === null || x === undefined || !Number.isFinite(x)) continue;
      b.samples[i].push(x);
    }
  }

  function finalizeBaselineStats(){
    const b = state.bp.baseline;
    if(!b || b.finalized) return;
    b.finalized = true;

    // MAD -> SD scaling constant under a Normal assumption.
    const MAD_TO_SD = 1.4826;

    let ok = 0;
    for(let i=0;i<b.nCols;i++){
      const samp = b.samples[i];
      if(!samp || samp.length < 5) continue;
      const med = _median(samp);
      if(!Number.isFinite(med)) continue;
      const dev = samp.map(v => Math.abs(v - med));
      const mad = _median(dev);
      let sc = mad * MAD_TO_SD;
      if(!(sc > 0) || !Number.isFinite(sc)){
        // Fallback to sample standard deviation if MAD collapses.
        sc = _meanStd(samp).sd;
      }
      if(sc > 0 && Number.isFinite(sc)){
        b.median[i] = med;
        b.scale[i] = sc;
        ok++;
      }
    }

    b.ready = ok > 0;
    // Free raw samples to keep memory bounded.
    b.samples = null;
  }

  function baselineZ(i, x){
    const b = state.bp.baseline;
    if(!b || !b.ready || !b.scale || i<0 || i>=b.scale.length) return null;
    const med = b.median[i];
    const sc = b.scale[i];
    if(!Number.isFinite(med) || !Number.isFinite(sc) || !(sc > 0)) return null;
    return (x - med) / sc;
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
    try{
      const r = await fetch(withToken('/api/reference'));
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

  async function loadBaseline(){
    if(state.bp._baselineInFlight) return;
    state.bp._baselineInFlight = true;
    try{
      const bs = (typeof state.bp.baselineSec === 'number' && Number.isFinite(state.bp.baselineSec) && state.bp.baselineSec > 0)
        ? state.bp.baselineSec : null;
      const bsQ = bs ? `?baseline_sec=${encodeURIComponent(String(bs))}` : '';
      const r = await fetch(withToken(`/api/baseline${bsQ}`));
      if(!r.ok) return;
      const obj = await r.json();
      const aligned = obj && obj.aligned ? obj.aligned : null;
      const base = obj && obj.baseline ? obj.baseline : null;
      if(aligned && Array.isArray(aligned.median) && Array.isArray(aligned.scale)){
        const nCols = aligned.median.length;
        const okCols = aligned.ok_cols || 0;
        const complete = !!(base && base.complete);
        const bsSrv = (base && typeof base.seconds === 'number' && Number.isFinite(base.seconds)) ? base.seconds : null;
        if(bsSrv && bsSrv > 0){ state.bp.baselineSec = bsSrv; }
        if(nCols > 0 && okCols > 0 && complete){
          state.bp.baseline = {
            method: (base && base.method) ? base.method : 'median_mad',
            nCols,
            samples: null,
            median: aligned.median,
            scale: aligned.scale,
            present: aligned.present || null,
            finalized: true,
            ready: true,
            nFrames: (base && typeof base.frames_used === 'number') ? base.frames_used : 0,
            source: 'server',
            baselineSec: bsSrv,
          };
          dirtyBp = true;
          scheduleRender();
        }
      }
    }catch(e){} finally{ state.bp._baselineInFlight = false; }
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
      if(f.metric_raw!==null && f.metric_raw!==undefined) add('metric_raw', fmt(f.metric_raw));
      if(f.threshold_desired!==null && f.threshold_desired!==undefined) add('thr_desired', fmt(f.threshold_desired));
      if(f.feedback_raw!==null && f.feedback_raw!==undefined) add('feedback_raw', fmt(f.feedback_raw));
      if(f.reward_value!==null && f.reward_value!==undefined) add('reward_value', fmt(f.reward_value));
      if(f.raw_reward!==null && f.raw_reward!==undefined) add('raw_reward', String(f.raw_reward));
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

// Shade artifact periods on the bandpower plot (requires ready==1 when available).
const arts = state.art.frames;
if(Array.isArray(arts) && arts.length){
  // Find the last artifact frame with t <= tMin.
  let lo=0, hi=arts.length-1, idx0=-1;
  while(lo<=hi){
    const mid = (lo+hi)>>1;
    const mt = (arts[mid] && typeof arts[mid].t === 'number') ? arts[mid].t : null;
    if(mt === null || !Number.isFinite(mt)){ lo = mid+1; continue; }
    if(mt <= tMin){ idx0 = mid; lo = mid+1; } else { hi = mid-1; }
  }
  if(idx0 < 0) idx0 = 0;

  bpCtx.fillStyle = 'rgba(255, 92, 92, 0.14)';

  let curT = tMin;
  let cur = arts[idx0];
  let curReady = (cur && cur.ready !== null && cur.ready !== undefined) ? !!cur.ready : true;
  let curBad = !!(cur && cur.bad);
  curBad = curReady && curBad;

  for(let i=idx0+1; i<arts.length; i++){
    const a = arts[i];
    if(!a || typeof a.t !== 'number' || !Number.isFinite(a.t)) continue;
    if(a.t < tMin) continue;
    if(a.t > tNow) break;

    if(curBad){
      const x0 = x(curT);
      const x1 = x(a.t);
      const ww = x1 - x0;
      if(ww > 0.5){
        bpCtx.fillRect(x0, 10, ww, h-30);
      }
    }

    curT = a.t;
    curReady = (a.ready !== null && a.ready !== undefined) ? !!a.ready : true;
    curBad = !!a.bad;
    curBad = curReady && curBad;
  }

  if(curBad){
    const x0 = x(curT);
    const x1 = x(tNow);
    const ww = x1 - x0;
    if(ww > 0.5){
      bpCtx.fillRect(x0, 10, ww, h-30);
    }
  }
}


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

  function lerp(a, b, t){ return a + (b-a)*t; }

  function rgbLerp(c0, c1, t){
    return [
      Math.round(lerp(c0[0], c1[0], t)),
      Math.round(lerp(c0[1], c1[1], t)),
      Math.round(lerp(c0[2], c1[2], t)),
    ];
  }

  function valueToRgb(v, vmin, vmax){
    if(v===null || v===undefined || !Number.isFinite(v) || !(vmax>vmin)) return [42,52,64];
    let t = (v - vmin) / (vmax - vmin);
    if(t<0) t=0; if(t>1) t=1;

    // Diverging map when the range crosses 0 (useful for z-scores and log/db values).
    if(vmin < 0 && vmax > 0){
      const mid = (0 - vmin) / (vmax - vmin);
      const blue = [59, 76, 192];     // #3b4cc0
      const neutral = [247, 247, 247];
      const red = [180, 4, 38];      // #b40426
      if(t <= mid){
        const tt = (mid > 1e-9) ? (t / mid) : 0;
        return rgbLerp(blue, neutral, tt);
      }
      const tt = ((1 - mid) > 1e-9) ? ((t - mid) / (1 - mid)) : 0;
      return rgbLerp(neutral, red, tt);
    }

    // Sequential fallback.
    const hue = (1 - t) * 240;
    return hslToRgb(hue, 0.90, 0.55);
  }

  function drawTopoCli(){
    resizeCanvasTo(topoCanvas);
    const w = topoCanvas.width, h = topoCanvas.height;
    topoCtx.clearRect(0,0,w,h);
    topoCtx.fillStyle = '#071018';
    topoCtx.fillRect(0,0,w,h);

    // Legend values in CLI mode come from nf_run_meta.json (if available).
    const cfg = state.bp.topomapCfg || null;
    const mode = (cfg && typeof cfg.mode === 'string') ? cfg.mode : '';
    let vmin = (cfg && Number.isFinite(cfg.vmin)) ? cfg.vmin : null;
    let vmax = (cfg && Number.isFinite(cfg.vmax)) ? cfg.vmax : null;

    // For z-score maps, qeeg_nf_cli defaults to roughly [-3, 3] unless overridden.
    if((vmin === null || vmax === null) && (mode === 'zbase' || mode === 'zref')){
      if(vmin === null) vmin = -3;
      if(vmax === null) vmax = 3;
    }

    const vminStr = (vmin === null) ? 'auto' : vmin.toFixed(3);
    const vmaxStr = (vmax === null) ? 'auto' : vmax.toFixed(3);
    vminEl.textContent = `min: ${vminStr}`;
    vmaxEl.textContent = `max: ${vmaxStr}`;

    // In CLI-BMP mode the click-to-select overlay isn't aligned; disable it.
    state.bp.electrodesPx = [];

    const cli = state.bp.cliTopomap;
    const img = (cli && cli.img) ? cli.img : null;
    const dpr = window.devicePixelRatio || 1;

    if(img && img.naturalWidth > 0 && img.naturalHeight > 0){
      const iw = img.naturalWidth;
      const ih = img.naturalHeight;
      const scale = Math.min(w/iw, h/ih);
      const dw = iw * scale;
      const dh = ih * scale;
      const dx = (w - dw) / 2;
      const dy = (h - dh) / 2;
      topoCtx.imageSmoothingEnabled = true;
      topoCtx.drawImage(img, dx, dy, dw, dh);

      topoCtx.fillStyle = 'rgba(255,255,255,0.78)';
      topoCtx.font = `${Math.round(11*dpr)}px system-ui`;
      const label = mode ? `CLI topomap (${mode})` : 'CLI topomap';
      topoCtx.fillText(label, 14*dpr, 18*dpr);
    } else {
      topoCtx.fillStyle = '#9fb0c3';
      topoCtx.font = `${Math.round(12*dpr)}px system-ui`;
      topoCtx.fillText('Waiting for nf_topomap_latest.bmp …', 14*dpr, 24*dpr);
    }
  }

  function drawTopo(){
    const topoSrc = resolveTopoSource(state.serverMeta);
    if(topoSrc === 'cli'){
      drawTopoCli();
      return;
    }

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

    // If an artifact is active for the latest timestamp, hold the last clean frame.
    let useFrame = latest;
    let holdingArtifact = false;
    if(latest && typeof latest.t === 'number' && Number.isFinite(latest.t)){
      const aNow = artifactAtTime(latest.t);
      const aReady = (aNow && aNow.ready !== null && aNow.ready !== undefined) ? !!aNow.ready : true;
      holdingArtifact = !!(aNow && aReady && aNow.bad);
      if(holdingArtifact && state.bp.lastClean){
        useFrame = state.bp.lastClean;
      }
    }

    let vals = null;
    if(useFrame && Array.isArray(useFrame.values) && useFrame.values.length >= (meta.bands.length*nCh)){
      vals = [];
      for(let c=0;c<nCh;c++){
        const col = bIdx*nCh + c;
        vals.push(bpDisplayValue(col, useFrame.values[col]));
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

    // In z-score modes, default to a symmetric fixed range in auto-scale for interpretability.
    const mm = mapMode();
    if((mm === 'zbase' || mm === 'zref') && scaleSel.value !== 'fixed'){
      vmin = -3;
      vmax = 3;
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

    if(holdingArtifact){
      topoCtx.fillStyle = 'rgba(255, 92, 92, 0.85)';
      topoCtx.font = `${Math.round(12*dpr)}px system-ui`;
      topoCtx.textAlign = 'left';
      topoCtx.textBaseline = 'top';
      topoCtx.fillText('artifact hold', 12*dpr, 12*dpr);
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

  function drawBandpowerHeatmap(){
    if(!bpHeatCanvas || !bpHeatCtx) return;

    const meta = state.bp.meta;
    resizeCanvasTo(bpHeatCanvas);
    const w = bpHeatCanvas.width, h = bpHeatCanvas.height;
    bpHeatCtx.clearRect(0,0,w,h);
    bpHeatCtx.fillStyle = '#071018';
    bpHeatCtx.fillRect(0,0,w,h);

    const dpr = window.devicePixelRatio || 1;

    if(!meta || !state.bp.frames.length){
      bpHeatCtx.fillStyle = '#9fb0c3';
      bpHeatCtx.font = `${Math.round(12*dpr)}px system-ui`;
      bpHeatCtx.fillText('Waiting for bandpower_timeseries.csv …', 14*dpr, 24*dpr);
      if(hmVminEl) hmVminEl.textContent = 'min: —';
      if(hmVmaxEl) hmVmaxEl.textContent = 'max: —';
      return;
    }

    const bands = meta.bands || [];
    const channels = meta.channels || [];
    const nBands = bands.length;
    const nCh = channels.length;
    if(!nBands || !nCh){
      bpHeatCtx.fillStyle = '#9fb0c3';
      bpHeatCtx.font = `${Math.round(12*dpr)}px system-ui`;
      bpHeatCtx.fillText('No bandpower metadata', 14*dpr, 24*dpr);
      if(hmVminEl) hmVminEl.textContent = 'min: —';
      if(hmVmaxEl) hmVmaxEl.textContent = 'max: —';
      return;
    }

    let chName = (chSel && chSel.value) ? chSel.value : (channels[0] || '');
    let cIdx = channels.indexOf(chName);
    if(cIdx < 0){
      cIdx = 0;
      chName = channels[0] || '';
      if(chSel && chName) chSel.value = chName;
    }

    const frames = state.bp.frames;
    const last = frames[frames.length-1];
    const tNow = (last && typeof last.t === 'number' && Number.isFinite(last.t)) ? last.t : 0;
    const tStart = (frames[0] && typeof frames[0].t === 'number' && Number.isFinite(frames[0].t)) ? frames[0].t : 0;
    let tMin = Math.max(tStart, tNow - state.winSec);
    if(!(tNow > tMin)) tMin = tNow - Math.max(1, state.winSec);

    const need = nBands * nCh;
    const vis = [];
    for(const f of frames){
      if(!f || typeof f !== 'object') continue;
      const t = f.t;
      if(t===null || t===undefined || !Number.isFinite(t)) continue;
      if(t < tMin) continue;
      if(!Array.isArray(f.values) || f.values.length < need) continue;
      vis.push(f);
    }

    if(vis.length < 2){
      bpHeatCtx.fillStyle = '#9fb0c3';
      bpHeatCtx.font = `${Math.round(12*dpr)}px system-ui`;
      bpHeatCtx.fillText('Waiting for bandpower frames …', 14*dpr, 24*dpr);
      if(hmVminEl) hmVminEl.textContent = 'min: —';
      if(hmVmaxEl) hmVmaxEl.textContent = 'max: —';
      return;
    }

    const left = 68*dpr;
    const right = 10*dpr;
    const top = 12*dpr;
    const bottom = 24*dpr;
    const plotW = Math.max(1, w - left - right);
    const plotH = Math.max(1, h - top - bottom);
    const rowH = plotH / nBands;

    // Decimate to keep rendering bounded (<=1 stripe / pixel).
    const maxCols = Math.max(50, Math.floor(plotW));
    const step = Math.max(1, Math.ceil(vis.length / maxCols));

    // Scale.
    let vmin = Infinity, vmax = -Infinity;
    const mm = mapMode();
    if(mm === 'zbase' || mm === 'zref'){
      vmin = -3;
      vmax = 3;
    } else {
      for(let i=0;i<vis.length;i+=step){
        const vals = vis[i].values;
        for(let b=0;b<nBands;b++){
          const col = b*nCh + cIdx;
          const v = bpDisplayValue(col, vals[col]);
          if(v!==null && v!==undefined && Number.isFinite(v)){
            vmin = Math.min(vmin, v);
            vmax = Math.max(vmax, v);
          }
        }
      }
      if(!(vmax > vmin)) { vmin = 0; vmax = 1; }
      const pad = 0.02*(vmax - vmin);
      vmin -= pad;
      vmax += pad;
    }

    // Rolling fixed scale (raw mode only); z-score modes keep a fixed symmetric range.
    if(scaleSel && scaleSel.value === 'fixed' && mm === 'raw'){
      if(state.bp.heatRollingMin===null || state.bp.heatRollingMax===null){
        state.bp.heatRollingMin = vmin;
        state.bp.heatRollingMax = vmax;
      } else {
        state.bp.heatRollingMin = 0.98*state.bp.heatRollingMin + 0.02*vmin;
        state.bp.heatRollingMax = 0.98*state.bp.heatRollingMax + 0.02*vmax;
      }
      vmin = state.bp.heatRollingMin;
      vmax = state.bp.heatRollingMax;
    } else if(scaleSel && scaleSel.value === 'fixed' && (mm === 'zbase' || mm === 'zref')){
      vmin = -3;
      vmax = 3;
    } else {
      state.bp.heatRollingMin = null;
      state.bp.heatRollingMax = null;
    }

    if(hmVminEl) hmVminEl.textContent = `min: ${Number.isFinite(vmin)?vmin.toFixed(3):'—'}`;
    if(hmVmaxEl) hmVmaxEl.textContent = `max: ${Number.isFinite(vmax)?vmax.toFixed(3):'—'}`;

    // Grid.
    bpHeatCtx.strokeStyle = 'rgba(255,255,255,0.06)';
    bpHeatCtx.lineWidth = 1;
    for(let i=0;i<=nBands;i++){
      const yy = top + i*rowH;
      bpHeatCtx.beginPath();
      bpHeatCtx.moveTo(left, yy);
      bpHeatCtx.lineTo(left + plotW, yy);
      bpHeatCtx.stroke();
    }
    for(let i=0;i<=4;i++){
      const xx = left + i*plotW/4;
      bpHeatCtx.beginPath();
      bpHeatCtx.moveTo(xx, top);
      bpHeatCtx.lineTo(xx, top + plotH);
      bpHeatCtx.stroke();
    }

    const tSpan = (tNow - tMin) || 1;

    // Heat stripes.
    for(let i=0;i<vis.length;i+=step){
      const f = vis[i];
      const t = f.t;
      const t2 = (i+step < vis.length) ? vis[i+step].t : tNow;
      const x1 = left + (t - tMin)/tSpan * plotW;
      const x2 = left + (t2 - tMin)/tSpan * plotW;
      const bw = Math.max(1, Math.ceil(x2 - x1));
      const vals = f.values;

      for(let b=0;b<nBands;b++){
        const col = b*nCh + cIdx;
        const vv = bpDisplayValue(col, vals[col]);
        const y0 = top + (nBands - 1 - b)*rowH;
        const [r,g,bb] = valueToRgb(vv, vmin, vmax);
        bpHeatCtx.fillStyle = `rgb(${r},${g},${bb})`;
        bpHeatCtx.fillRect(x1, y0, bw, Math.ceil(rowH));
      }

      const aAt = artifactAtTime(t);
      const aReady = (aAt && aAt.ready !== null && aAt.ready !== undefined) ? !!aAt.ready : true;
      const artifactHit = !!(aAt && aReady && aAt.bad);
      if(artifactHit){
        bpHeatCtx.fillStyle = 'rgba(255, 92, 92, 0.18)';
        bpHeatCtx.fillRect(x1, top, bw, plotH);
      }
    }

    // Border.
    bpHeatCtx.strokeStyle = 'rgba(255,255,255,0.25)';
    bpHeatCtx.lineWidth = 1.5*dpr;
    bpHeatCtx.strokeRect(left, top, plotW, plotH);

    // Band labels (low frequency at bottom).
    bpHeatCtx.fillStyle = '#9fb0c3';
    bpHeatCtx.font = `${Math.round(11*dpr)}px system-ui`;
    bpHeatCtx.textAlign = 'right';
    bpHeatCtx.textBaseline = 'middle';
    for(let b=0;b<nBands;b++){
      const ym = top + (nBands - 1 - b + 0.5)*rowH;
      bpHeatCtx.fillText(bands[b], left - 8*dpr, ym);
    }

    // Title + time axis.
    const xm = (xformSel && xformSel.value) ? xformSel.value : 'linear';
    let title = '';
    if(mm === 'raw'){
      const xLabel = (xm && xm !== 'linear') ? ` (${xm})` : '';
      title = `${chName}${xLabel}`;
    } else if(mm === 'zbase'){
      title = `${chName} z (baseline)`;
    } else if(mm === 'zref'){
      title = `${chName} z (reference)`;
    } else {
      title = chName;
    }

    bpHeatCtx.fillStyle = '#9fb0c3';
    bpHeatCtx.textAlign = 'left';
    bpHeatCtx.textBaseline = 'top';
    bpHeatCtx.font = `${Math.round(11*dpr)}px system-ui`;
    bpHeatCtx.fillText(title, left, 6*dpr);

    bpHeatCtx.textBaseline = 'alphabetic';
    bpHeatCtx.fillText(`t: ${tMin.toFixed(1)}–${tNow.toFixed(1)}s`, left, h - 6*dpr);
  }

  function connectSSE(path, badgeEl, label){
    if(!window.EventSource){
      setBadge(badgeEl, `${label}: no EventSource`, 'warn');
      return null;
    }
    const url = withToken(path);
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
    const q = qp.length ? ('?' + qp.join('&')) : '';
    const url = withToken(`/api/sse/stream${q}`);
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
      topo_source: (topoSrcSel && topoSrcSel.value) ? topoSrcSel.value : 'auto',
      client_id: CLIENT_ID,
    };
  }

  let pushTimer = null;
  function schedulePushUiState(){
    if(state.applyingRemote) return;
    if(pushTimer) clearTimeout(pushTimer);
    pushTimer = setTimeout(() => {
      pushTimer = null;
      const body = uiStateFromControls();
      fetch(withToken('/api/state'), {
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
      if(typeof st.topo_source === 'string'){
        if(topoSrcSel && ['auto','browser','cli'].includes(st.topo_source)) topoSrcSel.value = st.topo_source;
      }
      // band/channel depend on meta; stash if not yet available.
      state.pendingUi = state.pendingUi || {};
      if(typeof st.band === 'string' && st.band) state.pendingUi.band = st.band;
      if(typeof st.channel === 'string' && st.channel) state.pendingUi.channel = st.channel;
      dirtyNf = true;
      dirtyBp = true;
      scheduleRender();
      updateTopoPolling();
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
    if(changed){ drawTopo(); drawBandpowerSeries(); drawBandpowerHeatmap(); }
  }

  // ---------------- start + streaming ----------------

  pauseBtn.onclick = () => {
    state.paused = !state.paused;
    pauseBtn.textContent = state.paused ? 'Resume' : 'Pause';
    schedulePushUiState();
    dirtyNf = true;
    dirtyBp = true;
    scheduleRender();
    updateTopoPolling();
  };

  winSel.onchange = () => {
    state.winSec = parseFloat(winSel.value||'60') || 60;
    schedulePushUiState();
    dirtyNf = true;
    dirtyBp = true;
    scheduleRender();
  };

  bandSel.onchange = () => { state.bp.heatRollingMin = null; state.bp.heatRollingMax = null; dirtyBp = true; scheduleRender(); schedulePushUiState(); };
  chSel.onchange = () => { state.bp.heatRollingMin = null; state.bp.heatRollingMax = null; dirtyBp = true; scheduleRender(); schedulePushUiState(); };
  lblSel.onchange = () => { dirtyBp = true; scheduleRender(); schedulePushUiState(); };
  xformSel.onchange = () => { state.bp.rollingMin = null; state.bp.rollingMax = null; state.bp.heatRollingMin = null; state.bp.heatRollingMax = null; dirtyBp = true; scheduleRender(); schedulePushUiState(); };
  if(topoSrcSel){
    topoSrcSel.onchange = () => { dirtyBp = true; scheduleRender(); schedulePushUiState(); updateTopoPolling(); };
  }
  if(mapSel){
    mapSel.onchange = () => {
      state.bp.rollingMin = null; state.bp.rollingMax = null;
      state.bp.heatRollingMin = null; state.bp.heatRollingMax = null;
      if(mapSel.value === 'zref' && !state.bp.reference){ loadReference(); }
      if(mapSel.value === 'zbase' && (!state.bp.baseline || !state.bp.baseline.ready)){ loadBaseline(); }
      dirtyBp = true; scheduleRender(); schedulePushUiState();
    };
  }
  if(nfModeSel){
    nfModeSel.onchange = () => { dirtyNf = true; updateStats(); scheduleRender(); schedulePushUiState(); };
  }
  scaleSel.onchange = () => { state.bp.rollingMin = null; state.bp.rollingMax = null; state.bp.heatRollingMin = null; state.bp.heatRollingMax = null; dirtyBp = true; scheduleRender(); schedulePushUiState(); };

  function applyMeta(meta){
    if(!meta || typeof meta !== 'object') return;
    state.serverMeta = meta;
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

    // Load server-computed baseline (if available) so zbase works across reloads.
    const bs = (state.bp.baselineSec || 0);
    if(bs > 0 && (!state.bp.baseline || !state.bp.baseline.ready)){ loadBaseline(); }

    // If enabled, poll CLI-rendered topomap BMP for display.
    updateTopoPolling();
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
      if(dirtyBp){ drawTopo(); drawBandpowerSeries(); drawBandpowerHeatmap(); dirtyBp = false; }
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
      updateTopoPolling();
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

  function artifactAtTime(t){
    const arr = state.art.frames;
    if(!arr || !arr.length || t===null || t===undefined || !Number.isFinite(t)) return null;
    let i = (state.art.cursor||0);
    if(i < 0) i = 0;
    if(i >= arr.length) i = arr.length-1;
    // If the cursor points into the future (e.g., after trimming), reset.
    if(arr[i] && typeof arr[i].t === 'number' && arr[i].t > t){
      i = 0;
    }
    while(i+1 < arr.length && typeof arr[i+1].t === 'number' && arr[i+1].t <= t){
      i++;
    }
    state.art.cursor = i;
    return arr[i] || null;
  }

  function handleBandpowerMsg(raw){
    if(state.paused) return;
    try{
      const un = unpackMsg(raw);
      if(un.reset){ state.bp.frames = []; state.bp.lastT = -Infinity; state.bp.rollingMin = null; state.bp.rollingMax = null; state.bp.heatRollingMin = null; state.bp.heatRollingMax = null; state.bp.baseline = null; state.bp.lastClean = null; }
      for(const f of un.frames){
        if(!f || typeof f !== 'object') continue;
        if(f.t !== null && f.t !== undefined && Number.isFinite(f.t)){
          if(f.t <= state.bp.lastT) continue;
          state.bp.lastT = f.t;
        }
        state.bp.frames.push(f);
        // Collect baseline stats (client-side) for z-score mapping.
        if(f && Array.isArray(f.values) && typeof f.t === 'number' && Number.isFinite(f.t)){
          const aAt = artifactAtTime(f.t);
          const aReady = (aAt && aAt.ready !== null && aAt.ready !== undefined) ? !!aAt.ready : true;
          const artifactHit = !!(aAt && aReady && aAt.bad);
          // Track last clean bandpower frame for artifact-hold topomap rendering.
          if(!artifactHit){
            state.bp.lastClean = f;
          }
          const bs = (state.bp.baselineSec || 0);
          if(!artifactHit && f.t <= bs) baselineUpdate(f.values);
          else if(state.bp.baseline && !state.bp.baseline.finalized && f.t > bs) finalizeBaselineStats();
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
      if(un.reset){ state.art.frames = []; state.art.lastT = -Infinity; state.art.latest = null; state.art.cursor = 0; }
      for(const f of un.frames){
        if(!f || typeof f !== 'object') continue;
        if(f.t !== null && f.t !== undefined && Number.isFinite(f.t)){
          if(f.t <= state.art.lastT) continue;
          state.art.lastT = f.t;
        }
        state.art.frames.push(f);
        state.art.latest = f;
      }
      if(state.art.frames.length > 40000){
        state.art.frames.splice(0, state.art.frames.length-40000);
        state.art.cursor = 0;
      }
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
      updateTopoPolling();
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
          state.bp.heatRollingMin = null;
          state.bp.heatRollingMax = null;
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
        fetch(withToken('/api/meta'))
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
      qp.push(`topics=${encodeURIComponent(topics.join(','))}`);
      qp.push(`wait=1.0`);
      qp.push(`limit=2500`);
      qp.push(`nf=${encodeURIComponent(String(cur.nf||0))}`);
      qp.push(`bandpower=${encodeURIComponent(String(cur.bandpower||0))}`);
      qp.push(`artifact=${encodeURIComponent(String(cur.artifact||0))}`);
      qp.push(`meta=${encodeURIComponent(String(cur.meta||0))}`);
      qp.push(`state=${encodeURIComponent(String(cur.state||0))}`);
      return withToken(`/api/snapshot?${qp.join('&')}`);
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

  let filesLastLoad_utc = 0;

  function clearEl(el){
    if(!el) return;
    while(el.firstChild) el.removeChild(el.firstChild);
  }

  function renderFilesList(obj){
    if(!filesList) return;
    clearEl(filesList);

    const files = (obj && Array.isArray(obj.files)) ? obj.files : [];
    const now = Date.now()/1000;

    if(filesHint){
      if(files.length === 0){
        filesHint.textContent = 'No downloadable files found in outdir.';
      } else {
        filesHint.textContent = '';
      }
    }

    for(const f of files){
      const name = (f && f.name) ? String(f.name) : '';
      if(!name) continue;

      const row = document.createElement('div');
      row.className = 'fileRow';

      const left = document.createElement('div');
      left.style.display = 'flex';
      left.style.gap = '8px';
      left.style.flexWrap = 'wrap';
      left.style.alignItems = 'center';

      const a = document.createElement('a');
      a.textContent = name;
      a.target = '_blank';
      a.rel = 'noopener';

      const urlView = (f && f.url) ? String(f.url) : (`/api/file?name=${encodeURIComponent(name)}`);
      const urlDownload = (f && f.download_url) ? String(f.download_url) : (`/api/file?name=${encodeURIComponent(name)}&download=1`);
      const isHtml = name.toLowerCase().endsWith('.html');
      const primary = isHtml ? urlView : urlDownload;
      const href = withToken(primary);

      const downloadable = (f && typeof f.downloadable === 'boolean') ? f.downloadable : true;
      if(downloadable){
        a.href = href;
      } else {
        a.href = '#';
        a.style.opacity = '0.6';
        a.addEventListener('click', (ev)=>ev.preventDefault());
      }
      left.appendChild(a);

      // For HTML reports, provide an "open" (view) link plus a separate "download" chip.
      if(downloadable && isHtml){
        const dl = document.createElement('a');
        dl.textContent = 'download';
        dl.href = withToken(urlDownload);
        dl.target = '_blank';
        dl.rel = 'noopener';
        dl.className = 'fileTag';
        left.appendChild(dl);
      }

      row.appendChild(left);

      const meta = document.createElement('span');
      meta.className = 'fileMeta';
      const st = (f && f.stat && typeof f.stat === 'object') ? f.stat : {};
      const size = (st && Number.isFinite(st.size_bytes)) ? st.size_bytes : null;
      const mtime = (st && Number.isFinite(st.mtime_utc)) ? st.mtime_utc : null;
      const parts = [];
      if(size !== null) parts.push(fmtBytes(size));
      if(mtime !== null) parts.push('age ' + fmtAgeSec(now - mtime));
      if(f && f.category) parts.push(String(f.category));
      if(downloadable === false) parts.push('too large');
      meta.textContent = parts.join(' · ');
      row.appendChild(meta);

      filesList.appendChild(row);
    }
  }

  async function loadFiles(force=false){
    if(!filesList) return;
    const now = Date.now()/1000;
    if(!force && (now - filesLastLoad_utc) < 1.0 && filesList.childElementCount > 0){
      return;
    }
    filesLastLoad_utc = now;
    if(filesHint) filesHint.textContent = 'Loading…';
    try{
      const r = await fetch(withToken('/api/files'), {cache: 'no-store'});
      if(!r.ok){
        throw new Error(`HTTP ${r.status}`);
      }
      const obj = await r.json();
      renderFilesList(obj);
    }catch(e){
      if(filesHint) filesHint.textContent = 'Failed to load file list.';
    }
  }


  // ------------------------ reports ------------------------

  let reportsLastLoad_utc = 0;

  function setReportsStatus(msg){
    if(!reportsStatus) return;
    reportsStatus.textContent = msg || '';
  }

  function renderReports(obj){
    if(!reportsList) return;
    clearEl(reportsList);

    const kinds = (obj && Array.isArray(obj.kinds)) ? obj.kinds : [];
    const artifacts = (obj && Array.isArray(obj.artifacts)) ? obj.artifacts : [];

    if(kinds.length === 0){
      setReportsStatus('No report generators advertised by this server.');
    } else {
      const notReady = [];
      for(const k of kinds){
        if(k && k.ready === false){
          const miss = Array.isArray(k.missing_inputs) ? k.missing_inputs.join(', ') : '';
          notReady.push(String(k.kind || '') + (miss ? (' (missing ' + miss + ')') : ''));
        }
      }
      if(notReady.length){
        setReportsStatus('Some report generators are not ready: ' + notReady.join(' · '));
      } else {
        setReportsStatus('Ready.');
      }
    }

    if(artifacts.length === 0){
      const empty = document.createElement('div');
      empty.className = 'small muted';
      empty.textContent = 'No report artifacts found yet. Generate one using the buttons above.';
      reportsList.appendChild(empty);
      return;
    }

    const now = Date.now()/1000;

    for(const f of artifacts){
      const name = (f && f.name) ? String(f.name) : '';
      if(!name) continue;

      const row = document.createElement('div');
      row.className = 'fileRow';

      const left = document.createElement('div');
      left.style.display = 'flex';
      left.style.gap = '8px';
      left.style.flexWrap = 'wrap';
      left.style.alignItems = 'center';

      const a = document.createElement('a');
      a.textContent = name;
      a.target = '_blank';
      a.rel = 'noopener';

      const urlView = (f && f.url) ? String(f.url) : (`/api/file?name=${encodeURIComponent(name)}`);
      const urlDownload = (f && f.download_url) ? String(f.download_url) : (`/api/file?name=${encodeURIComponent(name)}&download=1`);
      const isHtml = name.toLowerCase().endsWith('.html');
      const primary = isHtml ? urlView : urlDownload;

      const downloadable = (f && typeof f.downloadable === 'boolean') ? f.downloadable : true;
      if(downloadable){
        a.href = withToken(primary);
      } else {
        a.href = '#';
        a.style.opacity = '0.6';
        a.addEventListener('click', (ev)=>ev.preventDefault());
      }
      left.appendChild(a);

      if(downloadable && isHtml){
        const dl = document.createElement('a');
        dl.textContent = 'download';
        dl.href = withToken(urlDownload);
        dl.target = '_blank';
        dl.rel = 'noopener';
        dl.className = 'fileTag';
        left.appendChild(dl);
      }

      row.appendChild(left);

      const meta = document.createElement('span');
      meta.className = 'fileMeta';
      const st = (f && f.stat && typeof f.stat === 'object') ? f.stat : {};
      const size = (st && Number.isFinite(st.size_bytes)) ? st.size_bytes : null;
      const mtime = (st && Number.isFinite(st.mtime_utc)) ? st.mtime_utc : null;
      const parts = [];
      if(size !== null) parts.push(fmtBytes(size));
      if(mtime !== null) parts.push('age ' + fmtAgeSec(now - mtime));
      if(f && f.mime) parts.push(String(f.mime).split(';')[0]);
      meta.textContent = parts.join(' · ');
      row.appendChild(meta);

      reportsList.appendChild(row);
    }
  }

  async function loadReports(force=false){
    if(!reportsDetails) return;
    const now = Date.now()/1000;
    if(!force && (now - reportsLastLoad_utc) < 1.0 && reportsList && reportsList.childElementCount > 0){
      return;
    }
    reportsLastLoad_utc = now;
    setReportsStatus('Loading…');
    try{
      const r = await fetch(withToken('/api/reports'), {cache: 'no-store'});
      if(r.status === 404){
        // Older server: hide the entire panel.
        reportsDetails.style.display = 'none';
        return;
      }
      if(!r.ok){
        throw new Error(`HTTP ${r.status}`);
      }
      const obj = await r.json();
      renderReports(obj);
    }catch(e){
      setReportsStatus('Failed to load reports info.');
    }
  }

  function setReportsButtonsDisabled(disabled){
    const btns = [reportsGenNf, reportsGenDash, reportsGenBundle, reportsRefresh];
    for(const b of btns){
      if(b) b.disabled = !!disabled;
    }
  }

  async function generateReports(kinds){
    if(!Array.isArray(kinds)) kinds = [kinds];
    const force = !!(reportsForce && reportsForce.checked);
    setReportsButtonsDisabled(true);
    setReportsStatus('Generating…');
    try{
      const body = JSON.stringify({kinds: kinds, force: force});
      const r = await fetch(withToken('/api/reports'), {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: body,
      });
      if(!r.ok){
        const t = await r.text().catch(()=> '');
        throw new Error(`HTTP ${r.status} ${t}`);
      }
      const obj = await r.json();
      const results = (obj && Array.isArray(obj.results)) ? obj.results : [];
      const ok = results.filter(x => x && x.ok).length;
      const bad = results.length - ok;
      setReportsStatus(`Generated ${ok}/${results.length} report(s)` + (bad ? (` (${bad} failed)`) : '') + '.');
    }catch(e){
      setReportsStatus('Report generation failed: ' + String(e && e.message ? e.message : e));
    }finally{
      setReportsButtonsDisabled(false);
      loadReports(true);
      loadFiles(true);
    }
  }


  async function loadRunMeta(force=false){
    if(!runMetaRaw && !runMetaKv && !runMetaStatus) return;
    const url = withToken('/api/run_meta');
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

    if(runMetaDownload){
      runMetaDownload.href = withToken('/api/run_meta?format=raw');
    }
  }

  async function loadStatsOnce(){
    if(!serverStats && !statsKv) return;
    const url = withToken('/api/stats');
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
    // Wire up Session panel controls (safe no-ops if elements missing).
    if(runMetaDownload){
      runMetaDownload.href = withToken('/api/run_meta?format=raw');
    }
    if(runMetaRefresh){
      runMetaRefresh.addEventListener('click', ()=>{ loadRunMeta(true); });
    }
    if(runMetaDetails){
      runMetaDetails.addEventListener('toggle', ()=>{
        if(runMetaDetails.open) loadRunMeta(false);
      });
    }

    if(bundleDownload){
      bundleDownload.href = withToken('/api/bundle');
    }
    if(filesRefresh){
      filesRefresh.addEventListener('click', ()=>{ loadFiles(true); });
    }
    if(downloadsDetails){
      downloadsDetails.addEventListener('toggle', ()=>{
        if(downloadsDetails.open) loadFiles(false);
      });
    }
    // Prefetch once so the list is ready when the user opens the panel.
    loadFiles(false);

    // Reports panel (optional; server may not support this endpoint).
    if(reportsGenNf){
      reportsGenNf.addEventListener('click', ()=>generateReports(['nf_feedback']));
    }
    if(reportsGenDash){
      reportsGenDash.addEventListener('click', ()=>generateReports(['reports_dashboard']));
    }
    if(reportsGenBundle){
      reportsGenBundle.addEventListener('click', ()=>generateReports(['reports_bundle']));
    }
    if(reportsRefresh){
      reportsRefresh.addEventListener('click', ()=>loadReports(true));
    }
    if(reportsDetails){
      reportsDetails.addEventListener('toggle', ()=>{ if(reportsDetails.open) loadReports(false); });
    }
    // Prefetch once. If unsupported, loadReports hides the panel.
    loadReports(false);

    ensureUiBindings();
    markAllReconnect('…');

    // Load server state (best-effort) and apply early.
    fetch(withToken('/api/state'))
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
    fetch(withToken('/api/meta'))
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
        fetch(withToken('/api/config'))
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
