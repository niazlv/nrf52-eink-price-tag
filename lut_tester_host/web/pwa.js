/* pwa.js — offline shell, update toast, install button. Runs after the DOM
 * it touches (the toasts) exists, i.e. last. */
// ═══════════════════════════════════════════════════════════════════
//  PWA: офлайн-оболочка, обновления, установка
// ═══════════════════════════════════════════════════════════════════
(function pwa() {
  // — Диагностика среды: объясняем, почему установка может не работать —
  const proto = location.protocol;
  const isLocalhost = ['localhost', '127.0.0.1', '[::1]'].includes(location.hostname);
  let warn = null;
  if (proto === 'file:' || proto === 'content:') {
    warn = 'Страница открыта как локальный файл. «Добавить на главный экран» в этом режиме создаёт нерабочий ярлык — Android не даёт ему доступа к файлу. Для настоящей установки и офлайн-режима размести эти файлы на HTTPS-хостинге (GitHub Pages, Netlify) и открой оттуда.';
  } else if (proto === 'http:' && !isLocalhost) {
    warn = 'Открыто по HTTP без шифрования (' + location.host + '). Chrome не разрешает установку PWA и офлайн-режим на таком адресе — нужен HTTPS или localhost.';
  }
  if (warn) {
    $('pwa-warn-txt').textContent = warn;
    $('pwa-warn').hidden = false;
    logE('PWA: установка недоступна — ' + (proto === 'http:' ? 'нет HTTPS' : 'открыто как файл (' + proto + '//)'));
  } else if (window.matchMedia('(display-mode: standalone)').matches) {
    logI('PWA: запущено как установленное приложение');
  } else {
    logI('PWA: среда пригодна для установки (' + (location.host || proto) + ')');
  }

  // — Service worker (нужен http(s); на file:// просто пропускаем) —
  if ('serviceWorker' in navigator && proto.startsWith('http')) {
    let waitingSW = null;
    const toast = $('sw-toast');
    const showUpdate = sw => { waitingSW = sw; toast.hidden = false; };
    $('sw-reload').onclick = () => {
      toast.hidden = true;
      if (waitingSW) waitingSW.postMessage('SKIP_WAITING');
    };
    let reloading = false;
    navigator.serviceWorker.addEventListener('controllerchange', () => {
      if (reloading) return; reloading = true; location.reload();
    });
    navigator.serviceWorker.register('sw.js', { scope: './' }).then(reg => {
      if (reg.waiting && navigator.serviceWorker.controller) showUpdate(reg.waiting);
      reg.addEventListener('updatefound', () => {
        const sw = reg.installing;
        sw && sw.addEventListener('statechange', () => {
          if (sw.state === 'installed' && navigator.serviceWorker.controller) showUpdate(sw);
        });
      });
    }).catch(e => logE('SW: ' + e.message));
  }

  // — Установка на рабочий стол —
  let deferredPrompt = null;
  const ib = $('btn-install');
  window.addEventListener('beforeinstallprompt', e => {
    e.preventDefault();
    deferredPrompt = e;
    ib.hidden = false;
  });
  ib.addEventListener('click', async () => {
    if (!deferredPrompt) return;
    deferredPrompt.prompt();
    await deferredPrompt.userChoice;
    deferredPrompt = null;
    ib.hidden = true;
  });
  window.addEventListener('appinstalled', () => { ib.hidden = true; logI('Приложение установлено'); });

  // — Индикатор сети (BLE и офлайн работают — это просто статус) —
  const chip = $('net-chip');
  const net = () => { chip.hidden = navigator.onLine; };
  window.addEventListener('online', net);
  window.addEventListener('offline', net);
  net();
})();
