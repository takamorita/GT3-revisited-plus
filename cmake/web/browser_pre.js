// The web page's side of gt3r (cmake/Emscripten.cmake links it into gt3r.js with --pre-js).
// Before main:
// - User data (config, saves, replays, mods) lives in IndexedDB: IDBFS at /persist, loaded
//   before main and written back a few seconds after a change and when the tab is hidden.
//   GT3R_DATA_DIR points the engine there (game::user_data_dir).
// - The disc image the player picked appears as /webdisc/<name>: a file whose reads go to
//   Module.gt3rDisc.read(position, length) (the page's disc reader, which reads the picked file
//   on a worker), so File::open, Disc::open and libchdr read it as any other file. Nothing of
//   the disc is copied into memory beyond that reader's cache. Files picked later in the game
//   (another disc, a mod, a save: platform::show_document_picker) go the same way.
// The page sets Module.gt3rDisc = { name, size, read(position, length) -> Uint8Array } and
// Module.arguments = ['/webdisc/<name>'] before it loads gt3r.js.
Module.preRun = Module.preRun || [];
Module.preRun.push(() => {
  const persistRoot = '/persist';
  const dataDir = persistRoot + '/GT3Revisited/';
  FS.mkdir(persistRoot);
  FS.mount(IDBFS, { autoPersist: false }, persistRoot);
  ENV.GT3R_DATA_DIR = dataDir;

  // Writes back (below) only once the load has succeeded: a write-back mirrors /persist into IndexedDB, so one made
  // while the load runs, or after it failed, would delete the saves and replays not read yet. After a failed load the
  // game runs without keeping its data (the warning says so) rather than wipe what is stored.
  let loaded = false;
  addRunDependency('gt3r-idbfs');
  FS.syncfs(true, (err) => {
    if (err) console.warn('gt3r: loading user data from IndexedDB failed; this session\'s changes are not kept', err);
    else loaded = true;
    try { FS.mkdirTree(dataDir); } catch (e) { /* already there */ }
    removeRunDependency('gt3r-idbfs');
    if (loaded) setInterval(save, 5000);
  });

  // Writes back: every few seconds (only changed files go out), and when the tab is hidden
  // or closed. One sync at a time.
  let syncing = false, again = false;
  const save = () => {
    if (!loaded) return;
    if (syncing) { again = true; return; }
    syncing = true;
    FS.syncfs(false, (err) => {
      syncing = false;
      if (err) console.warn('gt3r: saving user data to IndexedDB failed', err);
      if (again) { again = false; save(); }
    });
  };
  Module.gt3rSaveUserData = save;
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'hidden') save(); });
  addEventListener('pagehide', save);

  // A picked file at /webdisc/<name> whose reads go to read(position, length) -> Uint8Array.
  // As FS.createLazyFile does: MEMFS's stat and seek read usedBytes. The page calls this for the
  // files it picks later too (Module.gt3rPickFile); a file of the same name is replaced.
  Module.gt3rMountFile = (name, size, read) => {
    try { FS.mkdir('/webdisc'); } catch (e) { /* there */ }
    const path = '/webdisc/' + name;
    try { FS.unlink(path); } catch (e) { /* new */ }
    const node = FS.createFile('/webdisc', name, {}, true, false);
    Object.defineProperty(node, 'usedBytes', { get: () => size });
    const ops = { ...node.stream_ops };
    ops.read =(stream, buffer, offset, length, position) => {
      const n = Math.min(length, size - position);
      if (n <= 0) return 0;
      let bytes;
      try {
        bytes = read(position, n);
      } catch (e) {
        console.error('gt3r: read failed in ' + path, e);
        throw new FS.ErrnoError(29); // EIO
      }
      buffer.set(bytes, offset);
      return bytes.length;
    };
    ops.write = () => { throw new FS.ErrnoError(63); }; // EPERM: read only
    ops.mmap = () => { throw new FS.ErrnoError(43); };  // ENODEV
    node.stream_ops = ops;
    return path;
  };

  const disc = Module.gt3rDisc;
  if (disc) Module.gt3rMountFile(disc.name, disc.size, disc.read);
});
