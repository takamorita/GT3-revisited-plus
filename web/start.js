// Start screen: pick a disc, remember it, click to start.
// The "Start" click both starts audio and confirms permission to read the remembered file.
'use strict';

(() => {
  const $ = (id) => document.getElementById(id);
  const startEl = $('start'), messageEl = $('message'), resumeBtn = $('resume');
  const say = (text) => { messageEl.textContent = text || ''; };

  // --- Remembered disc (FileSystemFileHandle in IndexedDB) -------------------------------
  const db = () => new Promise((resolve, reject) => {
    const req = indexedDB.open('gt3r-web', 1);
    req.onupgradeneeded = () => req.result.createObjectStore('discs');
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
  const idb = async (mode, fn) => {
    const d = await db();
    return new Promise((resolve, reject) => {
      const tx = d.transaction('discs', mode);
      const req = fn(tx.objectStore('discs'));
      tx.oncomplete = () => resolve(req && req.result);
      tx.onerror = () => reject(tx.error);
      tx.onabort = () => reject(tx.error || new Error('IndexedDB: transaction aborted')); // (else the picker never starts)
    });
  };
  const remembered = () => idb('readonly', (s) => s.get('last')).catch(() => undefined);
  const remember = (handle) => idb('readwrite', (s) => s.put({ handle, name: handle.name }, 'last')).catch(() => {});

  // --- Launch ----------------------------------------------------------------------------
  let started = false;
  const bridges = new Map(); // /webdisc/<name> -> DiscBridge (picking the same name again closes the previous one)
  // Make a file readable as /webdisc/<name>. A disc gets a large cache; mod zips and saves get a small one.
  // Picking the same name again closes the previous one (Worker and cache) (before, each pick added a Worker and up to 192 MB)
  const open = async (file, isDisc = true) => {
    const b = await new DiscBridge(file, isDisc ? {} : { cacheBytes: 16 * 1024 * 1024 }).ready;
    const old = bridges.get(b.name);
    if (old) old.close();
    bridges.set(b.name, b);
    if (isDisc) self.gt3rDiscStats = () => b.summary();
    return b;
  };
  // Pick a file (shared by the start screen and the game). Cancel gives null. A disc is remembered for next time.
  // Browsers without showOpenFilePicker (Firefox, etc.) use <input type=file>
  const pickFile = async (exts, isDisc) => {
    if (!self.showOpenFilePicker) return new Promise((resolve) => {
      const input = Object.assign(document.createElement('input'), { type: 'file', accept: exts.join(',') });
      input.onchange = () => resolve(input.files[0] || null);
      input.oncancel = () => resolve(null);
      input.click();
    });
    const types = exts.length ? [{ description: isDisc ? 'Disc image' : 'File', accept: { 'application/octet-stream': exts } }] : undefined;
    const [handle] = await showOpenFilePicker({ id: isDisc ? 'gt3r-disc' : 'gt3r-file', types });
    if (isDisc) await remember(handle); // (remember does not throw on failure)
    return handle.getFile();
  };
  const launch = async (file) => {
    if (started) return;
    started = true;
    let disc = null;
    if (file) {
      try {
        disc = await open(file);
      } catch (e) {
        started = false;
        say(String(e.message || e));
        return;
      }
    }
    const canvas = $('canvas');
    self.Module = {
      canvas,
      arguments: disc ? ['/webdisc/' + disc.name] : [],
      gt3rDisc: disc ? { name: disc.name, size: disc.size, read: (p, n) => disc.read(p, n) } : null,
      print: (t) => console.log(t),
      printErr: (t) => console.warn(t),
      onAbort: (what) => stopped('Stopped: ' + what, false), // (not saved: a file may be half written)
      onExit: () => stopped('GT3 Revisited has quit.'),
      onRuntimeInitialized: () => { self.gt3rReady = true; },
      gt3rQuit: () => stopped('GT3 Revisited has quit.'),
      gt3rPickFile: pickInGame,
      // An export (Export to PS2, a camera file, an event's .zip): the game writes it at `path` in this iteration of
      // its loop, and the browser downloads it after.
      gt3rDownloadLater: (path, name) => setTimeout(() => downloadFromGame(path, name), 0),
    };
    startEl.hidden = true;
    canvas.hidden = false;
    canvas.focus();
    const s = document.createElement('script');
    s.src = 'gt3r.js';
    s.onerror = () => stopped('Could not load gt3r.js');
    document.body.appendChild(s);
  };

  // File picking inside the game (Open disc image, mod zip, loading a save:
  // platform::show_document_picker). The picked file is placed at /webdisc/<name> and its path is returned.
  // accept has the form ".iso,.chd" ("" means any). A disc is remembered for next time.
  const pickInGame = async (accept) => {
    const exts = accept ? accept.split(',') : [];
    const isDisc = exts.includes('.iso') || exts.includes('.chd');
    let path = '';
    try {
      const file = await pickFile(exts, isDisc);
      if (file) {
        const bridge = await open(file, isDisc);
        path = Module.gt3rMountFile(bridge.name, bridge.size, (p, n) => bridge.read(p, n));
      }
    } catch (e) {
      if (e.name !== 'AbortError') console.warn('gt3r: cannot pick or open the file', e);
    }
    Module.ccall('gt3r_web_picked', null, ['string'], [path]);
  };

  // A file the game wrote for the player to keep (app/save_dialogs.cpp): downloaded, then removed from /tmp.
  const downloadFromGame = (path, name) => {
    let bytes;
    try {
      bytes = Module.FS.readFile(path);
    } catch (_) {
      return; // (not written: the game said why)
    }
    try { Module.FS.unlink(path); } catch (_) {}
    const a = Object.assign(document.createElement('a'), { href: URL.createObjectURL(new Blob([bytes])), download: name });
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  };

  const stopped = (text, save = true) => {
    // After quit or a fault: reload to return. After a fault, do not save (do not leave half-written files in IndexedDB)
    if (save && self.Module && self.Module.gt3rSaveUserData) self.Module.gt3rSaveUserData();
    startEl.hidden = false;
    $('canvas').hidden = true;
    say(text + '\nReload the page to start again.');
    for (const b of startEl.querySelectorAll('button')) b.disabled = true;
  };

  // --- Buttons --------------------------------------------------------------------------
  if (!self.crossOriginIsolated) {
    say(self.gt3rCoiPending ? 'Preparing the page…'
      : 'This page must be served with the COOP/COEP headers (web/README.md), or the browser must allow its service worker (coi-sw.js).');
  }

  remembered().then((rec) => {
    if (!rec || !rec.handle) return;
    resumeBtn.hidden = false;
    resumeBtn.innerHTML = '';
    resumeBtn.append('Start');
    const small = document.createElement('small');
    small.textContent = rec.name;
    resumeBtn.append(small);
    resumeBtn.focus();
    resumeBtn.onclick = async () => {
      say('');
      try {
        let perm = await rec.handle.queryPermission({ mode: 'read' });
        if (perm !== 'granted') perm = await rec.handle.requestPermission({ mode: 'read' });
        if (perm !== 'granted') { say('No permission to read the file. Press again, or choose the file again.'); return; }
        launch(await rec.handle.getFile());
      } catch (e) {
        say(`Could not open ${rec.name} (it may have been moved or deleted). Choose it again.\n${e.message || e}`);
      }
    };
  });

  $('pick').onclick = async () => {
    say('');
    try {
      const file = await pickFile(['.iso', '.chd'], true);
      if (file) launch(file);
    } catch (e) {
      if (e.name !== 'AbortError') say(String(e.message || e));
    }
  };
  $('file').onchange = (e) => { if (e.target.files[0]) launch(e.target.files[0]); };
  $('nodisc').onclick = () => launch(null);

  // Drop (not remembered)
  startEl.addEventListener('dragover', (e) => { e.preventDefault(); startEl.classList.add('dragging'); });
  startEl.addEventListener('dragleave', () => startEl.classList.remove('dragging'));
  startEl.addEventListener('drop', async (e) => {
    e.preventDefault();
    startEl.classList.remove('dragging');
    const item = [...e.dataTransfer.items].find((i) => i.kind === 'file');
    if (!item) return;
    // Items are usable only during the event, so take both before the await
    const file = item.getAsFile();
    const handlePromise = item.getAsFileSystemHandle ? item.getAsFileSystemHandle() : null;
    // Chrome / Edge: if the drop also gives a handle, remember it
    if (handlePromise) {
      try {
        const h = await handlePromise;
        if (h && h.kind === 'file') await remember(h);
      } catch (_) { /* open without remembering */ }
    }
    launch(file);
  });
})();
