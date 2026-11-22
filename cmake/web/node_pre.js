// gt3r_tests under node (cmake/Emscripten.cmake). Before main:
// - the host folders the tests read are mounted at their own paths (NODEFS): the source tree
//   (GT3R_SOURCE_DIR, for examples/mods/...) and the folder of GT3R_DISC when it is set;
// - the GT3R_* variables of the host's environment are passed on, so `GT3R_DISC=... node
//   gt3r_tests.js` works as with the native runner.
// Everything else (the temporary folder the runner makes, user data) stays in memory (MEMFS).
Module.preRun = Module.preRun || [];
Module.preRun.push(() => {
  const path = require('path');
  const mountAt = (dir) => {
    if (!dir) return;
    dir = path.resolve(dir);
    try {
      FS.mkdirTree(dir);
      FS.mount(NODEFS, { root: dir }, dir);
    } catch (e) {
      console.error(`node_pre.js: cannot mount ${dir}: ${e}`);
    }
  };
  if (process.env.GT3R_SOURCE_DIR_MOUNT) mountAt(process.env.GT3R_SOURCE_DIR_MOUNT);
  if (process.env.GT3R_DISC) mountAt(path.dirname(process.env.GT3R_DISC));
  for (const [k, v] of Object.entries(process.env)) {
    if (k.startsWith('GT3R_')) ENV[k] = v;
  }
});
