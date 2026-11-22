// Debug overlay (for M3 measurements). index.html loads it only with ?debug (otherwise nothing is wrapped).
// Starts shown; F9 shows or hides it (not remembered), F8 changes the corner, F10 adds a split mark.
// Shows: a frame interval graph, interval stats, game processing time (to tell it apart from render waits),
// disc waits, a list of long frames, and a table of segments between marks.
//
// How it measures: wraps the requestAnimationFrame that the engine (Emscripten main loop) calls, and records
// the call interval (= frame interval) and the time spent inside the callback (= wasm game processing + GL calls).
// If the interval is much longer than the callback time, the wait is on the GPU or VSync (outside the callback).
// This file loads before the engine (index.html). The overlay draws itself with setInterval, not the unwrapped rAF.
//
// From the console: gt3rFrameStats(ms) summary of the last ms / gt3rMark(name) starts a segment /
//   gt3rSegments() segment table / gt3rHitches() long frames / gt3rDebug(true|false) show or hide
'use strict';

(() => {
  const N = 1 << 16; // frames kept (about 18 minutes at 60 fps)
  const MASK = N - 1;
  const iv = new Float32Array(N); // interval since the previous frame, ms
  const cb = new Float32Array(N); // ms spent inside the callback
  const ts = new Float64Array(N); // frame time (performance.now clock)
  let count = 0;
  let prevT = 0;
  let firstT = 0;
  const HITCH_MS = 50;
  const hitches = [];

  // --- Segments (between marks) -------------------------------------------------------------
  const segments = [];
  let seg = null;
  const discStats = () => (typeof self.gt3rDiscStats === 'function' ? self.gt3rDiscStats() : null);
  const startSeg = (name) => {
    seg = { name, from: count, t0: performance.now(), disc0: discStats() };
  };

  const pct = (sorted, p) => sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * p))];
  const r1 = (v) => Math.round(v * 10) / 10;

  // Summary of global frame numbers [from, to)
  const summarize = (from, to) => {
    from = Math.max(from, count - N, 0);
    const n = to - from;
    if (n < 2) return null;
    const a = new Float32Array(n);
    const c = new Float32Array(n);
    let total = 0;
    let over25 = 0;
    let over50 = 0;
    for (let k = 0; k < n; k++) {
      const v = iv[(from + k) & MASK];
      a[k] = v;
      c[k] = cb[(from + k) & MASK];
      total += v;
      if (v > 25) over25++;
      if (v > HITCH_MS) over50++;
    }
    a.sort();
    c.sort();
    return {
      frames: n, fps: r1((1000 * n) / total), medMs: r1(pct(a, 0.5)), p95Ms: r1(pct(a, 0.95)), p99Ms: r1(pct(a, 0.99)),
      maxMs: r1(a[n - 1]), over25Pct: r1((100 * over25) / n), over50: over50,
      cbMedMs: r1(pct(c, 0.5)), cbP95Ms: r1(pct(c, 0.95)), cbMaxMs: r1(c[n - 1]),
    };
  };

  const lastMs = (ms) => {
    const limit = performance.now() - ms;
    let k = count;
    while (k > 0 && count - k < N && ts[(k - 1) & MASK] >= limit) k--;
    return summarize(k, count);
  };

  const closeSeg = () => {
    if (!seg) return null;
    const s = summarize(seg.from, count);
    if (!s) return null;
    const d1 = discStats();
    const row = { name: seg.name, seconds: r1((performance.now() - seg.t0) / 1000), ...s };
    if (d1 && seg.disc0) {
      row.discWaitMs = Math.round(d1.waitMs - seg.disc0.waitMs);
      row.discReads = d1.reads - seg.disc0.reads;
    }
    segments.push(row);
    return row;
  };

  self.gt3rMark = (name) => {
    const row = closeSeg();
    startSeg(name || `mark ${segments.length + 2}`);
    return row;
  };
  self.gt3rFrameStats = (ms = 5000) => lastMs(ms);
  self.gt3rSegments = () => {
    const cur = seg ? summarize(seg.from, count) : null;
    return cur ? [...segments, { name: `${seg.name} (open)`, seconds: r1((performance.now() - seg.t0) / 1000), ...cur }] : [...segments];
  };
  self.gt3rHitches = () => [...hitches];

  // --- Time of WebGL calls that can be slow (used to break down long frames) ----------------------
  // Shader compile and link and the queries for their results (parallel compile waits show up here), texture and buffer uploads,
  // and syncs with the GPU (readPixels, finish, getError, clientWaitSync). Draw calls (draw*) are too many to wrap.
  const GL_WATCH = {
    compileShader: 'compile', linkProgram: 'link', getShaderParameter: 'compile?', getProgramParameter: 'link?',
    getShaderInfoLog: 'log', getProgramInfoLog: 'log',
    texImage2D: 'texImage', texSubImage2D: 'texImage', texImage3D: 'texImage', texSubImage3D: 'texImage',
    compressedTexImage2D: 'texImage', compressedTexSubImage2D: 'texImage', generateMipmap: 'mipmap',
    bufferData: 'buffer', bufferSubData: 'buffer', readPixels: 'sync', finish: 'sync', getError: 'sync', clientWaitSync: 'sync',
  };
  let glAcc = {}; // kind -> { ms, n } (from the end of the previous frame's callback until now)
  for (const C of [self.WebGL2RenderingContext, self.WebGLRenderingContext]) {
    if (!C) continue;
    for (const [fname, label] of Object.entries(GL_WATCH)) {
      const orig = C.prototype[fname];
      if (typeof orig !== 'function') continue;
      C.prototype[fname] = function (...args) {
        const t0 = performance.now();
        try { return orig.apply(this, args); } finally {
          const e = glAcc[label] || (glAcc[label] = { ms: 0, n: 0 });
          e.ms += performance.now() - t0;
          e.n++;
        }
      };
    }
  }
  const takeGl = () => { // kinds that took 1 ms or more, largest first
    const out = Object.entries(glAcc).filter(([, e]) => e.ms >= 1).sort((x, y) => y[1].ms - x[1].ms)
      .map(([k, e]) => ({ kind: k, ms: Math.round(e.ms), calls: e.n }));
    glAcc = {};
    return out;
  };
  const glText = (list) => (list && list.length ? list.slice(0, 3).map((e) => `${e.kind} ${e.ms}ms×${e.calls}`).join(', ') : '');

  // --- Wrap rAF (the engine's main loop) ------------------------------------------------------
  const raf0 = self.requestAnimationFrame.bind(self);
  let prevD = 0; // callback time of the previous frame (a long frame is usually caused by the callback before it)
  let prevGl = [];
  self.requestAnimationFrame = (fn) => raf0((t) => {
    const a = performance.now();
    fn(t);
    const d = performance.now() - a;
    const gl = takeGl();
    if (prevT) {
      const i = count & MASK;
      const gap = t - prevT;
      iv[i] = gap;
      cb[i] = d;
      ts[i] = a;
      if (!firstT) { firstT = t; startSeg('start'); }
      count++;
      if (gap > HITCH_MS && gap < 5000) {
        // A long gap usually means the previous callback was long (prevD). If that was short, the cause is outside the callback (GC, audio, Worker waits, etc.)
        hitches.push({ atSec: r1((t - firstT) / 1000), ms: Math.round(gap), prevCbMs: r1(prevD), cbMs: r1(d), gl: prevGl, seg: seg ? seg.name : '' });
        if (hitches.length > 200) hitches.shift();
      }
    }
    prevT = t;
    prevD = d;
    prevGl = gl;
  });
  document.addEventListener('visibilitychange', () => { prevT = 0; }); // time in the background is not counted

  // --- Screen -------------------------------------------------------------------------------
  const CORNERS = [['top', 'right'], ['bottom', 'right'], ['bottom', 'left'], ['top', 'left']];
  let corner = 0;

  const root = document.createElement('div');
  root.id = 'gt3r-debug';
  root.hidden = false; // (when loaded with ?debug: starts shown)
  root.style.cssText = 'position:fixed;z-index:10;width:430px;padding:8px;box-sizing:border-box;border:1px solid #3a3f52;border-radius:6px;' +
    'background:rgba(14,16,21,.82);color:#e6e8ee;font:11px/1.45 ui-monospace,Consolas,monospace;pointer-events:none;white-space:pre;';
  const graph = document.createElement('canvas');
  graph.width = 414; graph.height = 56;
  // (the graph is the handle: drag it to move the overlay)
  graph.style.cssText = 'display:block;width:414px;height:56px;margin-bottom:4px;pointer-events:auto;cursor:move;touch-action:none;';
  graph.title = 'Drag to move';
  const text = document.createElement('div');
  const buttons = document.createElement('div');
  buttons.style.cssText = 'display:flex;gap:6px;margin-top:6px;pointer-events:auto;';
  const mkBtn = (label, onClick) => {
    const b = document.createElement('button');
    b.textContent = label;
    b.style.cssText = 'font:inherit;padding:2px 8px;border:1px solid #3a3f52;border-radius:4px;background:#1a1d26;color:#e6e8ee;cursor:pointer;';
    b.onclick = (e) => { onClick(e); const c = document.getElementById('canvas'); if (c) c.focus(); };
    return b;
  };
  const toMarkdown = () => {
    const rows = self.gt3rSegments();
    const head = '| mark | s | frames | fps | med ms | p95 ms | >25ms % | max ms | cpu med/max ms | disc wait ms |\n|---|---|---|---|---|---|---|---|---|---|\n';
    return head + rows.map((r) => `| ${r.name} | ${r.seconds} | ${r.frames} | ${r.fps} | ${r.medMs} | ${r.p95Ms} | ${r.over25Pct} | ${r.maxMs} | ${r.cbMedMs} / ${r.cbMaxMs} | ${r.discWaitMs ?? ''} |`).join('\n');
  };
  buttons.append(
    mkBtn('Mark (F10)', () => self.gt3rMark()),
    mkBtn('Copy', () => { navigator.clipboard?.writeText(toMarkdown()).catch(() => {}); }),
    mkBtn('Corner (F8)', () => moveCorner()),
  );
  // --- Change 3D settings on the fly (the engine's gt3r_web_debug_get / _set, main.cpp) ---------------------
  // Same steps as the Settings screen (write the setting, apply, save), so changed values persist to the next run.
  // A change adds a mark automatically (so the segment table is laid out "per setting").
  const dbgGet = () => { try { return JSON.parse(self.Module.ccall('gt3r_web_debug_get', 'string', [], [])); } catch (_) { return null; } };
  // Audio output (cmake/web/sdl_webaudio.c): SDL's audio thread writes to a ring in shared memory, and an AudioWorklet plays it.
  // ctrl: 0 frames written / 1 frames played / 2 Worklet calls / 3 underruns / 4 queue-ahead limit / 5 frames per call / 6 discarding
  self.gt3rWebAudio = () => {
    const a = self.Module && self.Module.gt3rWebAudio;
    if (!a) return null;
    const c = a.ctrl, rate = (self.Module.SDL3 && self.Module.SDL3.audioContext) ? self.Module.SDL3.audioContext.sampleRate : 48000;
    const queued = (Atomics.load(c, 0) - Atomics.load(c, 1)) | 0;
    return {
      playing: !!a.node, queuedMs: r1((queued * 1000) / rate), targetMs: r1((c[4] * 1000) / rate), underruns: Atomics.load(c, 3),
      calls: Atomics.load(c, 2), dropping: Atomics.load(c, 6) === 1, rate,
      contextState: self.Module.SDL3 && self.Module.SDL3.audioContext ? self.Module.SDL3.audioContext.state : '?',
    };
  };
  const dbgSet = (key, value) => {
    try {
      if (key === 'audio_target_ms') { // queue-ahead limit (for experiments)
        const a = self.Module.gt3rWebAudio, rate = self.Module.SDL3.audioContext.sampleRate;
        Atomics.store(a.ctrl, 4, Math.round((Number(value) * rate) / 1000));
        return 1;
      }
      return self.Module.ccall('gt3r_web_debug_set', 'number', ['string', 'string'], [key, String(value)]);
    } catch (_) { return 0; }
  };
  self.gt3rGfx = dbgGet;
  self.gt3rSet = (key, value, mark = true) => {
    const ok = dbgSet(key, value);
    if (ok && mark) self.gt3rMark(`${key}=${value}`);
    return ok;
  };
  const SETTINGS = [
    { key: 'msaa', label: 'MSAA', options: (st) => st.levels.map((l) => [String(l), l > 1 ? `${l}x` : 'off']), on: (st, v) => String(st.msaa) === v },
    { key: 'render_scale', label: 'Scale', options: () => [['auto', 'auto'], ['0.5', '50%'], ['0.75', '75%'], ['1.0', '100%'], ['1.25', '125%']], on: (st, v) => st.scaleSetting === v },
    { key: 'dynamic', label: 'Dynamic', options: () => [['off', 'off'], ['on', 'on']], on: (st, v) => (st.dynamic ? 'on' : 'off') === v },
    { key: 'vsync', label: 'VSync', options: () => [['1', 'on'], ['0', 'off']], on: (st, v) => String(st.vsync) === v },
    { key: 'anisotropy', label: 'Aniso', options: () => [['1', 'off'], ['4', '4x'], ['16', '16x']], on: (st, v) => String(st.aniso) === v },
  ];
  const ctl = document.createElement('div');
  ctl.style.cssText = 'margin-top:6px;display:grid;gap:3px;pointer-events:auto;';
  let ctlKey = '';
  const refreshControls = (st) => {
    const key = JSON.stringify(st);
    if (key === ctlKey) return;
    ctlKey = key;
    ctl.textContent = '';
    if (!st || !st.levels) return;
    for (const def of SETTINGS) {
      const row = document.createElement('div');
      row.style.cssText = 'display:flex;gap:4px;align-items:center;';
      const lab = document.createElement('span');
      lab.textContent = def.label;
      lab.style.cssText = 'width:56px;color:#9aa0b1;';
      row.append(lab);
      for (const [value, name] of def.options(st)) {
        const active = def.on(st, value);
        const b = mkBtn(name, () => { self.gt3rSet(def.key, value); });
        b.style.padding = '1px 7px';
        if (active) { b.style.background = '#ffc42e'; b.style.color = '#1a1406'; b.style.borderColor = '#ffc42e'; }
        row.append(b);
      }
      ctl.append(row);
    }
    if (st.dynamic) {
      const d = document.createElement('div');
      d.textContent = `dynamic ${st.dynScale} (now ${st.scale})`;
      d.style.color = '#9aa0b1';
      ctl.append(d);
    }
  };

  // --- Profiler (JS Self-Profiling; needs Document-Policy: js-profiling = added by web/serve.mjs) -----------------
  // Samples only the main thread at a fixed interval and gives each function's "self time" and "time including callees".
  // wasm function names appear in builds with --profiling-funcs (cmake GT3R_WEB_PROFILING, ON by default).
  // Usage: gt3rProfile('start') → enter a heavy scene → gt3rProfile('stop') gives the top functions. The button does the same.
  let profiler = null;
  let profWhat = '';
  let profResult = null;
  let lastTrace = null;
  // Reading a trace (shared by the 4 functions): function name, the leaf-to-root name chain, sample weight (until the next sample; capped at 50 ms over dropped stretches)
  const parts = (t) => ({ frames: t.frames || [], stacks: t.stacks || [], samples: t.samples || [] });
  const fname = ({ frames, stacks }, sid) => (frames[stacks[sid].frameId] || {}).name || '(anon)';
  const short = (s, len = 60) => s.replace(/\(anonymous namespace\)/g, '{anon}').replace(/\(.*$/, '').slice(0, len);
  const chainOf = (tr, sid, max = Infinity) => {
    const c = [];
    for (let cur = sid; cur != null && tr.stacks[cur] && c.length < max; cur = tr.stacks[cur].parentId) c.push(fname(tr, cur));
    return c;
  };
  const weight = (samples, i) => Math.min(Math.max((samples[i + 1] || samples[i]).timestamp - samples[i].timestamp, 0), 50);
  const analyze = (trace) => {
    const tr = parts(trace), { samples } = tr;
    const self = new Map();
    const incl = new Map();
    const waits = new Map(); // call chains leading to lock waits (futex) (from just before the wait up to the first gt3r:: function)
    const add = (m, k, w) => m.set(k, (m.get(k) || 0) + w);
    let total = 0;
    for (let i = 0; i < samples.length; i++) {
      const sid = samples[i].stackId;
      if (sid == null) continue; // nothing was running
      const w = weight(samples, i);
      total += w;
      const chain = chainOf(tr, sid);
      if (chain.length) add(self, chain[0], w);
      for (const name of new Set(chain)) add(incl, name, w);
      const f = chain.indexOf('_do_futex_wait');
      if (f >= 0) { // along the leaf → root chain, from the wait upward to the first gt3r:: (at most 7)
        const path = [];
        for (let k = f; k < chain.length && path.length < 7; k++) {
          const n = chain[k].replace(/\(.*$/, '').slice(0, 70);
          if (n === '_do_futex_wait' || !path.length || path[path.length - 1] !== n) path.push(n);
          if (chain[k].startsWith('gt3r::') || chain[k].startsWith('SDL_')) break;
        }
        add(waits, path.join(' < '), w);
      }
    }
    const top = (m, n = 15) => [...m.entries()].sort((a, b) => b[1] - a[1]).slice(0, n).map(([name, ms]) => ({ name, ms: Math.round(ms) }));
    return { what: profWhat, samples: samples.length, busyMs: Math.round(total), interval: trace.sampleInterval, self: top(self), inclusive: top(incl), lockWaits: top(waits, 10) };
  };
  self.gt3rProfile = async (cmd, what = '') => {
    if (cmd === 'start') {
      if (typeof self.Profiler !== 'function') return 'no Profiler (Chrome, served with Document-Policy: js-profiling)';
      if (profiler) return 'running';
      try { profiler = new self.Profiler({ sampleInterval: 1, maxBufferSize: 100000 }); profWhat = what; profResult = null; return 'ok'; } catch (e) { return String(e.message || e); }
    }
    if (cmd === 'stop' && profiler) {
      const p = profiler; profiler = null;
      lastTrace = await p.stop();
      profResult = analyze(lastTrace);
    }
    return profResult;
  };
  // Places in the last profile where the sample interval was long (not counted in between): the length and the function of the sample just before it (4 from the leaf).
  // Sync waits (GPU, disc) and work the sampler cannot enter show up here
  self.gt3rProfileGaps = (n = 10, minMs = 30) => {
    if (!lastTrace) return null;
    const tr = parts(lastTrace), { samples } = tr;
    const at = (j, max) => chainOf(tr, samples[j].stackId, max).map((x) => short(x)).join(' < ') || '(idle)';
    const gaps = [];
    for (let i = 0; i + 1 < samples.length; i++) {
      const g = samples[i + 1].timestamp - samples[i].timestamp;
      if (g >= minMs) gaps.push({ ms: Math.round(g), before: at(i, 4), after: at(i + 1, 3) });
    }
    const total = gaps.reduce((a, x) => a + x.ms, 0);
    return { count: gaps.length, totalMs: total, top: gaps.sort((a, b) => b.ms - a.ms).slice(0, n) };
  };
  // Places in the last profile where samples with an empty stack (neither JS nor wasm running: GC, browser internals, sync with the GPU process, etc.)
  // continue: the length and the functions of the samples just before and after. Those of minMs or more, longest first
  self.gt3rProfileIdle = (n = 10, minMs = 10) => {
    if (!lastTrace) return null;
    const tr = parts(lastTrace), { samples } = tr;
    const top3 = (sid) => chainOf(tr, sid, 4).map((x) => short(x)).join(' < ');
    const runs = [];
    for (let i = 0; i < samples.length; i++) {
      if (samples[i].stackId != null) continue;
      let j = i;
      while (j + 1 < samples.length && samples[j + 1].stackId == null) j++;
      const end = j + 1 < samples.length ? samples[j + 1].timestamp : samples[j].timestamp;
      const ms = end - samples[i].timestamp;
      if (ms >= minMs) runs.push({ ms: Math.round(ms), before: i > 0 ? top3(samples[i - 1].stackId) : '', after: j + 1 < samples.length ? top3(samples[j + 1].stackId) : '' });
      i = j;
    }
    const total = runs.reduce((a, x) => a + x.ms, 0);
    return { count: runs.length, totalMs: total, top: runs.sort((a, b) => b.ms - a.ms).slice(0, n) };
  };
  // In the last profile, the time under functions whose name contains `part`, per direct child (ms, including callees).
  // Example: gt3rProfileChildren('build_vehicle') → [{name, ms}, ...]. Where recursion stacks the same function, it counts once
  self.gt3rProfileChildren = (part, n = 20) => {
    if (!lastTrace) return null;
    const tr = parts(lastTrace), { samples } = tr;
    const kids = new Map();
    let total = 0;
    for (let i = 0; i < samples.length; i++) {
      const sid = samples[i].stackId;
      if (sid == null) continue;
      const w = weight(samples, i);
      // Leaf to root. Find the outermost function containing `part` and charge the time to the function just below it (leaf side)
      const chain = chainOf(tr, sid);
      let at = -1;
      for (let k = chain.length - 1; k >= 0; k--) if (chain[k].includes(part)) { at = k; break; }
      if (at < 0) continue;
      total += w;
      const child = at > 0 ? chain[at - 1] : '(self)';
      kids.set(child, (kids.get(child) || 0) + w);
    }
    return { totalMs: Math.round(total), children: [...kids.entries()].sort((a, b) => b[1] - a[1]).slice(0, n).map(([k, ms]) => ({ ms: Math.round(ms), name: short(k, 110) })) };
  };
  const profBtn = mkBtn('Profile', async () => {
    if (!profiler) { const r = await self.gt3rProfile('start'); profBtn.textContent = r === 'ok' ? 'Stop' : 'No profiler'; if (r !== 'ok') console.warn('gt3r profile:', r); }
    else { await self.gt3rProfile('stop'); profBtn.textContent = 'Profile'; }
  });
  buttons.append(profBtn, mkBtn('Hide (F9)', () => show(false)));

  root.append(graph, text, ctl, buttons);
  document.body.append(root);
  // Small tab while hidden (click to show again). Shown where the overlay was
  const tab = mkBtn('Debug (F9)', () => show(true));
  tab.id = 'gt3r-debug-tab';
  tab.hidden = true;
  tab.style.cssText += 'position:fixed;z-index:10;font:11px/1.45 ui-monospace,Consolas,monospace;opacity:.85;'; // (added to the mkBtn style)
  document.body.append(tab);

  // Placement: one of 4 corners (F8 cycles) or where it was dragged (free)
  let free = null;
  const place = () => {
    for (const el of [root, tab]) {
      for (const p of ['top', 'bottom', 'left', 'right']) el.style[p] = '';
      if (free) { el.style.left = `${free.x}px`; el.style.top = `${free.y}px`; }
      else for (const p of CORNERS[corner]) el.style[p] = '8px';
    }
  };
  const moveCorner = () => { free = null; corner = (corner + 1) % CORNERS.length; place(); };
  place();
  graph.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    const r = root.getBoundingClientRect(), dx = e.clientX - r.left, dy = e.clientY - r.top;
    graph.setPointerCapture(e.pointerId);
    const move = (ev) => {
      free = { x: Math.round(Math.min(Math.max(ev.clientX - dx, 0), innerWidth - r.width)),
               y: Math.round(Math.min(Math.max(ev.clientY - dy, 0), innerHeight - 40)) };
      place();
    };
    const up = () => { graph.removeEventListener('pointermove', move); graph.removeEventListener('pointerup', up); };
    graph.addEventListener('pointermove', move);
    graph.addEventListener('pointerup', up);
  });

  const show = (on) => { root.hidden = !on; tab.hidden = on; }; // (not remembered: before, once shown it stayed shown from then on)
  self.gt3rDebug = show;
  try { localStorage.removeItem('gt3rDebug'); } catch (_) { /* clear the value an older version remembered */ }

  addEventListener('keydown', (e) => {
    if (e.repeat) return;
    if (e.code === 'F9') { e.preventDefault(); show(root.hidden); }
    else if (e.code === 'F8' && !root.hidden) { e.preventDefault(); moveCorner(); }
    else if (e.code === 'F10' && !root.hidden) { e.preventDefault(); self.gt3rMark(); }
  }, true);

  // Disc waits are sampled every second; show the increase over the last 5 seconds
  const discHist = [];
  setInterval(() => { const d = discStats(); if (d) { discHist.push({ t: performance.now(), d }); if (discHist.length > 6) discHist.shift(); } }, 1000);

  const drawGraph = () => {
    const g = graph.getContext('2d');
    const w = graph.width, h = graph.height, scale = h / 50; // 0〜50 ms
    g.clearRect(0, 0, w, h);
    const shown = Math.min(count, Math.floor(w / 1.35));
    for (let k = 0; k < shown; k++) {
      const i = (count - shown + k) & MASK;
      const v = Math.min(iv[i], 50);
      g.fillStyle = iv[i] < 20 ? '#4fbf7a' : iv[i] < 40 ? '#ffc42e' : '#ff7a6b';
      g.fillRect(k * 1.35, h - v * scale, 1.1, v * scale);
    }
    g.strokeStyle = '#6a7088'; g.lineWidth = 1;
    for (const ms of [16.7, 33.3]) { g.beginPath(); g.moveTo(0, h - ms * scale); g.lineTo(w, h - ms * scale); g.stroke(); }
    g.strokeStyle = '#6aa8ff'; g.beginPath();
    for (let k = 0; k < shown; k++) {
      const y = h - Math.min(cb[(count - shown + k) & MASK], 50) * scale;
      if (k === 0) g.moveTo(0, y); else g.lineTo(k * 1.35, y);
    }
    g.stroke();
  };

  const fmt = (s) => (s ? `${String(s.fps).padStart(5)} fps  med ${s.medMs}  p95 ${s.p95Ms}  max ${s.maxMs} ms  >25ms ${s.over25Pct}%  cpu ${s.cbMedMs}` : '(no frames)');
  setInterval(() => {
    if (root.hidden) return;
    drawGraph();
    const st = self.gt3rReady ? dbgGet() : null;
    refreshControls(st);
    const cv = document.getElementById('canvas');
    const lines = [];
    lines.push(fmt(lastMs(5000)));
    lines.push(`canvas ${cv ? `${cv.width}x${cv.height} @${devicePixelRatio}` : '?'}`);
    const wa = self.gt3rWebAudio();
    if (wa) lines.push(`audio ${wa.playing ? wa.contextState : 'starting'}  ${wa.queuedMs}/${wa.targetMs} ms  underruns ${wa.underruns}${wa.dropping ? '  dropping' : ''}`);
    const last = discHist[discHist.length - 1];
    if (last) {
      const first = discHist[0];
      lines.push(`disc wait ${last.d.waitMs} ms (+${Math.round(last.d.waitMs - first.d.waitMs)} in ${Math.round((last.t - first.t) / 1000)} s)  max ${last.d.maxWaitMs}`);
    }
    if (hitches.length) {
      lines.push('slow frames (>50 ms)');
      for (const x of hitches.slice(-5).reverse()) {
        lines.push(`  +${x.atSec}s  ${x.ms} ms (cpu ${x.prevCbMs})  ${x.seg}`);
        const g = glText(x.gl);
        if (g) lines.push(`      GL: ${g}`);
      }
    }
    const rows = self.gt3rSegments().slice(-5);
    if (rows.length) {
      lines.push('marks (F10)');
      for (const r of rows) lines.push(`  ${r.name}: ${r.fps} fps  med ${r.medMs}  >25ms ${r.over25Pct}%  cpu ${r.cbMedMs}${r.discWaitMs != null ? `  disc ${r.discWaitMs}` : ''}`);
    }
    if (profResult) {
      lines.push(`profile (self ms, ${profResult.samples} samples, busy ${profResult.busyMs} ms)`);
      for (const e of profResult.self.slice(0, 8)) lines.push(`  ${String(e.ms).padStart(5)}  ${e.name.slice(0, 60)}`);
    }
    text.textContent = lines.join('\n');
  }, 250);
})();
