/* E·INK controller — service worker */
const CACHE = 'eink-v3.1.51';
const STAGE = CACHE + '-staged';
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
 * app.js from another is a broken page. Derived from ASSETS so a script added
 * there is part of the set automatically. */
const SHELL = ASSETS.filter(u => /\.(html|css|js)$/.test(u));
/* Rewritten with a fresh build stamp by every `make build` / `make web`, so
 * it changes on every deploy — even one that touched only app.js. */
const PROBE = './version.json';

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
 * not change and no new worker was installed). A normal deploy bumps the
 * version, a new worker installs, and the page shows the «Обновить» toast —
 * that path does not depend on this one.
 *
 * Two steps, so the running page is never touched: on a navigation, compare
 * the live version.json with the cached one and, if the server has moved on,
 * download the whole shell into a staging cache; on the NEXT navigation,
 * before anything is served, move the staged set into the live cache as one.
 * Swapping in place would race the page that was just served from the old
 * index.html and is still requesting its old app.js. */
async function stageShell() {
  if (self.registration.installing || self.registration.waiting) return;   // a real update is on its way
  const fresh = await fetch(PROBE, { cache: 'no-cache' });
  if (!fresh || !fresh.ok) return;
  const stamp = await fresh.clone().text();
  const live = await caches.open(CACHE);
  const cur = await live.match(PROBE);
  if (cur && (await cur.clone().text()) === stamp) return;                 // nothing new on the server
  const staged = await caches.open(STAGE);
  const have = await staged.match(PROBE);
  if (have && (await have.clone().text()) === stamp) return;               // already staged
  const files = await Promise.all(SHELL.map(u => fetch(u, { cache: 'no-cache' }).catch(() => null)));
  if (files.some(r => !r || !r.ok)) return;                                // partial: keep the consistent old set
  await Promise.all(SHELL.map((u, i) => staged.put(u, files[i])));
  await staged.put(PROBE, fresh);
}

async function promoteStaged() {
  if (!(await caches.has(STAGE))) return;
  const staged = await caches.open(STAGE);
  const live = await caches.open(CACHE);
  const parts = await Promise.all([...SHELL, PROBE].map(u => staged.match(u)));
  if (parts.every(Boolean)) {
    await Promise.all([...SHELL, PROBE].map((u, i) => live.put(u, parts[i])));
  }
  await caches.delete(STAGE);
}

self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (url.origin !== location.origin) return;

  // firmware/ — NEVER cache (manifest.json + .bin must always be fresh)
  if (url.pathname.includes('/firmware/')) return;

  // Навигация: кэш → сеть (мгновенный старт даже при слабом интернете).
  // Сначала докатываем подготовленную оболочку, если она есть, потом
  // отдаём index.html из кэша; свежую версию готовим в фоне — целиком.
  if (req.mode === 'navigate') {
    e.respondWith((async () => {
      await promoteStaged().catch(() => {});
      const live = await caches.open(CACHE);
      const cached = await live.match('./index.html');
      if (cached) return cached;
      try { return await fetch(req); } catch (err) { return (await live.match('./')) || Promise.reject(err); }
    })());
    e.waitUntil(stageShell().catch(() => {}));
    return;
  }

  // Статика: кэш → сеть с дозаписью. Только живой кэш — в staging может
  // уже лежать app.js следующей версии, страница его получить не должна.
  e.respondWith((async () => {
    const live = await caches.open(CACHE);
    const hit = await live.match(req);
    if (hit) return hit;
    const res = await fetch(req);
    if (res.ok) live.put(req, res.clone());
    return res;
  })());
});
