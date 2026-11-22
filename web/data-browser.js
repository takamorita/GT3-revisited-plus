// The start screen's "Data": shows the user data the game keeps in the browser (saves, replays, screenshots, music, mods, etc.)
// and moves files in and out (drag and drop to upload, select and save, delete, rename, new folder).
//
// User data lives in IndexedDB "/persist" (Emscripten's IDBFS, cmake/web/browser_pre.js). The key is the path
// (/persist/GT3Revisited/saves/... ), the value is { timestamp: Date, mode, contents: Uint8Array } (folders have no contents).
// The game loads this at startup (it creates paths in order, so parent folder entries are needed too), and while running
// writes its own files back every few seconds. So this screen is used only before the game starts (start screen). After the game, reload the page.
'use strict';

(() => {
  const ROOT = '/persist/GT3Revisited';
  const DB = '/persist', STORE = 'FILE_DATA', VERSION = 21; // (same as IDBFS; a different version cannot be opened)
  const S_IFDIR = 0o040000, FILE_MODE = 0o100666, DIR_MODE = 0o040777;
  const isDir = (mode) => (mode & 0o170000) === S_IFDIR;
  const $ = (id) => document.getElementById(id);

  // --- IndexedDB -----------------------------------------------------------------------------
  const openDb = () => new Promise((resolve, reject) => {
    const req = indexedDB.open(DB, VERSION);
    req.onupgradeneeded = () => { // (a browser that has never run the game: create it in the same shape as IDBFS)
      const store = req.result.objectStoreNames.contains(STORE)
        ? req.transaction.objectStore(STORE) : req.result.createObjectStore(STORE);
      if (!store.indexNames.contains('timestamp')) store.createIndex('timestamp', 'timestamp', { unique: false });
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
  const tx = async (mode, fn) => {
    const db = await openDb();
    try {
      return await new Promise((resolve, reject) => {
        const t = db.transaction(STORE, mode);
        let out;
        Promise.resolve(fn(t.objectStore(STORE))).then((v) => { out = v; }, reject);
        t.oncomplete = () => resolve(out);
        t.onerror = () => reject(t.error);
        t.onabort = () => reject(t.error || new Error('IndexedDB: aborted'));
      });
    } finally { db.close(); }
  };
  const req = (r) => new Promise((resolve, reject) => { r.onsuccess = () => resolve(r.result); r.onerror = () => reject(r.error); });

  // List of all entries (path -> { dir, size, time }). Contents are not kept
  const listAll = () => tx('readonly', (s) => new Promise((resolve, reject) => {
    const all = new Map();
    const c = s.openCursor();
    c.onsuccess = () => {
      const cur = c.result;
      if (!cur) { resolve(all); return; }
      const v = cur.value;
      all.set(String(cur.key), { dir: isDir(v.mode), size: v.contents ? v.contents.byteLength : 0, time: v.timestamp });
      cur.continue();
    };
    c.onerror = () => reject(c.error);
  }));
  const readFile = (path) => tx('readonly', (s) => req(s.get(path))).then((v) => (v && v.contents) || new Uint8Array());
  // Write files ([path, bytes]) and folders. Missing folder entries are created up through the parents
  const write = (files, folders = []) => tx('readwrite', async (s) => {
    const now = new Date();
    const dirs = new Set();
    for (const d of [...folders, ...files.map(([p]) => p.slice(0, p.lastIndexOf('/')))])
      for (let p = d; p.length >= ROOT.length; p = p.slice(0, p.lastIndexOf('/'))) dirs.add(p);
    for (const d of dirs) if (!(await req(s.get(d)))) s.put({ timestamp: now, mode: DIR_MODE }, d);
    for (const [path, bytes] of files) s.put({ timestamp: now, mode: FILE_MODE, contents: bytes }, path);
  });
  // Delete (with contents for a folder)
  const remove = (paths) => tx('readwrite', async (s) => {
    const keys = (await req(s.getAllKeys())).map(String);
    for (const p of paths) for (const k of keys) if (k === p || k.startsWith(p + '/')) s.delete(k);
  });
  // Rename (within the same folder; a folder keeps its contents)
  const rename = (from, to) => tx('readwrite', async (s) => {
    const keys = (await req(s.getAllKeys())).map(String);
    if (keys.some((k) => k === to || k.startsWith(to + '/'))) throw new Error('the name is taken');
    for (const k of keys) {
      if (k !== from && !k.startsWith(from + '/')) continue;
      const v = await req(s.get(k));
      v.timestamp = new Date();
      s.put(v, to + k.slice(from.length));
      s.delete(k);
    }
  });

  // --- Screen ----------------------------------------------------------------------------------
  let cwd = ROOT, entries = new Map(), busy = false;
  const view = $('data'), listEl = $('data-list'), pathEl = $('data-path'), msgEl = $('data-message');
  const say = (t, bad) => { msgEl.textContent = t || ''; msgEl.classList.toggle('bad', !!bad); };
  const sizeText = (n) => n < 1024 ? `${n} B` : n < 1048576 ? `${(n / 1024).toFixed(1)} KB` : `${(n / 1048576).toFixed(1)} MB`;
  const timeText = (d) => (d instanceof Date ? d : new Date(d)).toLocaleString(undefined, { dateStyle: 'short', timeStyle: 'short' });
  const children = (dir) => [...entries].filter(([k]) => k.startsWith(dir + '/') && !k.slice(dir.length + 1).includes('/'))
    .sort(([a, x], [b, y]) => (y.dir - x.dir) || a.localeCompare(b));
  const treeSize = (p) => [...entries].reduce((n, [k, v]) => n + ((k === p || k.startsWith(p + '/')) ? v.size : 0), 0);
  const selected = () => [...listEl.querySelectorAll('input[type=checkbox]:checked')].map((c) => c.value);
  const base = (p) => p.slice(p.lastIndexOf('/') + 1);

  const work = async (what, fn) => {
    if (busy) return;
    busy = true;
    view.classList.add('busy');
    try { await fn(); } catch (e) { say(`${what}: ${e.message || e}`, true); console.error(e); }
    busy = false;
    view.classList.remove('busy');
    await refresh();
  };

  const refresh = async () => {
    try { entries = await listAll(); } catch (e) { say(`Cannot read the data: ${e.message || e}`, true); entries = new Map(); }
    if (cwd !== ROOT && !entries.has(cwd)) cwd = ROOT;
    // Location (click to go there)
    pathEl.innerHTML = '';
    const parts = cwd.slice(ROOT.length).split('/').filter(Boolean);
    [['Data', ROOT], ...parts.map((n, i) => [n, ROOT + '/' + parts.slice(0, i + 1).join('/')])].forEach(([n, p], i) => {
      if (i) pathEl.append(' / ');
      const a = document.createElement('button');
      a.className = 'crumb';
      a.textContent = n;
      a.onclick = () => { cwd = p; refresh(); };
      pathEl.append(a);
    });
    listEl.innerHTML = '';
    const rows = children(cwd);
    if (!rows.length) {
      const empty = document.createElement('li');
      empty.className = 'empty';
      empty.textContent = 'Empty. Drop files here to upload.';
      listEl.append(empty);
    }
    for (const [path, e] of rows) {
      const li = document.createElement('li');
      const cb = Object.assign(document.createElement('input'), { type: 'checkbox', value: path });
      cb.setAttribute('aria-label', base(path));
      const name = document.createElement('button');
      name.className = 'name';
      name.textContent = (e.dir ? '📁 ' : '') + base(path);
      name.title = e.dir ? 'Open' : 'Download';
      name.onclick = () => (e.dir ? (cwd = path, refresh()) : work('Download', () => download([path])));
      const meta = document.createElement('span');
      meta.className = 'meta';
      meta.textContent = `${sizeText(e.dir ? treeSize(path) : e.size)}　${timeText(e.time)}`;
      li.append(cb, name, meta);
      listEl.append(li);
    }
    updateButtons();
  };
  const updateButtons = () => {
    const n = selected().length;
    $('data-download').disabled = !n;
    $('data-delete').disabled = !n;
    $('data-rename').disabled = n !== 1;
    $('data-up').disabled = cwd === ROOT;
  };
  listEl.addEventListener('change', updateButtons);

  // Save: a single file downloads as is. For several files or folders, pick a destination folder and write out the tree
  // (browsers without showDirectoryPicker download them one by one)
  const files = (paths) => [...entries].filter(([k, v]) => !v.dir && paths.some((p) => k === p || k.startsWith(p + '/'))).map(([k]) => k);
  const download = async (paths) => {
    const list = files(paths);
    if (!list.length) { say('Nothing to download'); return; }
    if (list.length === 1 && paths.length === 1 && !entries.get(paths[0]).dir) {
      saveBlob(base(list[0]), await readFile(list[0]));
      say(`Downloaded ${base(list[0])}`);
      return;
    }
    if (self.showDirectoryPicker) {
      let dir;
      try { dir = await showDirectoryPicker({ id: 'gt3r-data', mode: 'readwrite' }); } catch (e) { if (e.name === 'AbortError') return; throw e; }
      for (const k of list) {
        const rel = k.slice(cwd.length + 1).split('/');
        let d = dir;
        for (const part of rel.slice(0, -1)) d = await d.getDirectoryHandle(part, { create: true });
        const w = await (await d.getFileHandle(rel[rel.length - 1], { create: true })).createWritable();
        await w.write(await readFile(k));
        await w.close();
      }
      say(`Saved ${list.length} files to ${dir.name}`);
      return;
    }
    for (const k of list) { saveBlob(base(k), await readFile(k)); await new Promise((r) => setTimeout(r, 300)); }
    say(`Downloaded ${list.length} files`);
  };
  const saveBlob = (name, bytes) => {
    const a = Object.assign(document.createElement('a'), { href: URL.createObjectURL(new Blob([bytes])), download: name });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  };

  // Upload: dropped files and folders (folders with contents) go into the current folder. Same names are overwritten
  const call = (fn) => new Promise((resolve, reject) => fn(resolve, reject));
  const readEntry = async (entry, prefix, out) => {
    if (entry.isFile) { out.push([prefix + entry.name, await call((ok, ng) => entry.file(ok, ng))]); return; }
    const dir = prefix + entry.name + '/', reader = entry.createReader();
    out.push([dir, null]); // (empty folders are created too)
    // readEntries may not return everything at once (repeat until it returns empty)
    for (let batch; (batch = await call((ok, ng) => reader.readEntries(ok, ng))).length;)
      for (const e of batch) await readEntry(e, dir, out);
  };
  const upload = async (items) => {
    const files = [], dirs = [];
    for (const [rel, f] of items) {
      if (!f) { dirs.push(cwd + '/' + rel.slice(0, -1)); continue; }
      files.push([cwd + '/' + rel, new Uint8Array(await f.arrayBuffer())]);
    }
    const replaced = files.filter(([p]) => entries.has(p)).length;
    await write(files, dirs);
    say(`Uploaded ${files.length} files${replaced ? ` (${replaced} replaced)` : ''}`);
  };
  // Even when a drop lands outside the frame, keep the browser from opening the file and leaving the page
  view.addEventListener('dragover', (e) => e.preventDefault());
  view.addEventListener('drop', (e) => { e.preventDefault(); say('Drop inside the box'); });
  const box = $('data-box');
  box.addEventListener('dragover', (e) => { e.preventDefault(); e.stopPropagation(); box.classList.add('dragging'); });
  box.addEventListener('dragleave', (e) => { if (!box.contains(e.relatedTarget)) box.classList.remove('dragging'); });
  box.addEventListener('drop', (e) => {
    e.preventDefault();
    e.stopPropagation(); // (do not pass it on to the start screen's "open the dropped disc")
    box.classList.remove('dragging');
    // Items are usable only during the event, so take the entries first
    const got = [...e.dataTransfer.items].filter((i) => i.kind === 'file').map((i) => (i.webkitGetAsEntry ? i.webkitGetAsEntry() : null));
    const plain = [...e.dataTransfer.files];
    work('Upload', async () => {
      const items = [];
      if (got.length && got.every(Boolean)) for (const en of got) await readEntry(en, '', items);
      else for (const f of plain) items.push([f.name, f]);
      await upload(items);
    });
  });
  $('data-file').onchange = (e) => {
    const list = [...e.target.files].map((f) => [f.name, f]);
    e.target.value = '';
    if (list.length) work('Upload', () => upload(list));
  };
  $('data-upload').onclick = () => $('data-file').click();
  $('data-download').onclick = () => work('Download', () => download(selected()));
  $('data-delete').onclick = () => {
    const sel = selected();
    const names = sel.map(base).join(', ');
    if (!confirm(`Delete ${names}? This cannot be undone.`)) return;
    work('Delete', async () => { await remove(sel); say(`Deleted ${sel.length}`); });
  };
  $('data-rename').onclick = () => {
    const [p] = selected();
    const to = prompt('New name', base(p));
    if (!to || to === base(p)) return;
    if (/[\/\\]/.test(to)) { say('No / or \\ in names', true); return; }
    work('Rename', async () => { await rename(p, p.slice(0, p.lastIndexOf('/') + 1) + to); say(`Renamed to ${to}`); });
  };
  $('data-mkdir').onclick = () => {
    const name = prompt('Folder name', '');
    if (!name) return;
    if (/[\/\\]/.test(name)) { say('No / or \\ in names', true); return; }
    work('New folder', async () => { await write([], [cwd + '/' + name]); say(`Created ${name}`); });
  };
  $('data-up').onclick = () => { cwd = cwd.slice(0, cwd.lastIndexOf('/')); refresh(); };

  // Open and close (start screen button)
  self.gt3rOpenData = () => { say(''); view.hidden = false; $('start').hidden = true; cwd = ROOT; refresh(); $('data-close').focus(); };
  $('data-close').onclick = () => { view.hidden = true; $('start').hidden = false; $('data-open').focus(); };
  addEventListener('keydown', (e) => { if (e.key === 'Escape' && !view.hidden) $('data-close').click(); });
})();
