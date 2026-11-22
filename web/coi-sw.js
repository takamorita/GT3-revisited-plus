// A service worker for hosts that cannot send headers (GitHub Pages): it adds to the page's own files the
// Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy headers the engine's threads need
// (SharedArrayBuffer), as web/serve.zig and web/serve.mjs send them. index.html registers it only when the page
// is not cross-origin isolated, and reloads once under it. Other origins' requests are left alone.
'use strict';

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (new URL(request.url).origin !== self.location.origin) return;
  if (request.cache === 'only-if-cached' && request.mode !== 'same-origin') return;
  event.respondWith(fetch(request).then((response) => {
    if (response.status === 0) return response; // (opaque: nothing to add)
    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    headers.set('Document-Policy', 'js-profiling'); // (the debug overlay's profiler, debug.js)
    return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
  }));
});
