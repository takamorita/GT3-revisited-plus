// Small static server for gt3r-web (Node.js 18 or later, no dependencies).
// Adds the COOP / COEP headers that SharedArrayBuffer (the engine's threads) needs.
// Browsers treat localhost as a secure context, so HTTPS is not needed.
//
//   node web/serve.mjs [folder to serve (default dist)] [port (default 8080)]
import { createServer } from 'node:http';
import { createReadStream, statSync } from 'node:fs';
import { extname, isAbsolute, join, relative, resolve } from 'node:path';

const root = resolve(process.argv[2] ?? 'dist');
const port = Number(process.argv[3] ?? 8080);
const types = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.wasm': 'application/wasm', '.css': 'text/css', '.json': 'application/json',
  '.png': 'image/png', '.ico': 'image/x-icon', '.txt': 'text/plain; charset=utf-8',
};

createServer((req, res) => {
  let path;
  try {
    // (inside try so a bad %xx does not crash it. Check that it is inside root with relative: startsWith also let a neighbor such as dist2 through)
    path = resolve(root, '.' + decodeURIComponent(new URL(req.url, 'http://localhost').pathname));
  } catch {
    res.writeHead(400).end('bad request');
    return;
  }
  const rel = relative(root, path);
  if (rel.startsWith('..') || isAbsolute(rel)) { res.writeHead(403).end(); return; }
  try {
    if (statSync(path).isDirectory()) path = join(path, 'index.html');
    const size = statSync(path).size;
    res.writeHead(200, {
      'Content-Type': types[extname(path)] ?? 'application/octet-stream',
      'Content-Length': size,
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cross-Origin-Resource-Policy': 'same-origin',
      'Cache-Control': 'no-cache',
      'Document-Policy': 'js-profiling', // needed by the in-page profiler (debug.js "Profile")
    });
    // so read errors (removed while reading, unreadable) do not crash the server
    createReadStream(path).on('error', () => res.destroy()).pipe(res);
  } catch {
    res.writeHead(404).end('not found');
  }
}).listen(port, '127.0.0.1', () => console.log(`gt3r-web: http://localhost:${port}/ (${root})`));
