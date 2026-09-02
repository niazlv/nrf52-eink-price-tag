/* E·INK controller — service worker */
const CACHE = 'eink-v3.1.34';
const ASSETS = [
  './',
  './index.html',
  './manifest.webmanifest',
  './version.json',
  './icon-192.png',
  './icon-512.png',
  './icon-512-maskable.png',
  './icon-180.png',
];

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

self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (url.origin !== location.origin) return;

  // firmware/ — NEVER cache (manifest.json + .bin must always be fresh)
  if (url.pathname.includes('/firmware/')) return;

  // Навигация: кэш → сеть (мгновенный старт даже при слабом интернете).
  // Оболочка отдаётся из кэша сразу, свежий index.html подтягивается в фоне.
  // Новую версию ставит сам SW (новый CACHE → тост «Обновить»), а не этот fetch.
  if (req.mode === 'navigate') {
    const update = fetch(req).then(fresh => {
      if (fresh && fresh.ok) caches.open(CACHE).then(c => c.put('./index.html', fresh.clone()));
      return fresh;
    }).catch(() => null);
    e.respondWith((async () => {
      const cached = await caches.match('./index.html');
      return cached || (await update) || (await caches.match('./'));
    })());
    e.waitUntil(update);
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
