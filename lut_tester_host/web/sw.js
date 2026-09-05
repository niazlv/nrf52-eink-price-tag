/* E·INK controller — service worker */
const CACHE = 'eink-v3.1.50';
/* The whole app shell. Every file index.html loads must be here, or an
 * installed app that goes offline will be missing it. */
const ASSETS = [
  './',
  './index.html',
  './style.css',
  './protocol.js',
  './app.js',
  './pwa.js',
  './manifest.webmanifest',
  './version.json',
  './icon-192.png',
  './icon-512.png',
  './icon-512-maskable.png',
  './icon-180.png',
];
/* The files that must change together: an index.html from one deploy with an
 * app.js from another is a broken page. The background refresh below only
 * ever swaps them as a set. */
const SHELL = ['./index.html', './style.css', './protocol.js', './app.js', './pwa.js'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(ASSETS)));
});

self.addEventListener('activate', e => {
  e.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k)));
    await self.clients.claim();
  })());
});

self.addEventListener('message', e => {
  if (e.data === 'SKIP_WAITING') self.skipWaiting();
});

/* Safety net for a deploy that forgot to bump WEB_VERSION (so this file did
 * not change and no new cache was installed): on every navigation, fetch the
 * live index.html; if it differs from the cached one, fetch the rest of the
 * shell and store all of it at once. A normal deploy bumps the version, a new
 * worker installs, and the page shows the «Обновить» toast — that path does
 * not depend on this one. */
async function refreshShell() {
  const c = await caches.open(CACHE);
  const fresh = await fetch('./index.html', { cache: 'no-cache' });
  if (!fresh || !fresh.ok) return;
  const cached = await c.match('./index.html');
  if (cached && (await cached.clone().text()) === (await fresh.clone().text())) return;
  const rest = await Promise.all(SHELL.slice(1).map(u => fetch(u, { cache: 'no-cache' })));
  if (rest.some(r => !r || !r.ok)) return;        // partial: keep the consistent old set
  await c.put('./index.html', fresh);
  await Promise.all(SHELL.slice(1).map((u, i) => c.put(u, rest[i])));
}

self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (url.origin !== location.origin) return;

  // firmware/ — NEVER cache (manifest.json + .bin must always be fresh)
  if (url.pathname.includes('/firmware/')) return;

  // Навигация: кэш → сеть (мгновенный старт даже при слабом интернете).
  // Оболочка отдаётся из кэша сразу; свежая версия подтягивается в фоне —
  // целиком, чтобы index.html и app.js не разъехались.
  if (req.mode === 'navigate') {
    e.respondWith((async () => {
      const cached = await caches.match('./index.html');
      if (cached) return cached;
      try { return await fetch(req); } catch (err) { return (await caches.match('./')) || Promise.reject(err); }
    })());
    e.waitUntil(refreshShell().catch(() => {}));
    return;
  }

  // Статика: кэш → сеть с дозаписью
  e.respondWith((async () => {
    const hit = await caches.match(req);
    if (hit) return hit;
    try {
      const res = await fetch(req);
      if (res.ok) (await caches.open(CACHE)).put(req, res.clone());
      return res;
    } catch (err) {
      return hit || Promise.reject(err);
    }
  })());
});
