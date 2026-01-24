(function(){
  'use strict';
  var mEl = document.getElementById('metric');
  var tEl = document.getElementById('thr');
  var sEl = document.getElementById('state');
  var aEl = document.getElementById('age');
  var fsBtn = document.getElementById('fsBtn');
  var pauseBtn = document.getElementById('pauseBtn');

  function getToken(){
    var m = window.location.search.match(/(?:\?|&)token=([^&]+)/);
    return m ? decodeURIComponent(m[1]) : "";
  }
  var TOKEN = getToken();

  var CLIENT_ID_KEY = 'qeeg_rt_dashboard_client_id';
  function newClientId(){
    return Math.random().toString(36).slice(2) + '-' + (new Date()).getTime().toString(36);
  }
  var CLIENT_ID = null;
  try{
    CLIENT_ID = window.localStorage.getItem(CLIENT_ID_KEY);
  }catch(e){ CLIENT_ID = null; }
  if(!CLIENT_ID){
    CLIENT_ID = newClientId();
    try{ window.localStorage.setItem(CLIENT_ID_KEY, CLIENT_ID); }catch(e2){}
  }

  var paused = false;

  // ---------------- helpers ----------------

  function fmt(v){
    if(v===null || v===undefined || !isFinite(v)) return "—";
    var av = Math.abs(v);
    var d = (av >= 100) ? 1 : (av >= 10 ? 2 : 3);
    return v.toFixed(d);
  }

  function setBadge(cls, txt){
    if(!sEl) return;
    sEl.className = "badge " + cls;
    sEl.innerHTML = txt;
  }

  function setAge(age){
    if(!aEl) return;
    if(age===null || age===undefined || !isFinite(age)){
      aEl.innerHTML = "age: —";
      return;
    }
    if(age < 1){ aEl.innerHTML = "age: <1s"; return; }
    if(age < 60){ aEl.innerHTML = "age: " + Math.round(age) + "s"; return; }
    var m = Math.floor(age/60);
    var s = Math.round(age - 60*m);
    aEl.innerHTML = "age: " + m + "m " + s + "s";
  }

  function applyPaused(p){
    paused = !!p;
    if(pauseBtn){
      pauseBtn.innerHTML = paused ? "Resume" : "Pause";
    }
  }

  function httpJson(method, url, body, cb){
    // cb(err, obj)
    if(window.fetch){
      var opts = {method: method, headers: {}};
      if(body !== null && body !== undefined){
        opts.headers['Content-Type'] = 'application/json';
        opts.body = JSON.stringify(body);
      }
      window.fetch(url, opts)
        .then(function(r){
          if(!r.ok) throw new Error('HTTP ' + r.status);
          return r.json();
        })
        .then(function(obj){ cb(null, obj); })
        .catch(function(e){ cb(e, null); });
      return;
    }

    // XHR fallback (older browsers)
    try{
      var xhr = new XMLHttpRequest();
      xhr.open(method, url, true);
      xhr.onreadystatechange = function(){
        if(xhr.readyState !== 4) return;
        if(xhr.status < 200 || xhr.status >= 300){
          cb(new Error('HTTP ' + xhr.status), null);
          return;
        }
        var obj = null;
        try{ obj = JSON.parse(xhr.responseText || 'null'); }catch(e2){ obj = null; }
        cb(null, obj);
      };
      xhr.onerror = function(){ cb(new Error('network'), null); };
      if(body !== null && body !== undefined){
        xhr.setRequestHeader('Content-Type', 'application/json');
        xhr.send(JSON.stringify(body));
      } else {
        xhr.send();
      }
    }catch(e3){
      cb(e3, null);
    }
  }

  function pushPaused(){
    if(!TOKEN) return;
    httpJson('PUT', '/api/state?token=' + encodeURIComponent(TOKEN), {paused: paused, client_id: CLIENT_ID}, function(){ });
  }

  function handleStateMsg(msg){
    if(!msg || typeof msg !== 'object') return;
    if(msg.type === 'batch' && msg.frames && msg.frames.length){
      msg = msg.frames[msg.frames.length - 1];
    }
    if(msg && msg.client_id && msg.client_id === CLIENT_ID) return;
    if(msg && typeof msg.paused !== 'undefined'){
      applyPaused(!!msg.paused);
    }
  }

  function handleFrame(fr){
    if(!fr || typeof fr !== "object") return;
    if(paused) return;

    // Unwrap batch events: use the last frame (freshest).
    if(fr.type === "batch" && fr.frames && fr.frames.length){
      fr = fr.frames[fr.frames.length - 1];
    }

    if(typeof fr.t === "number"){
      var now = (new Date()).getTime()/1000.0;
      setAge(now - fr.t);
    }

    if(typeof fr.metric === "number"){
      if(mEl) mEl.innerHTML = fmt(fr.metric);
    }
    if(typeof fr.threshold === "number"){
      if(tEl) tEl.innerHTML = "threshold: " + fmt(fr.threshold);
    }

    var reward = fr.reward ? 1 : 0;
    var artifact = fr.artifact ? 1 : 0;
    if(artifact){
      setBadge("artifact", "artifact");
    } else if(reward){
      setBadge("reward", "reward");
    } else {
      setBadge("noreward", "no reward");
    }
  }

  // ---------------- transports ----------------

  function connectNf(){
    if(!window.EventSource){
      setBadge("artifact", "no EventSource");
      return;
    }
    var url = "/api/sse/nf?token=" + encodeURIComponent(TOKEN);
    var es = new EventSource(url);
    setBadge("noreward", "connecting…");

    es.onmessage = function(ev){
      try{ handleFrame(JSON.parse(ev.data)); }catch(e){}
    };
    es.onerror = function(){
      setBadge("artifact", "disconnected");
    };
  }

  function connectState(){
    if(!TOKEN || !window.EventSource) return;
    var es = new EventSource('/api/sse/state?token=' + encodeURIComponent(TOKEN));
    es.onmessage = function(ev){
      try{ handleStateMsg(JSON.parse(ev.data)); }catch(e){}
    };
  }

  function connectStream(){
    if(!window.EventSource){
      setBadge("artifact", "no EventSource");
      return;
    }
    var url = "/api/sse/stream?token=" + encodeURIComponent(TOKEN) + "&topics=nf,state";
    var es = new EventSource(url);
    setBadge("noreward", "connecting…");

    // Some older EventSource implementations may not support addEventListener.
    if(es.addEventListener){
      es.addEventListener('nf', function(ev){
        try{ handleFrame(JSON.parse(ev.data)); }catch(e){}
      });
      es.addEventListener('state', function(ev){
        try{ handleStateMsg(JSON.parse(ev.data)); }catch(e){}
      });
    } else {
      // Fallback: onmessage will only receive unnamed events; the server also
      // emits keepalives and may not deliver named events here.
      connectState();
      connectNf();
      return;
    }

    es.onerror = function(){
      setBadge("artifact", "disconnected");
    };
  }

  function connectSnapshot(){
    // Polling fallback via /api/snapshot.
    var curNf = 0;
    var curState = 0;
    setBadge("noreward", "polling…");

    function buildUrl(){
      var qp = [];
      qp.push('token=' + encodeURIComponent(TOKEN));
      qp.push('topics=' + encodeURIComponent('nf,state'));
      qp.push('wait=1.0');
      qp.push('limit=2500');
      qp.push('nf=' + encodeURIComponent(String(curNf||0)));
      qp.push('state=' + encodeURIComponent(String(curState||0)));
      return '/api/snapshot?' + qp.join('&');
    }

    function pollOnce(delay){
      setTimeout(function(){
        httpJson('GET', buildUrl(), null, function(err, resp){
          if(!err && resp){
            if(resp.nf){
              if(typeof resp.nf.cursor === 'number') curNf = resp.nf.cursor;
              if(resp.nf.batch) handleFrame(resp.nf.batch);
            }
            if(resp.state){
              if(typeof resp.state.cursor === 'number') curState = resp.state.cursor;
              if(resp.state.batch) handleStateMsg(resp.state.batch);
            }
            pollOnce(50);
          } else {
            pollOnce(500);
          }
        });
      }, delay||0);
    }

    pollOnce(0);
  }

  // ---------------- UI wiring ----------------

  if(pauseBtn){
    pauseBtn.onclick = function(){
      applyPaused(!paused);
      pushPaused();
    };
  }

  if(fsBtn){
    fsBtn.onclick = function(){
      var el = document.documentElement;
      var req = el.requestFullscreen || el.webkitRequestFullscreen || el.mozRequestFullScreen || el.msRequestFullscreen;
      if(req){ req.call(el); }
    };
  }

  if(!TOKEN){
    setBadge("artifact", "missing token");
    return;
  }

  // Load initial UI state.
  httpJson('GET', '/api/state?token=' + encodeURIComponent(TOKEN), null, function(err, st){
    if(!err && st && typeof st.paused !== 'undefined') applyPaused(!!st.paused);
  });

  // Choose transport.
  if(!window.EventSource){
    // No EventSource support: poll snapshots.
    connectSnapshot();
    return;
  }

  httpJson('GET', '/api/config?token=' + encodeURIComponent(TOKEN), null, function(err, cfg){
    var supports = cfg && cfg.supports ? cfg.supports : {};
    if(!err && supports && supports.sse_stream){
      connectStream();
    } else {
      connectState();
      connectNf();
    }
  });
})();
