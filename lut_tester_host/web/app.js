/* app.js — the E·INK controller. One classic script, same global scope it
 * had when it lived inside index.html; protocol.js (loaded first) decodes
 * the tag's reply lines, pwa.js (loaded last) owns the service worker.
 * Every file of the shell is listed in sw.js ASSETS so the PWA keeps working
 * offline and updates as one snapshot. */
// ═══════════════════════════════════════════════════════════════════
//  Constants
// ═══════════════════════════════════════════════════════════════════
const NUS_SVC = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_RX  = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_TX  = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';

// VSTREAM wire geometry — the binary video protocol is 128x296-only.
const DISP_W  = 128;  // physical width
const DISP_H  = 296;  // physical height
const FB_SIZE = DISP_W * DISP_H / 8; // 4736

// Panels the firmware can report in SYSINFO panel=WxH (physical = controller
// RAM geometry; logical = the canvas the UI draws on). 128x296 is driven
// portrait and shown landscape (rot 1); 400x300 is landscape natively. A
// single-plane panel has no red buffer in the firmware: red goes first and is
// staged with FAPPLY RED (see sendBWR).
// toneStr = per-pulse darkening (%) of the TONE_DARK / SOFT_DARK LUTs on that
// panel — the photo-tone sliders' defaults. 128x296 values are the tuned
// originals; 400x300 soft was measured by eye (9), dark is 2x that (4 vs 2
// subframes) until the ladder (sendToneLadder) says otherwise.
const PANELS = {
  '128x296': { key:'128x296', ctl:'SSD1675A', pw:128, ph:296, lw:296, lh:128, rot:1, redPlane:true,  toneStr:{ dark:36, soft:18 } },
  '400x300': { key:'400x300', ctl:'SSD1619A', pw:400, ph:300, lw:400, lh:300, rot:0, redPlane:false, toneStr:{ dark:18, soft:9  } },
};
let PANEL = PANELS['128x296'];

// The tone-strength sliders are remembered per panel key; PANELS is the fallback.
function loadToneStr(p) {
  let s = null;
  try { s = JSON.parse(localStorage.getItem('toneStr:' + p.key) || 'null'); } catch {}
  return { ...p.toneStr, ...(s || {}) };
}
function applyToneStr(p) {
  const s = loadToneStr(p);
  const set = (id, lbl, v) => { const e = $(id); if (!e) return; e.value = v; const l = $(lbl); if (l) l.textContent = v; };
  set('tone-pulse-str', 'tone-pstr-val', s.dark);
  set('tone-soft-str',  'tone-soft-str-val', s.soft);
}
function saveToneStr() {
  const s = { dark: +$('tone-pulse-str').value, soft: +$('tone-soft-str').value };
  try { localStorage.setItem('toneStr:' + PANEL.key, JSON.stringify(s)); } catch {}
}
const fbSize = () => PANEL.pw * PANEL.ph / 8;

// Logical canvas pixel → [byte index, bit mask] in one controller plane.
function physIndex(lx, ly) {
  const phx = PANEL.rot === 1 ? PANEL.pw - 1 - ly : lx;
  const phy = PANEL.rot === 1 ? lx : ly;
  return [phy * (PANEL.pw >> 3) + (phx >> 3), 1 << (7 - (phx & 7))];
}

function setPanel(key) {
  const p = PANELS[key] || PANELS['128x296'];
  const changed = p !== PANEL;
  PANEL = p;
  const cv = $('disp-canvas');
  if (cv && (cv.width !== p.lw || cv.height !== p.lh)) {
    cv.width = p.lw; cv.height = p.lh;                 // resets the bitmap
    cv.style.width = p.lw + 'px';                     // natural size; CSS caps at 100%
    cv.style.height = p.lh + 'px';
    clearCanvas();
  }
  const lbl = $('disp-label'); if (lbl) lbl.textContent = `${p.ctl} · ${p.lw}×${p.lh}`;
  const sp = $('sys-panel');   if (sp)  sp.textContent  = `${p.lw}×${p.lh} (${p.ctl}${p.redPlane ? '' : ', red via FAPPLY RED'})`;
  const ll = $('enc-land-label'); if (ll) ll.textContent = `Landscape (${p.lw}×${p.lh})`;
  const pt = $('anim-prev-title'); if (pt) pt.textContent = `Превью дисплея (${p.lw}×${p.lh})`;
  const ap = $('anim-preview');
  if (ap && (ap.width !== p.lw || ap.height !== p.lh)) {
    ap.width = p.lw; ap.height = p.lh;
    ap.style.width = p.lw + 'px';
    ap.style.height = p.lh + 'px';
  }
  applyToneStr(p);
  if (changed && typeof IE !== 'undefined' && IE.img) { IE.oc = null; renderImg(); }
}

// Stream geometry. The UI's "landscape" flag means "rotate the frame" only on
// a portrait-driven panel (128x296); on the 400x300 it is the native layout.
function vsRotated(land) { return land !== (PANEL.rot === 0); }
function vsDims(land) {
  return vsRotated(land) ? [PANEL.ph, PANEL.pw] : [PANEL.pw, PANEL.ph];
}
// Logical stream pixel → [byte index, bit mask] in a physical plane.
function vsIdx(lx, ly, land) {
  const rotated = vsRotated(land);
  const phx = rotated ? PANEL.pw - 1 - ly : lx;
  const phy = rotated ? lx : ly;
  return [phy * (PANEL.pw >> 3) + (phx >> 3), 1 << (7 - (phx & 7))];
}

// Send progress. One state object drives two views: the box under the display
// preview (#send-box) and the fixed status bar (#send-bar) that stays visible
// on every tab while a photo is in flight. Phases:
//   tx      — FW:/RW: chunks over NUS (bytes known up front)
//   passes  — tone photo: N VSTREAM passes, one panel refresh each
//   refresh — all bytes sent, waiting for the panel (TELE:fapply / FAPPLY done)
//   done | warn | stop | error — terminal, the views hide themselves
// While tx/passes run, body.sending greys out every .send-btn (no double sends).
const sendProg = { total: 0, sent: 0, rx: 0, tag: '', phase: '', t0: 0,
                   passes: 0, pass: 0, tick: null, hideT: null, wd: null };
let sendLock = false;                      // held by sendCanvas() around the whole transfer
const fmtKB = b => (b / 1024).toFixed(1) + ' KB';
const fmtS  = ms => (ms / 1000).toFixed(1) + ' s';
const SEND_ICON = { tx: '↑', passes: '↑', refresh: '⟳', done: '✓', warn: '⚠', stop: '■', error: '✗' };
function sendBusy() { return sendLock || sendProg.phase === 'tx' || sendProg.phase === 'passes'; }

function sendProgShow(total, opts = {}) {
  clearTimeout(sendProg.hideT); clearTimeout(sendProg.wd); clearInterval(sendProg.tick);
  Object.assign(sendProg, { total, sent: 0, rx: 0, tag: opts.tag || '', t0: Date.now(),
                            phase: opts.passes ? 'passes' : 'tx', passes: opts.passes || 0, pass: 0,
                            planeBase: 0, txTime: '', txBytes: 0, text: '', barText: '' });
  const box = $('send-box'); if (box) box.style.display = 'block';
  const bar = $('send-bar'); if (bar) bar.hidden = false;
  const stop = $('send-bar-stop'); if (stop) stop.hidden = !opts.passes;   // only tone passes can stop
  document.body.classList.add('sending');
  sendProg.tick = setInterval(sendProgRender, 250);   // elapsed time keeps ticking between chunks
  sendProgRender();
}
function sendProgRender() {
  const p = sendProg, el = Date.now() - p.t0, dt = fmtS(el);
  let pct = 100, txt = '', card = '';
  if (p.phase === 'tx') {
    pct  = p.total ? Math.round(100 * p.sent / p.total) : 0;
    const spd = el > 300 ? ` · ${fmtKB(p.sent / (el / 1000))}/s` : '';
    txt  = `${p.tag} ${pct}% · ${(p.sent / 1024).toFixed(1)}/${fmtKB(p.total)}${spd} · ${dt}`;
    card = `${p.tag} отправлено ${p.sent}/${p.total} B · принято ${p.rx} B · ${dt}`;
  } else if (p.phase === 'passes') {
    pct  = p.passes ? Math.round(100 * p.pass / p.passes) : 0;
    txt  = `${p.tag} ${p.pass}/${p.passes} · ${pct}% · ${fmtKB(p.sent)} · ${dt}`;
    card = `${p.tag} · проход ${p.pass}/${p.passes} · ${pct}% · ${fmtKB(p.sent)} · ${dt}`;
  } else if (p.phase === 'refresh') {
    txt  = `Передано ${fmtKB(p.txBytes)} за ${p.txTime} · обновление экрана… ${dt}`;
    card = `передано ${p.txBytes} B за ${p.txTime} · обновление экрана… ${dt}`;
  } else {
    txt  = p.barText || p.text || p.phase;
    card = p.text || p.phase;
  }
  const pb = $('send-prog'), info = $('send-info');
  if (pb)   pb.style.width = pct + '%';
  if (info) info.textContent = card;
  const bar = $('send-bar'), bt = $('send-bar-txt'), bf = $('send-bar-fill'), bi = $('send-bar-ico');
  if (bar) bar.className = p.phase;
  if (bt)  bt.textContent = txt;
  if (bf)  bf.style.width = pct + '%';
  if (bi)  bi.textContent = SEND_ICON[p.phase] || '↑';
}
function sendProgRx(line) {             // "FW:rx 4096/15000"
  const r = Proto.parseFwRx(line);
  if (!r || sendProg.phase !== 'tx' || r.plane !== sendProg.tag) return;
  sendProg.rx = (sendProg.planeBase || 0) + r.got;
  sendProgRender();
}
function sendProgPass(n, bytes) {       // tone photo: one pass confirmed by the panel
  sendProg.pass = n; sendProg.sent += bytes || 0;
  sendProgRender();
}
function sendProgRefresh() {
  const el = Date.now() - sendProg.t0;
  sendProg.txTime = fmtS(el); sendProg.txBytes = sendProg.sent;
  sendProg.phase = 'refresh'; sendProg.t0 = Date.now();
  document.body.classList.remove('sending');    // bytes are out; the panel is refreshing
  sendProgRender();
  // The panel normally answers within seconds; if the reply never comes (old
  // firmware, dropped notification) close the bar instead of ticking forever.
  clearTimeout(sendProg.wd);
  sendProg.wd = setTimeout(() => {
    if (sendProg.phase === 'refresh') sendProgDone('экран не ответил за 60 с — смотри консоль', 'warn');
  }, 60000);
}
function sendProgEnd(phase, text, barText, hideMs) {
  const p = sendProg;
  clearInterval(p.tick); clearTimeout(p.wd); clearTimeout(p.hideT);
  p.phase = phase; p.text = text; p.barText = barText;
  document.body.classList.remove('sending');
  const stop = $('send-bar-stop'); if (stop) stop.hidden = true;
  sendProgRender();
  p.hideT = setTimeout(() => {
    if (p.phase !== phase) return;                 // a new send took over
    const b = $('send-box'); if (b) b.style.display = 'none';
    const bar = $('send-bar'); if (bar) bar.hidden = true;
  }, hideMs);
}
function sendProgDone(text, kind = 'done') {     // kind: done | warn | stop
  const p = sendProg, scr = p.phase === 'refresh' ? fmtS(Date.now() - p.t0) : '';
  let bar;
  if (kind === 'done') {
    bar = p.passes
      ? `Готово · ${p.pass}/${p.passes} проходов · ${fmtKB(p.sent)} за ${fmtS(Date.now() - p.t0)}`
      : `Готово · ${fmtKB(p.txBytes || p.total)} за ${p.txTime}` + (scr ? ` · экран ${scr}` : '');
  } else {
    bar = text;                                  // the icon column already shows ■ / ⚠
  }
  sendProgEnd(kind, text, bar, kind === 'done' ? 6000 : 9000);
}
function sendProgFail(msg) {
  sendProgEnd('error', 'ошибка: ' + msg, msg || 'ошибка отправки', 9000);
}
function sendBarStop() {                 // tone photo: stop after the current pass
  if (!streamRunning) return;
  streamStop = true; vsAbort();
  logI('Останавливаю отправку после текущего прохода…');
}

// Binary VSTREAM stop escapes. The second byte is intentionally different:
// [CC DD] is video/animation stop, so firmware restores the screensaver.
// [CC DE] is still-photo stop, so firmware powers down HV/fast BLE but leaves
// the final photo on screen with the screensaver disabled.
const VS_STOP_VIDEO = new Uint8Array([0xCC, 0xDD]);
const VS_STOP_PHOTO = new Uint8Array([0xCC, 0xDE]);

const VS_TYPE_RAW  = 0;
const VS_TYPE_RLE  = 1;
const VS_TYPE_DRLE = 2;
const VS_TYPE_RAW2  = 3;
const VS_TYPE_RLE2  = 4;
const VS_TYPE_DRLE2 = 5;
const VS_CRC_FLAG   = 0x40;  // OR into type byte → frame carries a CRC-16 trailer
const VS_HALF_FLAG  = 0x20;  // OR into type byte → frame is (W/2)×(H/2), device pixel-doubles (fw 3.4.7+)

// Host-uploaded drive tables for video (same as vstream.py's DRIVE_LUTS):
// black = VSH1, white = VSL for (ta+tb) subframes, no red, VCOM idle. Sent
// with LUTW: and selected by VSTREAM:start:CUSTOM (fw 3.4.7+).
function makeDriveLut(ta, tb = 0) {
  const lut = new Uint8Array(70);
  lut[0] = 0x55;              // LUT0 black: Ph0 = VSH1
  lut[7] = 0xAA;              // LUT1 white: Ph0 = VSL
  lut[35] = ta; lut[36] = tb; // Ph0 timing: TA, TB subframes
  return lut;
}
const DRIVE_LUTS = { video: makeDriveLut(0x08), fast: makeDriveLut(0x05, 0x05) };
const VS_KEY_INTERVAL = 48;  // force a full keyframe at least every N frames
const VS_KEY_SLACK    = 64;  // prefer a keyframe when full ≤ delta + this many bytes

const LUT_SZ   = 70;
const N_GROUPS = 5;
const N_PHASES = 7;
const FW_CHUNK = 96;   // bytes per FW:/RW: line; the firmware's RX line buffer is 256 chars

const GRP_LABELS = ['BLACK (00)','WHITE (01)','RED (10)','RED (11)','VCOM (!)'];
const GRP_COLORS = ['#d4d4d4','#98c379','#e06c75','#e06c75','#556666'];

const FACTORY_LUT = new Uint8Array([
  0x22,0x11,0x10,0x00,0x10,0x00,0x00,  // VS LUT0 BLACK
  0x11,0x88,0x80,0x80,0x80,0x00,0x00,  // VS LUT1 WHITE
  0x6A,0x9B,0xA8,0x9B,0x9B,0xFF,0xFF,  // VS LUT2 RED
  0x6A,0x9B,0xA8,0x9B,0x9B,0xFF,0xFF,  // VS LUT3 RED
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // VS LUT4 VCOM
  0x00,0x14,0x00,0x12,0x01,  // Ph0 timing
  0x06,0x06,0x06,0x06,0x02,  // Ph1
  0x14,0x14,0x14,0x14,0x01,  // Ph2
  0x00,0x00,0x00,0x00,0x00,  // Ph3
  0x00,0x00,0x04,0x3B,0x07,  // Ph4
  0x14,0x14,0x14,0x3B,0x00,  // Ph5
  0x14,0x14,0x14,0x3B,0x00,  // Ph6
]);

const ANIM_LUT = new Uint8Array([
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x60,0x00,0x00,0x00,0x00,0x00,0x00,
  0x90,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x01,0x03,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,
]);

// ═══════════════════════════════════════════════════════════════════
//  App state
// ═══════════════════════════════════════════════════════════════════
let bleDevice=null, rxChar=null, txChar=null;
let connected=false, mtu=498; // optimistic (firmware ATT MTU 498); shrinks on InvalidModificationError
let rxBuf='';
let lut = new Uint8Array(FACTORY_LUT);
let lgetBuf = Array(7).fill(null);
let tele = {fap:null, fast:null, full:null, lut_name:null};
let lineWaiters = [];

// VStream — pipelined session (mirrors vstream.py): up to vs.window frames may
// be in flight; every TELE:vs ACK returns one credit. Counting ACKs instead of
// a single buffered slot makes late/duplicate ACKs harmless (the old single-slot
// scheme lost ACKs after a resync raced a delayed ACK → cascading timeouts).
let streamRunning=false, streamStop=false;
let streamSendCancel=null; // cancels stuck sendBytesStream()
const vs = {
  window: 3,
  inflight: 0,
  acked: 0,
  lastMs: 0,
  ackTimes: [],        // rolling timestamps (≤21) → cyc = avg ACK period
  dispHist: [],        // rolling device ms (≤20) → display share of the cycle
  creditWaiters: [],
  drainWaiters: [],
  readyWaiters: [],
  devCrc: false,       // device advertised CRC support (crc=opt in ready line)
  useCrc: false,       // emit CRC-protected frames this session
  resync: false,       // device asked for a keyframe (rs=1 in an ACK)
  resyncCount: 0,
  lastWc: null,        // last ACK wire-CRC verdict: 'ok' | 'bad' | 'na' | null
  lastRs: 0,           // last ACK resync flag
};
function vsReset() {
  vs.inflight=0; vs.acked=0; vs.lastMs=0;
  vs.ackTimes=[]; vs.dispHist=[];
  vs.creditWaiters=[]; vs.drainWaiters=[]; vs.readyWaiters=[];
  vs.resync=false; vs.resyncCount=0; vs.lastWc=null; vs.lastRs=0;
  vs.devCrc=false; vs.useCrc=false; vs.half=false;
  vs.shown=0;   // ACKs with wc≠bad and rs=0: frames the panel actually displayed
}
function vsOnAck(line) {
  const ack = Proto.parseVsAck(line);
  vs.lastMs = ack.ms; vs.lastWc = ack.wc; vs.lastRs = ack.rs;
  if (vs.lastRs) { vs.resync = true; vs.resyncCount++; }
  vs.acked++;
  // The device ACKs every frame it decoded, including ones it refused to show
  // (wire-CRC bad, or a delta while poisoned until the next keyframe) — those
  // are the "frozen" moments a viewer sees. Count what was shown separately.
  if (vs.lastWc !== 'bad' && !vs.lastRs) vs.shown = (vs.shown || 0) + 1;
  vs.ackTimes.push(performance.now()); if (vs.ackTimes.length>21) vs.ackTimes.shift();
  vs.dispHist.push(vs.lastMs);          if (vs.dispHist.length>20) vs.dispHist.shift();
  if (vs.inflight>0) vs.inflight--;
  const w = vs.creditWaiters.shift();
  if (w) w(true);
  if (vs.inflight===0) { vs.drainWaiters.forEach(f=>f()); vs.drainWaiters=[]; }
}
function vsOnReady() {
  vs.readyWaiters.forEach(f=>f()); vs.readyWaiters=[];
}
// Resolve false on timeout/abort, true when a slot is available.
function vsAcquireCredit(timeoutMs=15000) {
  if (vs.inflight < vs.window) { vs.inflight++; return Promise.resolve(true); }
  return new Promise(res => {
    let done=false;
    const fn = ok => { if(!done){ done=true; clearTimeout(t); if(ok) vs.inflight++; res(ok); } };
    const t = setTimeout(() => {
      const i = vs.creditWaiters.indexOf(fn);
      if (i>=0) vs.creditWaiters.splice(i,1);
      fn(false);
    }, timeoutMs);
    vs.creditWaiters.push(fn);
  });
}
function vsDrain(timeoutMs=15000) {
  if (vs.inflight===0) return Promise.resolve(true);
  return new Promise(res => {
    let done=false;
    const fn = () => { if(!done){ done=true; clearTimeout(t); res(true); } };
    const t = setTimeout(() => {
      const i = vs.drainWaiters.indexOf(fn);
      if (i>=0) vs.drainWaiters.splice(i,1);
      if (!done){ done=true; res(false); }
    }, timeoutMs);
    vs.drainWaiters.push(fn);
  });
}
function vsWaitReady(timeoutMs=10000) {
  return new Promise(res => {
    let done=false;
    const fn = () => { if(!done){ done=true; clearTimeout(t); res(true); } };
    const t = setTimeout(() => {
      const i = vs.readyWaiters.indexOf(fn);
      if (i>=0) vs.readyWaiters.splice(i,1);
      if (!done){ done=true; res(false); }
    }, timeoutMs);
    vs.readyWaiters.push(fn);
  });
}
function vsAbort() { // wake every waiter so loops can exit promptly on Stop
  vs.creditWaiters.splice(0).forEach(f=>f(false));
  vs.drainWaiters.splice(0).forEach(f=>f());
  vs.readyWaiters.splice(0).forEach(f=>f());
}
function vsCycleMs() {
  const t = vs.ackTimes;
  return t.length>=2 ? (t[t.length-1]-t[0])/(t.length-1) : 0;
}
function vsDispMs() {
  const d = vs.dispHist;
  return d.length ? d.reduce((a,b)=>a+b,0)/d.length : 0;
}
let animVideoEl=null, animDecoder=null;
let animTotalFrames=0, animFileType=null, animImgEl=null;
let animPreEncoded=null; // Full-frame cache, filled only after background encode reaches the end
let animFrameProducer=null; // Bounded background video encoder for the current stream
let animScreenStream=null; // MediaStream from getDisplayMedia

// ═══════════════════════════════════════════════════════════════════
//  Utility
// ═══════════════════════════════════════════════════════════════════
const $ = id => document.getElementById(id);
const sleep = ms => new Promise(r => setTimeout(r, ms));
const hexB = b => b.toString(16).padStart(2,'0').toUpperCase();
const hexOf = a => Array.from(a).map(b=>b.toString(16).padStart(2,'0')).join('');
const clamp = (v,lo,hi) => Math.max(lo, Math.min(hi, v));
const esc = s => s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');

// ═══════════════════════════════════════════════════════════════════
//  BLE
// ═══════════════════════════════════════════════════════════════════
// GATT-коннект с повторами: nRF52 часто промахивается с первого раза
// (узкое окно advertising, сон, перестройка стека после разрыва)
async function gattConnectRetry(device, tries = 3) {
  for (let i = 1; ; i++) {
    try {
      setStatus(`Подключение… попытка ${i}/${tries}`, false, 'busy');
      return await device.gatt.connect();
    } catch (e) {
      logE(`GATT (${i}/${tries}): ${e.message}`);
      if (i >= tries) throw e;
      try { device.gatt.disconnect(); } catch {}
      await sleep(500 * i);
    }
  }
}

async function bleConnect() {
  if (connected) { bleDevice && bleDevice.gatt.disconnect(); return; }
  otaReconnectCancel();          // a manual connect takes over from the post-OTA wait
  if (!navigator.bluetooth) {
    alert('Web Bluetooth не поддерживается.\nОткрой в Chrome и разреши bluetooth.');
    return;
  }
  const name = $('dev-name').value.trim();
  // Полное имя «Имя (КОД)» уникально (несъёмный 6-hex код) → точный фильтр
  // сводит chooser к одному ценнику. Так приходит deep-link из NFC-метки.
  // Иначе: имя/префикс ИЛИ сервис NUS (находит и переименованные); пусто = по NUS.
  let filters;
  // Точный фильтр (ровно один ценник), когда имя пришло из метки, либо это
  // полное имя «Имя (КОД)» с несъёмным кодом.
  if (name && (name === deepLinkExactName || /^.+ \([0-9A-Fa-f]{6}\)$/.test(name))) {
    filters = [{name}];
  } else if (name) {
    filters = [name.endsWith('*') ? {namePrefix: name.slice(0, -1)} : {name},
               {services: [NUS_SVC]}];
  } else {
    filters = [{services: [NUS_SVC]}];
  }
  setStatus('Сканирую…', false, 'busy');
  try {
    bleDevice = await navigator.bluetooth.requestDevice({
      filters,
      optionalServices: [NUS_SVC, SMP_SVC_UUID],
    });
    bleDevice.addEventListener('gattserverdisconnected', onBleDisc);
    await bleAttach(bleDevice);
  } catch(e) {
    // Closing the device chooser is a normal outcome, not a failure to shout about.
    const cancelled = /cancel/i.test(e.message);
    setStatus(cancelled ? 'Не подключено' : 'Ошибка: ' + e.message, false, cancelled ? 'idle' : 'err');
    if (cancelled) { logI('Выбор ценника отменён'); return; }
    logE(e.message);
    if (/Connection attempt failed|GATT operation|not connected|range/i.test(e.message)) {
      logI('Частые причины и что сделать:');
      logI('· ценник уже занят другим окном — закрой прочие вкладки и установленное приложение (смахни из недавних), затем повтори');
      logI('· устройство спит между advertising-окнами — разбуди его (кнопка/перезагрузка питания) и подключайся сразу');
      logI('· выключи и включи Bluetooth на телефоне; если ценник «сопряжён» в системных настройках — удали сопряжение');
    }
  }
}

// Everything after the chooser: GATT, NUS, status, first commands. Shared by
// the Connect button and the post-OTA reconnect (same device object, no
// chooser; quiet = single connect attempt, the caller loops).
async function bleAttach(device, opts = {}) {
  const server  = opts.quiet ? await device.gatt.connect() : await gattConnectRetry(device);
  const service = await server.getPrimaryService(NUS_SVC);
  rxChar = await service.getCharacteristic(NUS_RX);
  txChar = await service.getCharacteristic(NUS_TX);
  await txChar.startNotifications();
  txChar.addEventListener('characteristicvaluechanged', onNotify);
  connected = true;
  autoTimeDoneThisConn = false;   // новая сессия → снова можно авто-выставить время
  sysState.wallUnix = null;
  mtu = 498; // reset to optimistic; sendBytes will shrink on first oversized write
  updateMtuDisplay();
  setStatus(`Подключено → ${device.name}`, true);
  $('btn-conn').textContent = 'Отключить';
  currentDeviceName = device.name || '';   // стартовое имя; NAME: уточнит/обновит
  nfcRefresh();   // имя ценника теперь известно → построить ссылку для метки
  logI(`Connected to ${device.name}`);
  await sleep(400);
  sendRaw_('HOST:1');
  onSysConnect();
}

// Освобождаем ценник при закрытии окна/приложения,
// иначе фоновая PWA держит единственный слот подключения nRF52
window.addEventListener('pagehide', () => {
  try { if (bleDevice && bleDevice.gatt.connected) bleDevice.gatt.disconnect(); } catch {}
});

function onBleDisc() {
  connected = false;
  smpChar = null;
  if (smpPendingReject) smpPendingReject(new Error('связь с ценником оборвалась'));
  sysState.authed = false; sysState.secOn = false;
  sysState.picture = null; dfuSilentTouched = false;
  sysState.wallUnix = null; autoTimeDoneThisConn = false;
  autoAuthTried = false; authBusy = false;
  setStatus('Не подключено', false);
  $('btn-conn').textContent = 'Подключить';
  liveStop();                    // a dropped link ends any stream indicator
  battChip(null);                // the charge belongs to the tag we just lost
  currentDeviceName = '';
  nfcRefresh();
  // Не тащим статус приёма на следующий ценник: у него он может быть другим,
  // а старая прошивка на MESHRX вообще не ответит.
  const mrx = $('sys-meshrx');
  if (mrx) { mrx.textContent = '—'; mrx.style.color = 'var(--fg2)'; }
  for (const id of [...Object.values(STAT_IDS), 'st-flash']) {
    const el = $(id); if (el) { el.textContent = '—'; if (id === 'st-flash') el.style.color = 'var(--fg2)'; }
  }
  pwrReset();
  logI('Disconnected');
  // OTA: an expected drop after REBOOT → dial back in; an offer belonged to the tag we lost.
  if (ota.phase === 'reboot') { if (!otaReconnActive) otaReconnect(); }
  else if (ota.phase === 'avail') otaSet('idle');
  else otaRender();
}

function onNotify(ev) {
  rxBuf += new TextDecoder().decode(ev.target.value);
  let nl;
  while ((nl = rxBuf.indexOf('\n')) >= 0) {
    const line = rxBuf.slice(0, nl).trim();
    rxBuf = rxBuf.slice(nl + 1);
    if (!line) continue;
    logR(line);
    wakeLineWaiters(line);
    if (line.startsWith('TELE:') || line.startsWith('STAT:')) parseTele(line);
    if (line.startsWith('STATS:')) parseStats(line);
    if (line.startsWith('SYSINFO:')) parseSysinfo(line);
    if (line.startsWith('FW:rx') || line.startsWith('RW:rx')) sendProgRx(line);
    if (line.startsWith('FAPPLY done') || line.startsWith('TELE:fapply')) {
      if (sendProg.phase === 'refresh') sendProgDone(`готово · ${line.replace(/^TELE:fapply /, '')}`);
    }
    if (line.startsWith('NAME:')) parseName(line);
    if (line.startsWith('meshrx=')) parseMeshrx(line);
    if (line.startsWith('PWR:')) parsePwr(line);
    if (line.startsWith('PWRB:')) parsePwr(line);
    if (line.startsWith('BUSY:dfu')) logI('Ценник занят обновлением — команда отложена');
    if (line.startsWith('BUSY:cmd')) logI('Ценник ещё выполняет предыдущие команды — эта не принята, повтори позже');
    if (line.startsWith('AUTH:')) handleAuthLine(line);
    if (line.startsWith('BATT:')) parseBattAlert(line);   // LOW / SHUTDOWN / OK
    if (line.startsWith('LUT:') && line[5] === ':') parseLget(line);
    if (line.startsWith('TELE:vs')) vsOnAck(line);
    if (line.startsWith('VSTREAM:')) {
      logI(line);
      if (line.startsWith('VSTREAM:ready')) {
        vs.devCrc = Proto.parseVsReady(line).crc;
        vs.useCrc = vs.devCrc;   // enable wire CRC when the device supports it
        vsOnReady();
      }
    }
  }
}

function waitForLine(match, timeoutMs=10000) {
  return new Promise(resolve => {
    let done = false;
    const fn = line => {
      if (done) return false;
      const ok = typeof match === 'function' ? match(line) : match.test(line);
      if (!ok) return false;
      done = true;
      clearTimeout(timer);
      resolve(line);
      return true;
    };
    const timer = setTimeout(() => {
      if (done) return;
      done = true;
      const i = lineWaiters.indexOf(fn);
      if (i >= 0) lineWaiters.splice(i, 1);
      resolve(null);
    }, timeoutMs);
    lineWaiters.push(fn);
  });
}

function wakeLineWaiters(line) {
  for (const fn of lineWaiters.slice()) {
    if (!fn(line)) continue;
    const i = lineWaiters.indexOf(fn);
    if (i >= 0) lineWaiters.splice(i, 1);
  }
}

function updateMtuDisplay() {
  const label = `MTU ${mtu} (payload ${mtu-3}B)`;
  const e1 = $('mtu-txt'), e2 = $('tb-mtu');
  if (e1) e1.textContent = label;
  if (e2) e2.textContent = label;
}

// Parallel chunk writes (Promise.all of writeValueWithoutResponse) are fast but
// race on flaky controllers: Web Bluetooth rejects overlapping ops with "GATT
// operation already in progress", and a partial-success fallback re-sends the
// whole packet → duplicate bytes that desync the device parser. Single-chunk
// packets (every DRLE delta) gain nothing from it; only multi-chunk keyframes/
// RAW do, and those are exactly the racy case. Default OFF → sequential writes,
// matching the proven bleak/vstream.py host. Flip to true only on a solid stack.
let _bleParallelWriteSupported = false;

async function sendBytes(data, noResponse=false, abortRef=null) {
  if (!connected || !rxChar) return;
  const buf = data instanceof Uint8Array ? data : new Uint8Array(data);
  let chunkSz = mtu - 3;
  if (noResponse && _bleParallelWriteSupported) {
    // Streaming fast path: fire all chunks as quickly as possible without
    // yielding the event loop between them. This queues them in Chrome's BLE
    // write buffer so they land in consecutive connection events — matching
    // bleak's tight write loop.
    const chunks = [];
    for (let i = 0; i < buf.length; i += chunkSz) {
      chunks.push(buf.slice(i, Math.min(i + chunkSz, buf.length)));
    }
    let retry = false;
    let promises = [];
    try {
      promises = chunks.map(chunk => rxChar.writeValueWithoutResponse(chunk));
      await Promise.all(promises);
      return; // success
    } catch (e) {
      // Promise.all rejects on the FIRST failing chunk but leaves the other
      // chunk writes in flight. Let them all settle before any sequential retry,
      // otherwise the retry races them → another "GATT operation already in
      // progress" that the sequential path would re-throw and kill the stream.
      await Promise.allSettled(promises);
      if (e.name === 'InvalidModificationError' && chunkSz > 20) {
        chunkSz = chunkSz > 244 ? 244 : Math.max(20, chunkSz >> 1);
        mtu = chunkSz + 3;
        updateMtuDisplay();
        retry = true;
      } else if (e.name === 'NetworkError' || e.name === 'NotSupportedError'
                 || e.message?.includes('GATT operation already in progress')) {
        // Browser doesn't support parallel writes; disable permanently & fall through
        _bleParallelWriteSupported = false;
        retry = true;
      } else {
        throw e;
      }
    }
    if (!retry) return;
    // Fall through to sequential
  }
  if (noResponse) {
    let gattBusyRetries = 0;
    for (let i = 0; i < buf.length; ) {
      if (abortRef?.abort) throw new Error(abortRef.reason ?? 'stopped');
      const sz = Math.min(chunkSz, buf.length - i);
      const chunk = buf.slice(i, i + sz);
      try {
        await rxChar.writeValueWithoutResponse(chunk);
        i += sz;
      } catch (e) {
        if (e.name === 'InvalidModificationError' && chunkSz > 20) {
          chunkSz = chunkSz > 244 ? 244 : Math.max(20, chunkSz >> 1);
          mtu = chunkSz + 3;
          updateMtuDisplay();
        } else if (e.message?.includes('GATT operation already in progress') && gattBusyRetries < 30) {
          // Transient: a previous write hasn't drained on a flaky controller.
          // Back off briefly and retry the SAME chunk instead of killing the
          // stream — the next keyframe/photo pass recovers cleanly.
          gattBusyRetries++;
          await sleep(8);
        } else {
          throw e;
        }
      }
    }
  } else {
    for (let i = 0; i < buf.length; ) {
      if (abortRef?.abort) throw new Error(abortRef.reason ?? 'stopped');
      const sz = Math.min(chunkSz, buf.length - i);
      const chunk = buf.slice(i, i + sz);
      try {
        await rxChar.writeValueWithResponse(chunk);
        i += sz;
      } catch (e) {
        if (e.name === 'InvalidModificationError' && chunkSz > 20) {
          chunkSz = chunkSz > 244 ? 244 : Math.max(20, chunkSz >> 1);
          mtu = chunkSz + 3;
          updateMtuDisplay();
        } else {
          throw e;
        }
      }
    }
  }
}

// sendBytes wrapper for streaming: truly cancellable via abortRef checked between chunks.
// 8s total timeout + Stop button both set abortRef.abort, so sendBytes exits at next chunk boundary.
async function sendBytesStream(data) {
  const abortRef = { abort: false, reason: null };
  let cancelFn;
  const timer = setTimeout(() => {
    if (!abortRef.abort) { abortRef.abort = true; abortRef.reason = 'BLE send timeout'; }
  }, 8000);
  streamSendCancel = cancelFn = () => {
    if (!abortRef.abort) { abortRef.abort = true; abortRef.reason = 'stopped'; }
  };
  try {
    await sendBytes(data, true, abortRef);
  } finally {
    clearTimeout(timer);
    if (streamSendCancel === cancelFn) streamSendCancel = null;
  }
}

function send(cmd) {
  sendBytes(new TextEncoder().encode(cmd)).catch(e => logE(e.message));
  logT(cmd.trim());
}

function sendRaw_(cmd) {
  send(cmd + '\n');
}

// ═══════════════════════════════════════════════════════════════════
//  Status
// ═══════════════════════════════════════════════════════════════════
// Connection state drives the header pill, the dot and the Connect button.
// state: idle (grey) · busy (amber, scanning/connecting) · on (green) · err (red).
// Legacy calls pass (msg, bool); the state is then derived from the bool.
function setStatus(msg, on, state) {
  const st = state || (on ? 'on' : 'idle');
  $('st-txt').textContent = msg;
  $('dot').className = 'dot' + (st === 'on' ? ' on' : st === 'idle' ? '' : ' ' + st);
  const pill = $('conn-pill'); if (pill) pill.className = 'conn ' + st;
  const btn = $('btn-conn');
  if (btn) {                       // no double-tap while the chooser/GATT is busy
    btn.disabled = st === 'busy';
    btn.textContent = st === 'busy' ? 'Подключение…' : (connected ? 'Отключить' : 'Подключить');
  }
  // The device filter only matters before connecting — reclaim its row on phones
  // so the header stays two lines with the battery/stream chips in place.
  const gear = $('hdr-more-btn'), row = $('hdr-more');
  if (gear) gear.hidden = connected;
  if (row && connected && isPhoneLayout()) {
    row.hidden = true;
    if (gear) gear.setAttribute('aria-expanded', 'false');
  }
}

// ── Live indicators: stream chip, battery chip, update badge ────────
// The stream chip polls vs state instead of hooking the hot send loop.
let liveTimer = null;
function vsFps() {
  const cyc = vsCycleMs();
  const okShare = vs.acked ? (vs.shown || 0) / vs.acked : 1;
  return cyc > 0 ? (1000 / cyc) * okShare : 0;
}
function liveStart() {
  const chip = $('live-chip'), nav = $('nav-anim');
  if (chip) chip.hidden = false;
  if (nav) nav.classList.add('live');
  clearInterval(liveTimer);
  const tick = () => {
    if (!chip) return;
    const fps = vsFps();
    chip.textContent = '● Стрим' + (fps > 0.05 ? ` ${fps.toFixed(1)} fps` : '');
  };
  tick();
  liveTimer = setInterval(tick, 700);
}
function liveStop() {
  clearInterval(liveTimer); liveTimer = null;
  const chip = $('live-chip'), nav = $('nav-anim');
  if (chip) chip.hidden = true;
  if (nav) nav.classList.remove('live');
}
function battChip(pct, mv, low) {
  const el = $('batt-chip');
  if (!el) return;
  if (pct == null) { el.hidden = true; return; }
  el.hidden = false;
  el.textContent = `🔋 ${pct}%`;
  el.title = `Батарея ценника: ${mv} mV`;
  el.className = low || pct <= 20 ? 'bad' : pct <= 50 ? 'warn' : '';
}
function navBadge(id, on) {
  const el = $(id); if (el) el.classList.toggle('badge', !!on);
}
// The preview starts blank; the hint disappears once anything is drawn on it.
function dispEmpty(show) {
  const el = $('disp-empty'); if (el) el.style.display = show ? '' : 'none';
}

// ═══════════════════════════════════════════════════════════════════
//  Logging
// ═══════════════════════════════════════════════════════════════════
function appendLog(el, cls, pre, msg) {
  const ts = new Date().toTimeString().slice(0,8);
  const d = document.createElement('div');
  d.innerHTML = `<span class="ts">${ts}</span> <span class="${cls}">${pre} ${esc(msg)}</span>`;
  el.appendChild(d);
  // Defer scroll to avoid forced synchronous reflow in hot paths (streaming ACKs).
  // RAF batches all pending scrolls into a single frame.
  if (!el._scrollPending) {
    el._scrollPending = true;
    requestAnimationFrame(() => {
      el._scrollPending = false;
      el.scrollTop = el.scrollHeight;
    });
  }
  while (el.children.length > 500) el.removeChild(el.firstChild);
}
function logR(m) {
  // During streaming, suppress all received-line logging to avoid DOM thrashing.
  // TELE:vs ACKs fire at ~7Hz and each DOM insert delays the credit-wake path.
  if (streamRunning) return;
  appendLog($('main-con'),'rx','←',m);
}
function logT(m) { appendLog($('main-con'),'tx','→',m); }
function logI(m) { appendLog($('main-con'),'inf','·',m); }
function logE(m) { appendLog($('main-con'),'err','!',m); uiToast(m, 'err'); }
function animLog(m){ appendLog($('anim-log'),'inf','·',m); }

// ═══════════════════════════════════════════════════════════════════
//  Telemetry parsing
// ═══════════════════════════════════════════════════════════════════
function parseTele(line) {
  // During streaming, TELE:vs ACKs should not trigger full telemetry DOM updates
  if (streamRunning && line.startsWith('TELE:vs')) return;
  const r = Proto.parseTele(line);
  if (r.fapply) tele.fap = r.fapply;
  if (r.fast)   tele.fast = r.fast;
  if (r.full)   tele.full = r.full;
  if (r.lut)    tele.lut_name = r.lut;
  renderTele();
}

function renderTele() {
  const f = v => v!=null ? v+'ms' : '—';
  $('tb-fap').textContent  = f(tele.fap);
  $('tb-fast').textContent = f(tele.fast);
  $('tb-upd').textContent  = f(tele.full);
  $('tb-lut').textContent  = tele.lut_name || '—';
  $('tt-fast').textContent = f(tele.fast);
  $('tt-fap').textContent  = f(tele.fap);
  $('tt-upd').textContent  = f(tele.full);
}

function parseLget(line) {
  const r = Proto.parseLgetLine(line);   // LUT:N:XXXXXXXXXXXXXXXXXXXX, seven rows
  if (!r) return;
  lgetBuf[r.idx] = r.hex;
  if (lgetBuf.some(s=>s===null)) return;
  const hex = lgetBuf.join('');
  lgetBuf = Array(7).fill(null);
  if (hex.length !== LUT_SZ*2) { logE('LGET: bad length '+hex.length); return; }
  for (let i=0; i<LUT_SZ; i++) lut[i] = parseInt(hex.slice(i*2, i*2+2), 16);
  lutToUI(); drawWf();
  logI('[LGET] LUT загружен с устройства');
}

// ═══════════════════════════════════════════════════════════════════
//  Device commands
// ═══════════════════════════════════════════════════════════════════
// Время + дата одной командой. Раньше кнопка слала «TIME=HH:MM:SS», который
// оставляет на ценнике прежнюю дату — а она после сброса стоит на дате сборки,
// потому что часы отсчитываются от неё и потерю питания не переживают. Так что
// отдельная «установка только времени» смысла не имела: дату всё равно надо
// выставлять при каждом сбросе.
//
// Прошивка хранит компоненты «как есть» (mktime/gmtime без TZ), поэтому шлём
// ЛОКАЛЬНЫЕ числа — на экране ценника они и отобразятся как местное время.
function sendFullTime() {
  const n = new Date();
  sendRaw_(`TIME ${n.getHours()}:${n.getMinutes()}:${n.getSeconds()} ` +
           `${n.getDate()}.${n.getMonth() + 1}.${n.getFullYear()}`);
}
// То же, но с ответом пользователю — вешается на кнопки, в отличие от
// автоматики при подключении, которая работает молча.
function syncTime() {
  if (!connected) { uiToast('Сначала подключись к ценнику'); return; }
  const n = new Date();
  const p = v => String(v).padStart(2, '0');
  sendFullTime();
  autoTimeDoneThisConn = true;   // выставили руками — автоматике уже нечего делать
  uiToast(`Время и дата переданы: ${p(n.getHours())}:${p(n.getMinutes())} ` +
          `${p(n.getDate())}.${p(n.getMonth() + 1)}.${n.getFullYear()}`, 'ok');
}
// Часы ценника отсчитываются от build-времени, пока их кто-то не выставит, и
// НЕ переживают потерю питания. После OTA/первой загрузки они показывают build —
// тогда выставляем реальное время автоматически, без участия пользователя.
//
// Признак «никто не выставлял»: текущие часы ушли от build-времени не дальше,
// чем на uptime (+запас). Если бы время выставляли, wall был бы ≈ «сейчас», т.е.
// намного больше build+uptime. Сравнение идёт в одной шкале (mktime/gmtime на
// устройстве ≙ Date.UTC компонентов), поэтому от часового пояса браузера не зависит.
function buildDateToUnix(s) { return Proto.buildDateToUnix(s); }   // "2025-06-12_14:30:00"
function maybeAutoSetTime() {
  if (autoTimeDoneThisConn) return;
  if (!connected) return;
  if (sysState.secOn && !sysState.authed) return;   // защищён → TIME проигнорируется
  if (sysState.wallUnix == null || !sysState.buildDate) return;
  const buildUnix = buildDateToUnix(sysState.buildDate);
  if (buildUnix == null) return;

  const advanced = sysState.wallUnix - buildUnix;        // на сколько часы ушли от build
  const neverSet = advanced <= (sysState.uptimeSec + 300); // +5 мин запаса
  if (!neverSet) return;                                  // время уже выставлено вручную

  autoTimeDoneThisConn = true;
  logI('Время на ценнике не выставлено (стоит build) — выставляю текущее автоматически');
  sendFullTime();
}
function sendText() {
  const t = $('txt-inp').value.trim();
  if (!t) return;
  send(`TEXT: ${t}\n`);
  $('txt-inp').value = '';
}
function rebootDevice() {
  if (!connected) { alert('Не подключено'); return; }
  if (!confirm('Перезагрузить устройство?\n\nBLE-соединение разорвётся — переподключись вручную.')) return;
  send('REBOOT\n');
  // BLE disconnects as part of reboot — clear state after short delay
  setTimeout(() => {
    logI('Device rebooting — reconnect manually');
  }, 300);
}

function nukeDialog() {
  const n = prompt('NUKE — глубокая очистка\nЦиклов B/W/R (5-50):', '20');
  if (!n) return;
  const v = parseInt(n);
  if (v>=5&&v<=50) { markDeviceBusy(v * 45000); send(`NUKE:${v}\n`); } else alert('Введи число 5–50');
}
function sendRaw() {
  const v = $('cmd-inp').value.trim();
  if (!v) return;
  send(v + '\n');
  $('cmd-inp').value = '';
}

// ═══════════════════════════════════════════════════════════════════
//  Shell: layout probe, header "more" row, transient toast
// ═══════════════════════════════════════════════════════════════════
const isPhoneLayout = () => window.matchMedia('(max-width: 900px)').matches;

function hdrMore() {
  const row = $('hdr-more'), btn = $('hdr-more-btn');
  if (!row) return;
  row.hidden = !row.hidden;
  if (btn) btn.setAttribute('aria-expanded', String(!row.hidden));
}

let _uiToastTimer = null;
function uiToast(msg, kind = '', ms = 3500) {
  const el = $('ui-toast');
  if (!el) return;
  el.textContent = msg;
  el.className = kind ? kind + ' show' : 'show';
  clearTimeout(_uiToastTimer);
  _uiToastTimer = setTimeout(() => el.classList.remove('show'), ms);
}

// ═══════════════════════════════════════════════════════════════════
//  Tab switching
// ═══════════════════════════════════════════════════════════════════
let curTab = 'tag';
function tab(name, opts = {}) {
  const names = ['tag','anim','lut','sys'];
  const leavingSys = curTab === 'sys' && name !== 'sys';
  curTab = name;
  if (leavingSys) pwrStopTimers();   // the profile card is on «Система»; nothing to keep fresh elsewhere
  document.querySelectorAll('.tab').forEach((el,i) =>
    el.classList.toggle('on', names[i] === name));
  document.querySelectorAll('.tp').forEach(el =>
    el.classList.toggle('on', el.id === 'tp-'+name));
  if (isPhoneLayout() && !opts.keepScroll) window.scrollTo({ top: 0 });   // keepScroll: the caller scrolls itself
  if (name === 'lut') setTimeout(drawWf, 60);
  if (name === 'sys' && !opts.quiet) requestSysinfo();   // quiet: OTA switches here itself
}

// ═══════════════════════════════════════════════════════════════════
//  Display canvas
// ═══════════════════════════════════════════════════════════════════
function clearCanvas() {
  ieClose();
  const ctx = $('disp-canvas').getContext('2d');
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = '#ffffff';
  ctx.fillRect(0, 0, PANEL.lw, PANEL.lh);
  dispEmpty(true);
}

// ═══════════════════════════════════════════════════════════════════
//  Image editor — live preview pipeline
//  compose (fit/fill/stretch/100% + rotate + flip + pan + zoom)
//  → adjust (gamma / contrast / brightness)
//  → dither (error-diffusion / ordered / threshold) + BWR red layer
// ═══════════════════════════════════════════════════════════════════
const IE = { img:null, rot:0, zoom:100, x:0, y:0, fh:false, fv:false, raw:false, oc:null };

// Error-diffusion kernels: t = [dx, dy, weight], d = divisor
const IE_KERNELS = {
  fs:        { d:16, t:[[1,0,7],[-1,1,3],[0,1,5],[1,1,1]] },
  atkinson:  { d:8,  t:[[1,0,1],[2,0,1],[-1,1,1],[0,1,1],[1,1,1],[0,2,1]] },
  jjn:       { d:48, t:[[1,0,7],[2,0,5],[-2,1,3],[-1,1,5],[0,1,7],[1,1,5],[2,1,3],[-2,2,1],[-1,2,3],[0,2,5],[1,2,3],[2,2,1]] },
  stucki:    { d:42, t:[[1,0,8],[2,0,4],[-2,1,2],[-1,1,4],[0,1,8],[1,1,4],[2,1,2],[-2,2,1],[-1,2,2],[0,2,4],[1,2,2],[2,2,1]] },
  burkes:    { d:32, t:[[1,0,8],[2,0,4],[-2,1,2],[-1,1,4],[0,1,8],[1,1,4],[2,1,2]] },
  sierra:    { d:32, t:[[1,0,5],[2,0,3],[-2,1,2],[-1,1,4],[0,1,5],[1,1,4],[2,1,2],[-1,2,2],[0,2,3],[1,2,2]] },
  sierra2:   { d:16, t:[[1,0,4],[2,0,3],[-2,1,1],[-1,1,2],[0,1,3],[1,1,2],[2,1,1]] },
  sierralite:{ d:4,  t:[[1,0,2],[-1,1,1],[0,1,1]] },
};
const IE_BAYER = {
  bayer2: [[0,2],[3,1]],
  bayer4: [[0,8,2,10],[12,4,14,6],[3,11,1,9],[15,7,13,5]],
  bayer8: [[0,32,8,40,2,34,10,42],[48,16,56,24,50,18,58,26],[12,44,4,36,14,46,6,38],
           [60,28,52,20,62,30,54,22],[3,35,11,43,1,33,9,41],[51,19,59,27,49,17,57,25],
           [15,47,7,39,13,45,5,37],[63,31,55,23,61,29,53,21]],
};

function loadImg(file) {
  if (!file) return;
  const url = URL.createObjectURL(file);
  const img = new Image();
  img.onload = () => {
    URL.revokeObjectURL(url);
    IE.img = img;
    ieReset(false);
    $('img-ed').style.display = 'block';
    $('disp-canvas').classList.add('grabby');
    renderImg();
  };
  img.onerror = () => { URL.revokeObjectURL(url); logE('Не удалось открыть изображение'); };
  img.src = url;
}

function ieClose() {
  IE.img = null;
  $('img-ed').style.display = 'none';
  $('disp-canvas').classList.remove('grabby');
}

function ieReset(render = true) {
  IE.rot = 0; IE.zoom = 100; IE.x = 0; IE.y = 0; IE.fh = false; IE.fv = false;
  $('ie-rot').value = 0; $('ie-zoom').value = 100;
  $('ie-bri').value = 0; $('ie-con').value = 0; $('ie-gam').value = 100;
  if (render) renderImg();
}

function ieRot(d) {
  IE.rot = ((IE.rot + d + 540) % 360) - 180;
  $('ie-rot').value = IE.rot;
  renderImg();
}

function ieFlip(axis) {
  if (axis === 'h') IE.fh = !IE.fh; else IE.fv = !IE.fv;
  renderImg();
}

function currentOutputMode() {
  const top = $('output-mode');
  const ed = $('ie-preview');
  const v = ((ed && ed.value) || (top && top.value) || 'dither');
  return (v === 'tone' || v === 'tone-soft') ? v : 'dither';
}

function showEl(id, show, display='') {
  const el = $(id);
  if (el) el.style.display = show ? display : 'none';
}

function setOutputMode(mode, rerender=true) {
  const m = (mode === 'tone' || mode === 'tone-soft') ? mode : 'dither';
  const isTone     = m === 'tone';
  const isSoft     = m === 'tone-soft';
  const isToneAny  = isTone || isSoft;

  if ($('output-mode')) $('output-mode').value = m;
  if ($('ie-preview'))  $('ie-preview').value  = m;

  showEl('send-bwr',               !isToneAny);
  showEl('tone-send-controls',     isTone,    'inline-flex');
  showEl('tone-soft-send-controls',isSoft,    'inline-flex');
  showEl('ie-dither-row',          !isToneAny);
  showEl('ie-thr-row',             !isToneAny);
  showEl('ie-red-opt',             !isToneAny, 'flex');
  showEl('ie-send-bwr',            !isToneAny);
  showEl('ie-send-tone',           isTone);
  showEl('ie-send-tone-soft',      isSoft);

  if (rerender && IE.img) renderImg();
}

function currentAnimRenderMode() {
  const el = $('enc-render');
  const v = el ? el.value : 'mono';
  return (v === 'tone-servo' || v === 'tone-temporal') ? v : 'mono';
}

function setAnimRenderMode(mode) {
  const m = (mode === 'tone-servo' || mode === 'tone-temporal') ? mode : 'mono';
  const tone = m !== 'mono';
  if ($('enc-render')) $('enc-render').value = m;
  showEl('enc-tone-row', tone, 'flex');
  showEl('enc-lut-box', !tone, 'flex');
  showEl('enc-dith-label', !tone, 'flex');
  showEl('enc-tone-servo-box', m === 'tone-servo', 'flex');
  if (tone && $('enc-tone-lut')) {
    $('enc-tone-lut').value = $('enc-tone-lut').value || 'TONE_BIDIR_FAST';
  }
}

// Read all controls into state + refresh value labels, then re-render
function ieSet() {
  IE.rot  = +$('ie-rot').value;
  IE.zoom = +$('ie-zoom').value;
  $('iev-rot').textContent  = IE.rot + '°';
  $('iev-zoom').textContent = IE.zoom + '%';
  $('iev-bri').textContent  = $('ie-bri').value;
  $('iev-con').textContent  = $('ie-con').value;
  $('iev-gam').textContent  = (+$('ie-gam').value / 100).toFixed(2);
  $('iev-thr').textContent  = $('ie-thr').value;
  setOutputMode(currentOutputMode(), false);
  renderImg();
}

function renderImg() {
  if (!IE.img) return;
  dispEmpty(false);
  const cv = $('disp-canvas'), ctx = cv.getContext('2d');
  // 1) Compose on offscreen canvas
  const LW = PANEL.lw, LH = PANEL.lh;
  const oc = IE.oc || (IE.oc = document.createElement('canvas'));
  oc.width = LW; oc.height = LH;
  const o = oc.getContext('2d');
  o.fillStyle = $('ie-bg').value;
  o.fillRect(0, 0, LW, LH);
  o.imageSmoothingEnabled = $('ie-smooth').checked;
  o.imageSmoothingQuality = 'high';
  const iw = IE.img.width, ih = IE.img.height;
  let sx, sy;
  switch ($('ie-mode').value) {
    case 'fill':    sx = sy = Math.max(LW/iw, LH/ih); break;
    case 'stretch': sx = LW/iw; sy = LH/ih;           break;
    case 'orig':    sx = sy = 1;                      break;
    default:        sx = sy = Math.min(LW/iw, LH/ih); // fit
  }
  const z = IE.zoom / 100; sx *= z; sy *= z;
  o.save();
  o.translate(LW/2 + IE.x, LH/2 + IE.y);
  o.rotate(IE.rot * Math.PI / 180);
  o.scale(sx * (IE.fh ? -1 : 1), sy * (IE.fv ? -1 : 1));
  o.drawImage(IE.img, -iw/2, -ih/2);
  o.restore();
  // "Оригинал" — показать без обработки
  if (IE.raw) { ctx.drawImage(oc, 0, 0); return; }
  // 2) Adjust + dither
  const id = o.getImageData(0, 0, PANEL.lw, PANEL.lh);
  if (currentOutputMode() === 'tone') {
    ieProcessTonePreview(id);
  } else if (currentOutputMode() === 'tone-soft') {
    ieProcessToneSoftPreview(id);
  } else {
    ieProcess(id);
  }
  ctx.putImageData(id, 0, 0);
}

function ieProcess(id) {
  const w = PANEL.lw, h = PANEL.lh, d = id.data, n = w * h;
  const bri = +$('ie-bri').value, con = +$('ie-con').value;
  const gam = +$('ie-gam').value / 100;
  const thr = +$('ie-thr').value;
  const inv = $('ie-inv').checked, useRed = $('ie-red').checked;
  const mode = $('ie-dith').value;
  // LUT: gamma → contrast → brightness
  const cf = (259 * (con * 2.55 + 255)) / (255 * (259 - con * 2.55));
  const lut = new Uint8ClampedArray(256);
  for (let v = 0; v < 256; v++) {
    let g = 255 * Math.pow(v / 255, 1 / gam);
    lut[v] = cf * (g - 128) + 128 + bri * 1.27;
  }
  const gray = new Float32Array(n);
  const red  = useRed ? new Uint8Array(n) : null;
  for (let i = 0; i < n; i++) {
    const r = d[i*4], g = d[i*4+1], b = d[i*4+2];
    if (useRed && r > 100 && r - g > 45 && r - b > 45) { red[i] = 1; gray[i] = 255; continue; }
    gray[i] = lut[(0.299*r + 0.587*g + 0.114*b) | 0];
  }
  const out = new Uint8Array(n); // 1 = чёрный
  if (mode === 'none') {
    for (let i = 0; i < n; i++) out[i] = gray[i] < thr ? 1 : 0;
  } else if (IE_BAYER[mode]) {
    const m = IE_BAYER[mode], N = m.length, NN = N * N, bias = thr - 128;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const t = (m[y % N][x % N] + 0.5) * 255 / NN + bias;
      out[y*w + x] = gray[y*w + x] < t ? 1 : 0;
    }
  } else if (mode === 'random') {
    for (let i = 0; i < n; i++) out[i] = gray[i] < thr + (Math.random() - 0.5) * 160 ? 1 : 0;
  } else {
    // Error diffusion с серпантинным сканированием
    const k = IE_KERNELS[mode] || IE_KERNELS.fs, kd = k.d, kt = k.t;
    for (let y = 0; y < h; y++) {
      const ltr = !(y & 1);
      for (let xi = 0; xi < w; xi++) {
        const x = ltr ? xi : w - 1 - xi, i = y*w + x;
        const old = gray[i], nw = old < thr ? 0 : 255;
        out[i] = nw ? 0 : 1;
        const e = (old - nw) / kd;
        for (let t = 0; t < kt.length; t++) {
          const nx = x + (ltr ? kt[t][0] : -kt[t][0]), ny = y + kt[t][1];
          if (nx >= 0 && nx < w && ny < h) gray[ny*w + nx] += e * kt[t][2];
        }
      }
    }
  }
  for (let i = 0; i < n; i++) {
    if (red && red[i]) { d[i*4] = 200; d[i*4+1] = 0; d[i*4+2] = 0; d[i*4+3] = 255; continue; }
    const v = (out[i] === 1) !== inv ? 0 : 255;
    d[i*4] = d[i*4+1] = d[i*4+2] = v; d[i*4+3] = 255;
  }
}

function ieProcessTonePreview(id) {
  const d = id.data;
  const n = PANEL.lw * PANEL.lh;
  const lut = makeAdjustLut();
  const maxPulse    = parseInt($('tone-pulses')?.value) || 8;
  const lightPasses = parseInt($('tone-light-passes')?.value ?? 0) || 0;
  const inv         = $('ie-inv').checked;

  // Симуляция накопительного потемнения e-ink:
  // brightness *= (1 - str) за каждый тёмный импульс.
  const pulseStr = (+($('tone-pulse-str')?.value ?? 36)) / 100;
  // Белые импульсы — симметричное осветление: brightness /= (1 - str) но не выше 255.
  // Пиксели с pulseCount <= lightPass получают осветляющий импульс.
  const lightStr = pulseStr; // симметрично — та же сила в обратную сторону

  // Сначала вычисляем levels через обратную ф-ию (так же как canvasToToneLevels)
  const logBase = Math.log(1 - pulseStr);
  const tempLevels = new Float32Array(n);

  for (let i = 0; i < n; i++) {
    const r = d[i*4], g = d[i*4+1], b = d[i*4+2];
    const gray = lut[(0.299*r + 0.587*g + 0.114*b) | 0];
    const dark = inv ? gray / 255 : (255 - gray) / 255;
    let nl;
    if (dark <= 0)      nl = 0;
    else if (dark >= 1) nl = maxPulse;
    else                nl = Math.log(1 - dark) / logBase;
    tempLevels[i] = clamp(Math.round(nl), 0, maxPulse);
  }

  for (let i = 0; i < n; i++) {
    const pulseCount = tempLevels[i];

    // Тёмные проходы: brightness *= (1-str)^pulseCount
    let brightness = 255 * Math.pow(1 - pulseStr, pulseCount);

    // Белые проходы: пиксель получает осветляющий импульс если его pulseCount <= lightPass-порог
    for (let lp = 0; lp < lightPasses; lp++) {
      if (pulseCount <= lp) {
        brightness = Math.min(255, brightness / (1 - lightStr));
      }
    }

    d[i*4] = d[i*4+1] = d[i*4+2] = Math.round(Math.max(0, Math.min(255, brightness)));
    d[i*4+3] = 255;
  }
}

// ── Пан (1 палец/мышь), пинч-зум (2 пальца), колесо, double-tap ──
(function ieBindCanvas() {
  const cv = $('disp-canvas');
  const ptrs = new Map();   // активные указатели
  let pinch = null, lastTap = 0;

  const toCanvas = (clX, clY) => {
    const r = cv.getBoundingClientRect();
    return { x: (clX - r.left) * PANEL.lw / r.width, y: (clY - r.top) * PANEL.lh / r.height };
  };
  const pinchState = () => {
    const [a, b] = [...ptrs.values()];
    return { dist: Math.hypot(a.x - b.x, a.y - b.y), mx: (a.x + b.x) / 2, my: (a.y + b.y) / 2 };
  };
  // Зум к точке (в координатах канваса)
  function zoomAt(cx, cy, factor) {
    const nz = Math.max(5, Math.min(400, IE.zoom * factor));
    const rk = nz / IE.zoom;
    IE.x = (cx - 148) - ((cx - 148) - IE.x) * rk;
    IE.y = (cy - 64)  - ((cy - 64)  - IE.y) * rk;
    IE.zoom = nz;
    $('ie-zoom').value = Math.round(nz);
    $('iev-zoom').textContent = Math.round(nz) + '%';
  }

  cv.addEventListener('pointerdown', e => {
    if (!IE.img) return;
    cv.setPointerCapture(e.pointerId);
    ptrs.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (ptrs.size === 2) pinch = pinchState();
    else if (ptrs.size === 1) {
      const now = performance.now();              // double-tap → центрировать
      if (now - lastTap < 320) { IE.x = 0; IE.y = 0; renderImg(); }
      lastTap = now;
    }
    e.preventDefault();
  });

  cv.addEventListener('pointermove', e => {
    if (!IE.img || !ptrs.has(e.pointerId)) return;
    const prev = ptrs.get(e.pointerId);
    ptrs.set(e.pointerId, { x: e.clientX, y: e.clientY });
    const r = cv.getBoundingClientRect();
    if (ptrs.size === 2 && pinch) {
      const np = pinchState();
      if (pinch.dist > 8 && np.dist > 8) {
        const mid = toCanvas(np.mx, np.my);
        zoomAt(mid.x, mid.y, np.dist / pinch.dist);
      }
      IE.x += (np.mx - pinch.mx) * PANEL.lw / r.width;   // двухпальцевый пан
      IE.y += (np.my - pinch.my) * PANEL.lh / r.height;
      pinch = np;
      renderImg();
    } else if (ptrs.size === 1) {
      IE.x += (e.clientX - prev.x) * PANEL.lw / r.width;
      IE.y += (e.clientY - prev.y) * PANEL.lh / r.height;
      renderImg();
    }
  });

  ['pointerup', 'pointercancel'].forEach(ev => cv.addEventListener(ev, e => {
    ptrs.delete(e.pointerId);
    pinch = ptrs.size === 2 ? pinchState() : null;
  }));

  cv.addEventListener('wheel', e => {
    if (!IE.img) return;
    e.preventDefault();
    const p = toCanvas(e.clientX, e.clientY);
    zoomAt(p.x, p.y, e.deltaY < 0 ? 1.1 : 1 / 1.1);
    renderImg();
  }, { passive: false });

  cv.addEventListener('dblclick', () => { if (IE.img) { IE.x = 0; IE.y = 0; renderImg(); } });
  cv.addEventListener('contextmenu', e => { if (IE.img) e.preventDefault(); }); // long-press

  const ob = $('ie-orig');
  ob.addEventListener('pointerdown', e => { e.preventDefault(); IE.raw = true; renderImg(); });
  ob.addEventListener('contextmenu', e => e.preventDefault());
  ['pointerup', 'pointerleave', 'pointercancel'].forEach(ev =>
    ob.addEventListener(ev, () => { if (IE.raw) { IE.raw = false; renderImg(); } }));
})();

// Convert the logical canvas → the controller's physical planes (see PANELS /
// physIndex for the mapping). BW: 1-bpp, 0 = чёрный; RED: 1-bpp, 1 = красный.
function canvasToPhys(ctx) {
  const id = ctx.getImageData(0, 0, PANEL.lw, PANEL.lh);
  const d  = id.data, FB = fbSize();
  const bw  = new Uint8Array(FB).fill(0xFF); // all white
  const red = new Uint8Array(FB);            // no red
  for (let ly=0; ly<PANEL.lh; ly++) {
    for (let lx=0; lx<PANEL.lw; lx++) {
      const pi  = (ly*PANEL.lw + lx)*4;
      const r=d[pi], g=d[pi+1], b=d[pi+2];
      const [bi, msk] = physIndex(lx, ly);
      if (r > 120 && r-g > 60 && r-b > 60) { red[bi] |= msk; continue; } // красный слой
      if ((r+g+b)/3 < 128) bw[bi] &= ~msk;
    }
  }
  return { bw, red };
}

function makeAdjustLut() {
  const bri = +$('ie-bri').value, con = +$('ie-con').value;
  const gam = +$('ie-gam').value / 100;
  const cf = (259 * (con * 2.55 + 255)) / (255 * (259 - con * 2.55));
  const lut = new Uint8ClampedArray(256);

  for (let v = 0; v < 256; v++) {
    const g = 255 * Math.pow(v / 255, 1 / gam);
    lut[v] = cf * (g - 128) + 128 + bri * 1.27;
  }

  return lut;
}

function toneSourceImageData() {
  if (IE.img) {
    renderImg();
  }

  if (IE.img && IE.oc) {
    return { id: IE.oc.getContext('2d').getImageData(0, 0, PANEL.lw, PANEL.lh), raw: true };
  }

  return {
    id: $('disp-canvas').getContext('2d').getImageData(0, 0, PANEL.lw, PANEL.lh),
    raw: false,
  };
}

function canvasToToneLevels(maxPulse) {
  const src = toneSourceImageData();
  const d = src.id.data;
  const levels = new Uint8Array(PANEL.lw * PANEL.lh);
  const lut = src.raw ? makeAdjustLut() : null;
  const inv = src.raw && $('ie-inv').checked;

  // Обратная передаточная функция физической панели.
  //
  // Превью симулирует реальный отклик как:
  //   brightness = (1 - str)^N,  где str = pulseStr/100
  //
  // Значит физическая яркость при N импульсах = (1-str)^N * 255.
  // Нам нужно найти N при заданной целевой яркости target:
  //   target/255 = (1-str)^N
  //   N = log(target/255) / log(1-str)
  //
  // Это и есть обратная функция — преобразуем линейную тёмность пикселя
  // в количество импульсов, которое физически даст нужный результат.
  // Слайдер «Сила имп.» калибрует модель: при str=36 превью совпадает
  // с реальным экраном, значит обратная функция тоже верна.
  const pulseStr = (+($('tone-pulse-str')?.value ?? 36)) / 100;
  const logBase = Math.log(1 - pulseStr); // отрицательное число

  for (let i = 0; i < levels.length; i++) {
    const r = d[i*4], g = d[i*4+1], b = d[i*4+2];
    let gray = (0.299*r + 0.587*g + 0.114*b) | 0;
    if (lut) gray = lut[gray];

    const dark = inv ? gray / 255 : (255 - gray) / 255;

    let n;
    if (dark <= 0) {
      n = 0;                  // белый пиксель — 0 импульсов
    } else if (dark >= 1) {
      n = maxPulse;           // чёрный пиксель — максимум импульсов
    } else {
      // Нужная яркость: (1 - dark) * 255, т.е. target_brightness/255 = 1 - dark
      // N = log(1 - dark) / log(1 - str)
      n = Math.log(1 - dark) / logBase;
    }

    levels[i] = clamp(Math.round(n), 0, maxPulse);
  }

  return levels;
}

function toneLevelsToFrame(levels, pass) {
  const buf = new Uint8Array(fbSize()).fill(0xFF);

  for (let ly = 0; ly < PANEL.lh; ly++) {
    for (let lx = 0; lx < PANEL.lw; lx++) {
      if (levels[ly * PANEL.lw + lx] <= pass) continue;
      const [bi, msk] = physIndex(lx, ly);
      buf[bi] &= ~msk;
    }
  }

  return buf;
}

// Белый компенсирующий кадр для TONE_LIGHT LUT.
// Пиксели которые получили мало тёмных импульсов (toneLevel < threshold)
// отправляются белыми — BW RAM = 0xFF означает «белый» для TONE_LIGHT.
// pass=0: осветляем только пиксели у которых level=0 (чисто белые)
// pass=1: level<=1, pass=2: level<=2, итд — расширяем зону осветления.
function toneLevelsToFrameLight(levels, maxPulse, lightPass) {
  // Порог: осветляем пиксели чья тёмность ≤ lightThreshold.
  // При lightPass=0 — только уровень 0 (белые пиксели), при lightPass=k — всё до k.
  const threshold = lightPass;
  const buf = new Uint8Array(fbSize()).fill(0xFF); // 0xFF = белый

  for (let ly = 0; ly < PANEL.lh; ly++) {
    for (let lx = 0; lx < PANEL.lw; lx++) {
      const level = levels[ly * PANEL.lw + lx];
      // Пиксели ВЫШЕ порога (тёмные) — не осветляем, ставим 0x00 (чёрный в BW RAM)
      // чтобы TONE_LIGHT LUT их не трогал (LUT1 white: no-op для них)
      if (level > threshold) {
        const [bi, msk] = physIndex(lx, ly);
        buf[bi] &= ~msk; // 0 = чёрный пиксель → TONE_LIGHT оставляет его как есть
      }
      // Пиксели ≤ порога остаются 0xFF (белый) → TONE_LIGHT даёт им VSL импульс → осветление
    }
  }

  return buf;
}

async function sendCanvas() {
  if (!connected) { alert('Не подключено'); return; }
  if (sendBusy()) { uiToast('Подожди — идёт отправка'); return; }
  const ctx = $('disp-canvas').getContext('2d');
  const { bw, red } = canvasToPhys(ctx);
  sendLock = true;
  try { await sendBWR(bw, red, 'canvas'); }
  catch (e) { logE('Отправка: ' + e.message); sendProgFail(e.message); }
  finally { sendLock = false; }
}

// ── L2: guaranteed photo pass ─────────────────────────────────────────────
// Send one tone pass and confirm it actually landed. Stop-and-wait (drain after
// each frame) so vs.lastWc/lastRs describe THIS frame; on a corrupt/poisoned
// verdict resend the pass as a full keyframe (absolute RLE — self-healing, no
// dependence on device FB state) until the device confirms or retries run out.
// Slower than the pipelined video path but the image lands without stuck stripes.
// Without device CRC support it degrades to one best-effort send (nothing to
// verify). Returns {bytes, ok, tries}, or null if the stream must stop.
async function sendPhotoPassGuaranteed(frame, prev, label, retries=4) {
  if (!await vsAcquireCredit(15000)) {
    if (streamStop) return null;
    throw new Error('нет ACK/credit (' + label + ')');
  }
  let { pkt } = encodeVSFrame(frame, prev, 2, vs.useCrc, false, VS_KEY_SLACK);
  await sendBytesStream(pkt);
  await vsDrain(20000);
  let bytes = pkt.length, tries = 0;
  if (!vs.useCrc) return { bytes, ok: true, tries };   // can't verify → best-effort
  while ((vs.lastWc === 'bad' || vs.lastRs) && tries < retries && !streamStop) {
    tries++;
    logI(`  ↺ ${label}: CRC bad/poison — дослать keyframe #${tries}`);
    if (!await vsAcquireCredit(15000)) {
      if (streamStop) return null;
      throw new Error('нет ACK/credit (resync ' + label + ')');
    }
    pkt = encodeVSFrame(frame, null, 2, vs.useCrc, true, VS_KEY_SLACK).pkt; // full RLE
    await sendBytesStream(pkt);
    await vsDrain(20000);
    bytes = pkt.length;
  }
  const ok = !(vs.lastWc === 'bad' || vs.lastRs);
  if (!ok) logE(`  ✗ ${label}: не подтверждён после ${retries} попыток`);
  return { bytes, ok, tries };
}

// The photo tone passes are single-plane frames: TONE_DARK / TONE_LIGHT key the
// LUT on the B/W bit alone (the red=1 rows LUT2/LUT3 are no-ops), so they run
// on a single-buffer panel as well — its red RAM is zeroed by the CLEAR+UPDATE
// base (display_buffer writes 0x00 for a NULL red plane). Only tone-servo
// *video* needs the red plane as a hold bit; that check lives in startStream().
// The pulse-strength slider was calibrated on the 2.9"; another panel needs
// its own value.
async function sendCanvasTone() {
  if (!connected) { alert('Не подключено'); return; }
  if (streamRunning) { alert('Уже идёт VSTREAM'); return; }
  if (sendBusy()) { uiToast('Подожди — идёт отправка'); return; }
  const maxPulse    = parseInt($('tone-pulses').value) || 8;
  const lightPasses = parseInt($('tone-light-passes')?.value ?? 0) || 0;
  await sendToneLevels(canvasToToneLevels(maxPulse), maxPulse, lightPasses, 'canvas');
}

// Calibration ladder for the TONE_DARK slider. In the strong mode the slider is
// the panel's physical per-pulse strength: canvasToToneLevels inverts it to
// pick pulse counts and the preview re-applies the same number, so the preview
// barely moves with the slider — only the panel does, and "match the preview"
// cannot find the value. The ladder bypasses the model: maxPulse+1 bands, band
// k gets exactly k pulses, digits stamped at full black. The band that reads
// as 50 % grey gives str = 1 - 0.5^(1/N).
async function sendToneLadder() {
  if (!connected) { alert('Не подключено'); return; }
  if (streamRunning) { alert('Уже идёт VSTREAM'); return; }
  if (sendBusy()) { uiToast('Подожди — идёт отправка'); return; }
  const maxPulse = parseInt($('tone-pulses').value) || 8;
  const w = PANEL.lw, h = PANEL.lh, bands = maxPulse + 1;
  const levels = new Uint8Array(w * h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++)
    levels[y * w + x] = Math.min(maxPulse, Math.floor(x * bands / w));
  // Digits: black (all pulses) on the light half, white (no pulses) on the dark
  // half; the size follows the band width so two-digit labels do not collide.
  const lc = document.createElement('canvas'); lc.width = w; lc.height = h;
  const lx = lc.getContext('2d');
  const fs = Math.max(8, Math.min(Math.round(h / 10),
                                  Math.floor((w / bands) / (String(maxPulse).length * 0.7))));
  for (let k = 0; k < bands; k++)
    crispText(lx, String(k), Math.round((k + 0.5) * w / bands), h - 4, fs, { align:'center' });
  const d = lx.getImageData(0, 0, w, h).data;
  for (let i = 0; i < w * h; i++) if (d[i * 4 + 3] >= 128)
    levels[i] = levels[i] > maxPulse / 2 ? 0 : maxPulse;
  const table = [2, 3, 4, 5, 6, 8, 10, 12, 16].filter(n => n <= maxPulse)
    .map(n => `${n}→${Math.round(100 * (1 - Math.pow(0.5, 1 / n)))}`).join(' · ');
  logI(`Лесенка: полосы 0..${maxPulse} импульсов TONE_DARK, без модели. Найди полосу, которая выглядит как 50 % серый (ровно между белым и чёрным) — её номер N даёт «Сила имп.»: ${table}. Если последняя полоса не чёрная — бери 16 тонов.`);
  await sendToneLevels(levels, maxPulse, 0, 'лесенка');
}

async function sendToneLevels(levels, maxPulse, lightPasses, desc) {
  const usedMax = levels.reduce((m, v) => Math.max(m, v), 0);
  let prev = null, failed = null;

  streamRunning = true;
  streamStop = false;
  vsReset();
  sendProgShow(0, { passes: maxPulse + lightPasses, tag: 'Тона' });

  logI(`Отправляю ${desc} тонами: ${maxPulse} тёмных + ${lightPasses} белых импульсов, max=${usedMax}, сила ${$('tone-pulse-str').value} %`);

  try {
    await sendBytes(new TextEncoder().encode('HOST:1\n'));
    await sendBytes(new TextEncoder().encode('SS:0\n'));
    await sleep(100);

    const clearWait = waitForLine(line => line === 'cleared', 2500);
    await sendBytes(new TextEncoder().encode('CLEAR\n'));
    await clearWait;

    logI('  база: полный белый кадр...');
    const fullWait = waitForLine(
      line => line.startsWith('TELE:full') || /^done \d+ms/.test(line),
      20000
    );
    await sendBytes(new TextEncoder().encode('UPDATE\n'));
    if (!await fullWait) {
      logI('  не дождался TELE:full, продолжаю после таймаута');
    }

    // ── Тёмные проходы (TONE_DARK) ──────────────────────────────────────
    const lutWait = waitForLine(line => line.startsWith('LUTSET:TONE_DARK'), 2500);
    await sendBytes(new TextEncoder().encode('LUTSET:TONE_DARK\n'));
    await lutWait;

    const readyWait = vsWaitReady(12000);
    await sendBytes(new TextEncoder().encode('VSTREAM:start:TONE_DARK\n'));
    if (!await readyWait) {
      logI('  нет VSTREAM:ready за 12с, продолжаю осторожно');
    }

    for (let pass = 0; pass < maxPulse && !streamStop; pass++) {
      const frame = toneLevelsToFrame(levels, pass);
      const res = await sendPhotoPassGuaranteed(frame, prev, `dark ${pass + 1}/${maxPulse}`);
      if (!res) break;
      prev = frame;
      sendProgPass(pass + 1, res.bytes);

      const pct = Math.round((pass + 1) * 100 / (maxPulse + lightPasses));
      logI(`  dark pass ${pass + 1}/${maxPulse} · ${pct}% · pkt=${res.bytes}B` +
           (res.tries ? ` · resync×${res.tries}` : '') + (res.ok ? '' : ' · ✗'));
    }

    await vsDrain(20000);

    // ── Белые компенсирующие проходы (TONE_LIGHT) ────────────────────────
    if (lightPasses > 0 && !streamStop) {
      logI(`  переключаю на TONE_LIGHT для ${lightPasses} белых проходов...`);

      // Завершаем тёмный стрим, переключаем LUT, стартуем новый
      try { await sendBytes(VS_STOP_PHOTO, true); } catch(e) {}
      await sleep(300);

      const lutLightWait = waitForLine(line => line.startsWith('LUTSET:TONE_LIGHT'), 2500);
      await sendBytes(new TextEncoder().encode('LUTSET:TONE_LIGHT\n'));
      await lutLightWait;

      const readyLightWait = vsWaitReady(12000);
      await sendBytes(new TextEncoder().encode('VSTREAM:start:TONE_LIGHT\n'));
      if (!await readyLightWait) {
        logI('  нет VSTREAM:ready (light) за 12с, продолжаю');
      }

      prev = null;
      for (let lp = 0; lp < lightPasses && !streamStop; lp++) {
        // Белый проход: осветляем пиксели у которых мало тёмных импульсов.
        // С каждым проходом расширяем зону осветления (порог растёт).
        const frame = toneLevelsToFrameLight(levels, maxPulse, lp);
        const res = await sendPhotoPassGuaranteed(frame, prev, `light ${lp + 1}/${lightPasses}`);
        if (!res) break;
        prev = frame;
        sendProgPass(maxPulse + lp + 1, res.bytes);

        const pct = Math.round((maxPulse + lp + 1) * 100 / (maxPulse + lightPasses));
        logI(`  light pass ${lp + 1}/${lightPasses} · ${pct}% · pkt=${res.bytes}B` +
             (res.tries ? ` · resync×${res.tries}` : '') + (res.ok ? '' : ' · ✗'));
      }

      await vsDrain(20000);
      logI(`${desc} отправлен тонами (тёмные + белая компенсация)`);
    } else {
      logI(`${desc} отправлен тонами`);
    }
  } catch (e) {
    if (e.message !== 'stopped') { logE('Tone send: ' + e.message); failed = e.message; }
  } finally {
    const stopped = streamStop;
    try { await sendBytes(VS_STOP_PHOTO, true); } catch(e) {}
    await sleep(300);
    streamRunning = false;
    streamStop = false;
    if (failed)       sendProgFail(failed);
    else if (stopped) sendProgDone(`остановлено · ${sendProg.pass} из ${sendProg.passes} проходов`, 'stop');
    else              sendProgDone('готово');
  }
}

// ── Мягкий тоновый режим (TONE_SOFT_DARK) ─────────────────────────────────
// Использует виртуальную LUT с TA=TB=1 (вдвое слабее TONE_DARK).
// Квантование линейное — слабый импульс позволяет покрыть весь диапазон
// серого без пробелов даже при 16 тонах.

function ieProcessToneSoftPreview(id) {
  const d   = id.data;
  const n   = PANEL.lw * PANEL.lh;
  const lut = makeAdjustLut();
  const maxPulse = parseInt($('tone-soft-pulses')?.value) || 16;
  const inv      = $('ie-inv').checked;
  // SOFT_DARK ≈ вдвое слабее TONE_DARK → str ≈ 0.18 при дефолте слайдера 18
  const pulseStr = (+($('tone-soft-str')?.value ?? 18)) / 100;

  for (let i = 0; i < n; i++) {
    const r = d[i*4], g = d[i*4+1], b = d[i*4+2];
    const gray = lut[(0.299*r + 0.587*g + 0.114*b) | 0];
    // Линейное квантование: слабый импульс — линейный отклик достаточно точен
    const dark = inv ? gray / 255 : (255 - gray) / 255;
    const pulseCount = clamp(Math.round(dark * maxPulse), 0, maxPulse);
    // Симуляция накопления
    const brightness = 255 * Math.pow(1 - pulseStr, pulseCount);
    d[i*4] = d[i*4+1] = d[i*4+2] = Math.round(Math.max(0, Math.min(255, brightness)));
    d[i*4+3] = 255;
  }
}

function canvasToToneSoftLevels(maxPulse) {
  const src = toneSourceImageData();
  const d   = src.id.data;
  const levels = new Uint8Array(PANEL.lw * PANEL.lh);
  const lut = src.raw ? makeAdjustLut() : null;
  const inv = src.raw && $('ie-inv').checked;

  // Линейное квантование — мягкий импульс достаточно слаб чтобы
  // линейное соответствие dark→N давало корректный результат на панели.
  for (let i = 0; i < levels.length; i++) {
    const r = d[i*4], g = d[i*4+1], b = d[i*4+2];
    let gray = (0.299*r + 0.587*g + 0.114*b) | 0;
    if (lut) gray = lut[gray];
    const dark = inv ? gray / 255 : (255 - gray) / 255;
    levels[i] = clamp(Math.round(dark * maxPulse), 0, maxPulse);
  }
  return levels;
}

async function sendCanvasToneSoft() {
  if (!connected) { alert('Не подключено'); return; }
  if (streamRunning) { alert('Уже идёт VSTREAM'); return; }
  if (sendBusy()) { uiToast('Подожди — идёт отправка'); return; }

  const maxPulse = parseInt($('tone-soft-pulses')?.value) || 16;
  const levels   = canvasToToneSoftLevels(maxPulse);
  const usedMax  = levels.reduce((m, v) => Math.max(m, v), 0);
  let prev = null, failed = null;

  streamRunning = true;
  streamStop    = false;
  vsReset();
  sendProgShow(0, { passes: maxPulse, tag: 'Мягкий тон' });

  logI(`Отправляю canvas (мягкий тон): ${maxPulse} импульсов SOFT_DARK, max=${usedMax}`);
  if (!PANEL.redPlane) logI(`  ${PANEL.ctl}: отклик на импульс не калиброван — подбирай «Сила имп.» по результату`);

  try {
    await sendBytes(new TextEncoder().encode('HOST:1\n'));
    await sendBytes(new TextEncoder().encode('SS:0\n'));
    await sleep(100);

    const clearWait = waitForLine(line => line === 'cleared', 2500);
    await sendBytes(new TextEncoder().encode('CLEAR\n'));
    await clearWait;

    logI('  база: полный белый кадр...');
    const fullWait = waitForLine(
      line => line.startsWith('TELE:full') || /^done \d+ms/.test(line), 20000);
    await sendBytes(new TextEncoder().encode('UPDATE\n'));
    if (!await fullWait) logI('  не дождался TELE:full, продолжаю');

    // Режим 8 = TONE_SOFT_DARK (виртуальная таблица, TA=TB=1)
    const lutWait = waitForLine(line => line.startsWith('LUTSET:TONE_SOFT_DARK'), 2500);
    await sendBytes(new TextEncoder().encode('LUTSET:TONE_SOFT_DARK\n'));
    await lutWait;

    const readyWait = vsWaitReady(12000);
    await sendBytes(new TextEncoder().encode('VSTREAM:start:TONE_SOFT_DARK\n'));
    if (!await readyWait) logI('  нет VSTREAM:ready за 12с, продолжаю');

    for (let pass = 0; pass < maxPulse && !streamStop; pass++) {
      const frame = toneLevelsToFrame(levels, pass);
      const res = await sendPhotoPassGuaranteed(frame, prev, `soft ${pass + 1}/${maxPulse}`);
      if (!res) break;
      prev = frame;
      sendProgPass(pass + 1, res.bytes);

      const pct = Math.round((pass + 1) * 100 / maxPulse);
      logI(`  soft pass ${pass + 1}/${maxPulse} · ${pct}% · pkt=${res.bytes}B` +
           (res.tries ? ` · resync×${res.tries}` : '') + (res.ok ? '' : ' · ✗'));
    }

    await vsDrain(20000);
    logI('canvas отправлен (мягкий тон)');
  } catch (e) {
    if (e.message !== 'stopped') { logE('Soft tone send: ' + e.message); failed = e.message; }
  } finally {
    const stopped = streamStop;
    try { await sendBytes(VS_STOP_PHOTO, true); } catch(e) {}
    await sleep(300);
    streamRunning = false;
    streamStop    = false;
    if (failed)       sendProgFail(failed);
    else if (stopped) sendProgDone(`остановлено · ${sendProg.pass} из ${sendProg.passes} проходов`, 'stop');
    else              sendProgDone('готово');
  }
}

async function sendBWR(bw, red, desc) {
  logI(`Отправляю ${desc} (${PANEL.lw}×${PANEL.lh})…`);
  await sendBytes(new TextEncoder().encode('SS:0\n'));
  await sleep(350);
  const FB = fbSize();
  const hasRed = red.some(b=>b!==0);
  sendProgShow(FB * (hasRed ? 2 : 1));
  const sendPlane = async (buf, tag) => {
    sendProg.tag = tag;
    sendProg.planeBase = sendProg.sent;          // device acks count per plane
    for (let off=0; off<FB; off+=FW_CHUNK) {
      const ch = buf.slice(off, Math.min(off+FW_CHUNK, FB));
      await sendBytes(new TextEncoder().encode(`${tag}:${off}:${hexOf(ch).toUpperCase()}\n`));
      sendProg.sent += ch.length;
      if ((off / FW_CHUNK) % 4 === 0) sendProgRender();
      await sleep(5);
    }
    sendProgRender();
  };
  // A single-plane panel (400x300) has no red buffer in the firmware: the red
  // mask goes first, FAPPLY RED stages it into the controller's red RAM, then
  // the B/W frame follows and plain FAPPLY refreshes with both.
  if (hasRed && !PANEL.redPlane) {
    await sendPlane(red, 'RW');
    await sendBytes(new TextEncoder().encode('FAPPLY RED\n'));
    await sleep(400);
  }
  await sendPlane(bw, 'FW');
  if (hasRed && PANEL.redPlane) await sendPlane(red, 'RW');
  await sendBytes(new TextEncoder().encode('FAPPLY\n'));
  sendProgRefresh();
  logI(`${desc} отправлено → FAPPLY`);
}

// ═══════════════════════════════════════════════════════════════════
//  Test patterns (laid out from PANEL.lw × PANEL.lh at draw time)
// ═══════════════════════════════════════════════════════════════════

// Text for a 1-bpp panel. fillText is anti-aliased and canvasToPhys then
// thresholds it, so hairline strokes vanish — the generic "monospace" is
// Courier on iOS, and at the 10-12 px the 296×128 layout uses half of every
// glyph is gone. crispText renders the string at 3× on a scratch canvas and
// resolves each logical pixel by coverage (inked when ≥ cov of its 3×3
// subpixels are), then paints solid pixels in `color`. The anchor (x, y) is
// snapped to the logical grid so stems and bars land on whole pixels.
// opts: color, align ('left'|'center'), baseline ('alphabetic'|'middle'),
// bold, maxW (shrink the size until the string fits), cov.
const CRISP_FAMILY = 'Menlo, Consolas, "DejaVu Sans Mono", "Liberation Mono", "Roboto Mono", monospace';
const crispScratch = document.createElement('canvas');
function crispText(ctx, text, x, y, size, o = {}) {
  const S = 3, cov = o.cov ?? 3;
  const sc = crispScratch, sx = sc.getContext('2d');
  const fontFor = sz => `${o.bold ? 'bold ' : ''}${sz * S}px ${CRISP_FAMILY}`;
  const widthAt = sz => { sx.font = fontFor(sz); return sx.measureText(text).width / S; };
  if (o.maxW) while (size > 6 && widthAt(size) > o.maxW) size--;
  const tw = Math.ceil(widthAt(size)) + 2, th = Math.ceil(size * 1.6) + 2;   // logical px
  if (sc.width !== tw * S || sc.height !== th * S) { sc.width = tw * S; sc.height = th * S; }
  sx.clearRect(0, 0, sc.width, sc.height);
  sx.font = fontFor(size); sx.fillStyle = '#000';
  sx.textAlign = o.align || 'left'; sx.textBaseline = o.baseline || 'alphabetic';
  const ax = sx.textAlign === 'center' ? Math.floor(tw / 2) : 1;
  const ay = sx.textBaseline === 'middle' ? Math.floor(th / 2) : Math.ceil(size * 1.2);
  sx.fillText(text, ax * S, ay * S);
  const d = sx.getImageData(0, 0, sc.width, sc.height).data;
  const ox = Math.round(x) - ax, oy = Math.round(y) - ay;
  ctx.fillStyle = o.color || 'black';
  for (let ly = 0; ly < th; ly++) {
    let run = -1;
    for (let lx = 0; lx <= tw; lx++) {
      let n = 0;
      if (lx < tw) for (let dy = 0; dy < S; dy++) for (let dx = 0; dx < S; dx++)
        if (d[((ly * S + dy) * sc.width + lx * S + dx) * 4 + 3] >= 128) n++;
      const ink = n >= cov;
      if (ink && run < 0) run = lx;
      if (!ink && run >= 0) { ctx.fillRect(ox + run, oy + ly, lx - run, 1); run = -1; }
    }
  }
}

function testPat(n) {
  const c=$('disp-canvas'), ctx=c.getContext('2d');
  ieClose();
  dispEmpty(false);
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  const w = PANEL.lw, h = PANEL.lh;
  ctx.fillStyle='white'; ctx.fillRect(0,0,w,h);
  if(n===1) patBands(ctx, w, h);
  else if(n===2) patText(ctx, w, h);
  else patGrid(ctx, w, h);
  sendCanvas();
}

// The patterns are laid out from the panel size: a title bar ~1/9 of the
// height, fonts from the height, columns from the width. Nothing is stretched.
function patTitle(ctx, w, h, text) {
  const bh = Math.max(14, Math.round(h / 9));
  ctx.fillStyle='black'; ctx.fillRect(0,0,w,bh);
  crispText(ctx, text, 4, bh / 2, Math.max(10, Math.round(bh * 0.7)),
            { color:'white', baseline:'middle', maxW: w - 8 });
  return bh;
}

function patBands(ctx, w, h){
  const third = Math.floor(w / 3);
  ctx.fillStyle='black';   ctx.fillRect(0, 0, third, h);
  ctx.fillStyle='#c80000'; ctx.fillRect(w - third, 0, third, h);
  ctx.strokeStyle='black'; ctx.lineWidth=2; ctx.strokeRect(third + 1, 1, w - 2 * third - 2, h - 2);
  const fs = Math.max(13, Math.round(h / 9));
  const o = { align:'center', baseline:'middle', bold: fs >= 16 };   // bold smears below ~16 px
  crispText(ctx, 'BLACK', third / 2,     h / 2, fs, { ...o, color:'white' });
  crispText(ctx, 'WHITE', w / 2,         h / 2, fs, { ...o, color:'black' });
  crispText(ctx, 'RED',   w - third / 2, h / 2, fs, { ...o, color:'white' });
  patTitle(ctx, w, h, `BWR COLOR BANDS TEST — ${PANEL.ctl} ${w}×${h}`);
}

function patText(ctx, w, h){
  const bh = patTitle(ctx, w, h, `${PANEL.ctl} ${w}×${h} — TEXT & SHAPES TEST`);
  const fs = Math.max(11, Math.round(h / 11));          // body font
  const rowH = Math.round(fs * 1.5);
  const colW = Math.floor(w / 2) - 8;                   // two columns
  const boxW = Math.min(colW - 8, Math.round(w * 0.29));
  const boxH = Math.round(fs * 1.45);
  const x0 = 4, x1 = Math.floor(w / 2) + 4;
  const inBox = top => top + boxH - Math.round(fs * 0.35);   // baseline inside a box
  const boxLbl = color => ({ color, maxW: boxW - 8 });          // label shrinks to fit the box
  let y = bh + rowH;
  // Left column: black
  crispText(ctx, 'Hello World!', x0, y, fs);
  y += Math.round(fs * 0.6);
  ctx.fillStyle='black'; ctx.fillRect(x0, y, boxW, boxH);
  crispText(ctx, 'FILLED', x0 + 4, inBox(y), fs, boxLbl('white'));
  y += boxH + Math.round(fs * 0.5);
  ctx.strokeStyle='black'; ctx.lineWidth=2; ctx.strokeRect(x0, y, boxW, boxH);
  crispText(ctx, 'BORDER', x0 + 4, inBox(y), fs, boxLbl('black'));
  y += boxH + Math.round(fs * 0.8);
  // Checker of cells sized from the font, as many as fit the column
  const cw = Math.max(4, Math.round(fs * 0.7)), chh = Math.max(3, Math.round(fs * 0.55));
  const cols = Math.floor(colW / cw), rows = Math.max(2, Math.floor((h - y - 4) / chh));
  for(let r=0;r<rows;r++) for(let c=0;c<cols;c++)
    if((r+c)%2){ ctx.fillStyle='black'; ctx.fillRect(x0+c*cw, y+r*chh, cw, chh); }
  // Right column: red
  y = bh + rowH;
  crispText(ctx, 'Red text!', x1, y, fs, { color:'#c80000' });
  y += Math.round(fs * 0.6);
  ctx.fillStyle='#c80000'; ctx.fillRect(x1, y, boxW, boxH);
  crispText(ctx, 'RED FILL', x1 + 4, inBox(y), fs, boxLbl('white'));
  y += boxH + Math.round(fs * 0.5);
  ctx.strokeStyle='#c80000'; ctx.lineWidth=2; ctx.strokeRect(x1, y, boxW, boxH);
  crispText(ctx, 'RED BRD', x1 + 4, inBox(y), fs, boxLbl('#c80000'));
  y += boxH + Math.round(fs * 0.8);
  const r = Math.max(10, Math.min(Math.round((h - y) / 2) - 4, Math.round(w * 0.08)));
  ctx.beginPath(); ctx.arc(x1 + boxW / 2, y + r + 2, r, 0, Math.PI*2);
  ctx.fillStyle='#c80000'; ctx.fill();
}

function patGrid(ctx, w, h){
  const bh = patTitle(ctx, w, h, `RESOLUTION & DITHER TEST — ${PANEL.ctl} ${w}×${h}`);
  const lab = Math.max(9, Math.round(h / 14));
  const top = bh + 2;
  const bandH = Math.floor((h - top - lab - 4) * 0.5);   // upper test band
  const cw = Math.floor(w / 4);                           // four columns
  // H-lines
  for(let y=top;y<top+bandH;y+=2){ ctx.fillStyle='black'; ctx.fillRect(0,y,cw,1); }
  // V-lines
  for(let x=cw;x<2*cw;x+=2){ ctx.fillStyle='black'; ctx.fillRect(x,top,1,bandH); }
  // Checkerboard
  for(let y=top;y<top+bandH;y++) for(let x=2*cw;x<3*cw;x++)
    if((x+y)%2){ ctx.fillStyle='black'; ctx.fillRect(x,y,1,1); }
  // Diagonal single-pixel lines in the fourth column
  for(let x=3*cw;x<w;x++){ const y=top+((x-3*cw)%bandH); ctx.fillStyle='black'; ctx.fillRect(x,y,1,1); ctx.fillRect(x,top+bandH-1-((x-3*cw)%bandH),1,1); }
  const ly = top + bandH + lab;
  crispText(ctx, 'H-lines', 2, ly, lab);        crispText(ctx, 'V-lines', cw + 2, ly, lab);
  crispText(ctx, 'Checker', 2 * cw + 2, ly, lab); crispText(ctx, 'Diag', 3 * cw + 2, ly, lab);
  // Density gradient across the full width
  const gy = ly + 4, gh = h - gy - 2, steps = 8, sw = Math.floor(w / steps);
  for(let st=0;st<steps;st++){
    const dens=st/(steps-1), x0=st*sw;
    for(let y=gy;y<gy+gh;y++) for(let x=0;x<sw;x++)
      if(Math.random()<dens){ ctx.fillStyle='black'; ctx.fillRect(x0+x,y,1,1); }
  }
}

// ═══════════════════════════════════════════════════════════════════
//  Drag & drop
// ═══════════════════════════════════════════════════════════════════
function setupDrop(zone, handler) {
  zone.addEventListener('dragover', e=>{ e.preventDefault(); zone.classList.add('over'); });
  zone.addEventListener('dragleave', ()=>zone.classList.remove('over'));
  zone.addEventListener('drop', e=>{
    e.preventDefault(); zone.classList.remove('over');
    const f=e.dataTransfer.files[0]; if(f) handler(f);
  });
}
setupDrop($('img-drop'),  f=>loadImg(f));
setupDrop($('anim-drop'), f=>loadAnimFile(f));

// ═══════════════════════════════════════════════════════════════════
//  Stream Audio — воспроизведение звука видео синхронно с потоком
//  Используем Web Audio API (AudioContext + AudioBuffer) чтобы
//  не загружать второй <video> элемент и не мешать декодеру кадров.
// ═══════════════════════════════════════════════════════════════════
let streamAudioEnabled = false;
let _audioCtx = null;          // AudioContext (создаётся один раз)
let _audioBuffer = null;       // декодированный AudioBuffer текущего файла
let _audioSource = null;       // текущий AudioBufferSourceNode
let _audioStartTime = 0;       // ctx.currentTime в момент старта
let _audioStartOffset = 0;     // offset в аудио-буфере в момент старта
let _audioPlaying = false;
let _audioSyncMode = 'sync';
let _audioNominalFps = 30;
let _audioTotalFrames = 1;
let _audioLastSyncMs = 0;
let _audioFrameTimes = [];
let _audioFileUrl = null;      // URL файла для которого декодирован буфер

function toggleStreamAudio() {
  streamAudioEnabled = !streamAudioEnabled;
  const btn = $('btn-audio');
  if (streamAudioEnabled) {
    btn.textContent = '🔊 Звук';
    btn.className = 'btn acc';
    animLog('🔊 Звук включён — будет воспроизводиться при стриме');
  } else {
    btn.textContent = '🔇 Звук';
    btn.className = 'btn';
    stopStreamAudio();
    animLog('🔇 Звук выключен');
  }
}

// Декодировать аудио из видео-файла (вызывается при загрузке файла)
async function prepareStreamAudioBuffer() {
  if (!animVideoEl || !animVideoEl.src) { _audioBuffer = null; return; }
  if (animVideoEl.src === _audioFileUrl && _audioBuffer) return; // уже декодировано

  try {
    if (!_audioCtx) _audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    const resp = await fetch(animVideoEl.src);
    const arrBuf = await resp.arrayBuffer();
    _audioBuffer = await _audioCtx.decodeAudioData(arrBuf);
    _audioFileUrl = animVideoEl.src;
    animLog(`🔊 Аудио декодировано: ${_audioBuffer.duration.toFixed(1)}с, ${_audioBuffer.numberOfChannels}ch`);
  } catch (e) {
    _audioBuffer = null;
    _audioFileUrl = null;
    // Файл без аудио-трека — не ошибка, просто нечего играть
    animLog(`🔇 Аудио: нет аудио-дорожки или не удалось декодировать`);
  }
}

function _audioCreateSource(offset) {
  if (!_audioCtx || !_audioBuffer) return;
  if (_audioSource) { try { _audioSource.stop(); } catch {} _audioSource = null; }
  _audioSource = _audioCtx.createBufferSource();
  _audioSource.buffer = _audioBuffer;
  _audioSource.connect(_audioCtx.destination);
  _audioStartOffset = Math.max(0, Math.min(offset, _audioBuffer.duration));
  _audioSource.start(0, _audioStartOffset);
  _audioStartTime = _audioCtx.currentTime;
  _audioPlaying = true;
}

// Запуск аудио при старте стрима
async function startStreamAudio(syncMode, fps) {
  if (!streamAudioEnabled) return;
  if (!animVideoEl || !animVideoEl.src) return;

  _audioSyncMode = syncMode;
  _audioNominalFps = fps;
  _audioFrameTimes = [];
  _audioLastSyncMs = 0;

  // Декодируем если ещё не декодировали
  await prepareStreamAudioBuffer();
  if (!_audioBuffer) return;

  if (_audioCtx.state === 'suspended') await _audioCtx.resume();

  if (syncMode === 'sync') {
    // Sync: стартуем сразу с позиции 0
    _audioCreateSource(0);
    animLog('🔊 Аудио: sync режим, нормальная скорость');
  } else {
    // Guaranteed: стартуем, но скорость будем подстраивать
    _audioCreateSource(0);
    animLog('🔊 Аудио: guaranteed режим, скорость подстраивается под кадры');
  }
}

// Синхронизация аудио по прогрессу кадров
function syncStreamAudio(frameIdx, totalFrames, nominalFps, syncMode) {
  if (!streamAudioEnabled || !_audioBuffer || !_audioCtx) return;
  if (!_audioPlaying) return;

  const videoDuration = _audioBuffer.duration;
  const targetTime = (frameIdx / totalFrames) * videoDuration;

  if (syncMode === 'sync') {
    // Sync: аудио играет само, только корректируем при сильном дрейфе
    const currentPos = _audioStartOffset + (_audioCtx.currentTime - _audioStartTime);
    const drift = Math.abs(currentPos - targetTime);
    if (drift > 0.3) {
      // Пересоздать source с новой позиции
      _audioCreateSource(targetTime);
    }
  } else {
    // Guaranteed: подстраиваем playbackRate через пересоздание source
    const now = performance.now();
    _audioFrameTimes.push(now);
    if (_audioFrameTimes.length > 12) _audioFrameTimes.shift();

    if (_audioFrameTimes.length >= 3) {
      const dt = (_audioFrameTimes[_audioFrameTimes.length - 1] - _audioFrameTimes[0]) / (_audioFrameTimes.length - 1);
      const realFps = 1000 / dt;
      const rate = clamp(realFps / nominalFps, 0.25, 4.0);

      // Обновляем playbackRate на source (меняется без пересоздания)
      if (_audioSource && now - _audioLastSyncMs > 300) {
        _audioLastSyncMs = now;
        const currentRate = _audioSource.playbackRate.value;
        const smoothed = currentRate * 0.65 + rate * 0.35;
        _audioSource.playbackRate.value = clamp(smoothed, 0.25, 4.0);
      }

      // Корректировать позицию при дрейфе > 500ms
      const currentPos = _audioStartOffset +
        (_audioCtx.currentTime - _audioStartTime) * (_audioSource ? _audioSource.playbackRate.value : 1);
      const drift = Math.abs(currentPos - targetTime);
      if (drift > 0.5) {
        _audioCreateSource(targetTime);
        if (_audioSource) _audioSource.playbackRate.value = clamp(realFps / nominalFps, 0.25, 4.0);
      }
    }
  }
}

function stopStreamAudio() {
  _audioFrameTimes = [];
  _audioLastSyncMs = 0;
  _audioPlaying = false;
  if (_audioSource) {
    try { _audioSource.stop(); } catch {}
    _audioSource = null;
  }
}

// ═══════════════════════════════════════════════════════════════════
//  VStream — animation streaming
// ═══════════════════════════════════════════════════════════════════
function cancelAnimProducer() {
  if (!animFrameProducer) return;
  const p = animFrameProducer;
  animFrameProducer = null;
  p.cancel();
}

async function loadAnimFile(file) {
  if (!file) return;
  const info = $('anim-info');
  info.textContent = 'Загружаю: ' + file.name + '…';
  cancelAnimProducer();
  animPreEncoded=null;
  animDecoder=null; animVideoEl=null; animImgEl=null;
  animTotalFrames=0; animFileType=null;

  const isGif  = file.name.toLowerCase().endsWith('.gif') || file.type==='image/gif';
  const isVideo = file.type.startsWith('video/') || /\.(mp4|webm|avi|mov|mkv)$/i.test(file.name);

  if (isGif && typeof ImageDecoder !== 'undefined') {
    try {
      const buf = await file.arrayBuffer();
      const dec = new ImageDecoder({ data: buf, type: 'image/gif' });
      await dec.tracks.ready;
      animTotalFrames = dec.tracks.selectedTrack.frameCount;
      animDecoder = dec;
      animFileType = 'gif';
      info.textContent = `GIF: ${animTotalFrames} кадров | ${(file.size/1024).toFixed(0)} KB`;
      return;
    } catch(e) { logE('GIF decoder: '+e.message); }
  }

  if (isVideo || isGif) {
    if (!animVideoEl) {
      animVideoEl = document.createElement('video');
      animVideoEl.muted = true;
      animVideoEl.playsInline = true;
      animVideoEl.preload = 'auto';
      animVideoEl.style.display='none';
      document.body.appendChild(animVideoEl);
    }
    animVideoEl.src = URL.createObjectURL(file);
    animFileType = 'video';
    animPreEncoded = null; // invalidate cache on new file
    await new Promise(res => { animVideoEl.onloadedmetadata=res; });
    // Detect real FPS: use requestVideoFrameCallback to measure frame interval
    let detectedFps = 0;
    if ('requestVideoFrameCallback' in animVideoEl) {
      try {
        detectedFps = await new Promise((resolve) => {
          const times = [];
          let cbId;
          const onFrame = (now, meta) => {
            if (Number.isFinite(meta.mediaTime)) times.push(meta.mediaTime);
            if (times.length >= 6) {
              animVideoEl.pause();
              const dt = (times[times.length-1] - times[0]) / (times.length - 1);
              resolve(dt > 0 ? Math.round(1 / dt) : 0);
            } else {
              cbId = animVideoEl.requestVideoFrameCallback(onFrame);
            }
          };
          animVideoEl.currentTime = 0;
          animVideoEl.playbackRate = 1;
          cbId = animVideoEl.requestVideoFrameCallback(onFrame);
          animVideoEl.play().catch(() => resolve(0));
          // Timeout fallback
          setTimeout(() => { try { animVideoEl.pause(); } catch{} resolve(0); }, 3000);
        });
        animVideoEl.pause();
        animVideoEl.currentTime = 0;
      } catch { detectedFps = 0; }
    }
    if (!detectedFps) detectedFps = 30; // safe fallback for most video
    const userFps = parseFloat($('enc-fps').value) || detectedFps;
    animTotalFrames = Math.max(1, Math.round(animVideoEl.duration * userFps));
    info.textContent = `Видео: ${file.name} | ~${animTotalFrames} кадров @ ${userFps}fps${userFps===detectedFps?' (auto)':''}`;
    // Store detected fps so startStream can use it
    animVideoEl._detectedFps = detectedFps;
  } else {
    const url = URL.createObjectURL(file);
    animImgEl = new Image();
    await new Promise(res => { animImgEl.onload=res; animImgEl.src=url; });
    animTotalFrames=1; animFileType='img';
    info.textContent = `Изображение: ${file.name}`;
  }
}

// ═══════════════════════════════════════════════════════════════════
//  Screen capture
// ═══════════════════════════════════════════════════════════════════
async function startScreenShare() {
  cancelAnimProducer();
  animPreEncoded=null;
  if (!navigator.mediaDevices || !navigator.mediaDevices.getDisplayMedia) {
    alert('getDisplayMedia не поддерживается.\nОткрой в Chrome и убедись что сайт открыт через https:// или localhost.');
    return;
  }
  try {
    animScreenStream = await navigator.mediaDevices.getDisplayMedia({
      video: { frameRate: 30 },
      audio: streamAudioEnabled,
    });
    if (!animVideoEl) {
      animVideoEl = document.createElement('video');
      animVideoEl.style.display = 'none';
      document.body.appendChild(animVideoEl);
    }
    // Если захвачен аудио-трек и звук включён — воспроизводим
    const hasAudio = animScreenStream.getAudioTracks().length > 0;
    animVideoEl.muted = !hasAudio || !streamAudioEnabled;
    animVideoEl.srcObject = animScreenStream;
    await animVideoEl.play();
    animFileType = 'screen';
    animTotalFrames = 0;
    const track = animScreenStream.getVideoTracks()[0];
    const s = track.getSettings();
    const audioNote = hasAudio && streamAudioEnabled ? ' + 🔊 audio' : '';
    $('anim-info').textContent = `Screen: ${s.width||'?'}×${s.height||'?'} @ ${s.frameRate||'?'}fps${audioNote}`;
    $('screen-st').textContent = '🔴 Захват активен';
    $('btn-screen').textContent = '⏹ Стоп Screen';
    $('btn-screen').className = 'btn bad big';
    track.addEventListener('ended', stopScreenShare);
    logI(`Screen capture started: ${s.width}×${s.height}${audioNote}`);
  } catch(e) {
    logE('Screen capture: ' + e.message);
  }
}

function stopScreenShare() {
  if (animScreenStream) {
    animScreenStream.getTracks().forEach(t => t.stop());
    animScreenStream = null;
  }
  if (animVideoEl) animVideoEl.srcObject = null;
  if (animFileType === 'screen') { animFileType = null; animTotalFrames = 0; }
  $('anim-info').textContent = '';
  $('screen-st').textContent = 'Не активен';
  $('btn-screen').textContent = '📺 Screen Share';
  $('btn-screen').className = 'btn pur big';
}

function toggleScreenShare() {
  if (animFileType === 'screen') stopScreenShare();
  else startScreenShare();
}

function toggleStream() { if(streamRunning) stopStream(); else startStream(); }

function stopStream() {
  streamStop=true;
  if(streamSendCancel) { streamSendCancel(); streamSendCancel=null; }
  cancelAnimProducer();
  stopStreamAudio();
  vsAbort(); // wake credit/drain waiters so the loop exits promptly
}

async function startStream() {
  if (!connected){ alert('Не подключено'); return; }
  if (!animFileType){ alert('Загрузи файл анимации'); return; }
  // Servo tones send two planes and use the red one as a per-pixel hold bit;
  // a single-buffer panel's firmware swallows that half, so the servo would
  // run blind. Temporal tones and mono are single-plane and fine.
  if (currentAnimRenderMode() === 'tone-servo' && !PANEL.redPlane) {
    alert('«Тона servo» используют красную плоскость как hold-бит — на этой панели (один B/W буфер) недоступны. Выбери «Тона temporal» или моно.');
    return;
  }
  streamRunning=true; streamStop=false; vsReset();
  $('btn-stream').textContent='⏸ Pause';
  liveStart();                   // header chip + nav marker: visible from any tab

  const encType  = parseInt($('enc-type').value);
  const detectedFps = (animVideoEl && animVideoEl._detectedFps) || 30;
  const fps      = parseFloat($('enc-fps').value) || detectedFps;
  const renderMode = currentAnimRenderMode();
  const toneVideo = renderMode !== 'mono';
  const toneServo = renderMode === 'tone-servo';
  const toneCell = toneVideo ? (parseInt($('enc-tone-cell')?.value) || 2) : 1;
  const dith     = !toneVideo && $('enc-dith').checked;
  const inv      = $('enc-inv').checked;
  const land     = $('enc-land').checked;
  const loop     = $('enc-loop').checked;
  const rot      = parseInt($('enc-rot').value)||0;
  const scale    = $('enc-scale').value;
  const syncMode = $('enc-sync').value;
  const lut      = toneVideo ? $('enc-tone-lut').value : $('enc-lut').value;
  const [tw, th] = vsDims(land);
  const halfRes  = !toneVideo && $('enc-half').checked;
  vs.half = halfRes;   // encodeVSFrame/vsWrap read it; vsReset() above cleared it
  const toneState = toneVideo ? createToneVideoState(tw, th, toneCell, {
    kind: toneServo ? 'servo' : 'temporal',
    step: parseFloat($('enc-tone-step')?.value) || 0.16,
    smooth: parseFloat($('enc-tone-smooth')?.value) || 0.35,
  }) : null;

  const oc  = new OffscreenCanvas(tw, th);
  const oct = oc.getContext('2d', {willReadFrequently:true});

  // Start a bounded background encoder for video: wait only for a small startup
  // buffer, then stream while decoding continues ahead of the BLE/display loop.
  // In pre-encode mode: wait for ALL frames before connecting to device — ensures
  // zero decode jitter during playback (identical to vstream.py EBF behavior).
  const preEncode = $('enc-preenc').checked;
  if (animFileType === 'video') {
    animPreEncoded = null;
    let waitingForBuffer = true;
    const encodeFrame = toneState
      ? frameCtx => encodeToneVideoFrame(frameCtx, tw, th, land, toneState, inv)
      : null;
    const previewFrame = toneServo
      ? () => updateToneEstimatePreview(toneState)
      : null;
    animFrameProducer = startVideoFrameProducer(tw, th, dith, inv, land, rot, scale, fps, s => {
      if (!waitingForBuffer || streamStop) return;
      const need = preEncode ? s.total : Math.max(1, s.startBuffer);
      $('stream-prog').style.width = (Math.min(s.ready, need)/need*100).toFixed(0)+'%';
      $('stream-st').textContent = preEncode
        ? `Pre-encode ${s.ready}/${s.total} (${(s.ready/s.total*100).toFixed(0)}%)`
        : `Буфер ${Math.min(s.ready, need)}/${need}`;
    }, encodeFrame, !toneVideo, previewFrame);
    if (animFrameProducer) {
      const s0 = animFrameProducer.stats();
      if (preEncode) {
        animLog(`⏳ Pre-encode: кодирую все ${s0.total} кадров заранее…`);
        $('stream-st').textContent = 'Pre-encode…';
        const ok = await animFrameProducer.waitForAll(300000);
        waitingForBuffer = false;
        if (streamStop) {
          cancelAnimProducer();
          streamRunning=false; streamStop=false;
          $('btn-stream').textContent='▶ Stream';
          $('stream-st').textContent='Остановлено';
          $('stream-prog').style.width='0%';
          return;
        }
        const s = animFrameProducer.stats();
        if (ok && s.ready === s.total) {
          animTotalFrames = s.total;
          animLog(`✓ Pre-encode готов: ${s.total} кадров в памяти`);
        } else {
          animLog(`⚠ Pre-encode: готово ${s.ready}/${s.total} — стримлю что есть`);
          animTotalFrames = s.total;
        }
      } else {
        animLog(`⏳ Буферизую ${s0.startBuffer} кадров, дальше кодирование пойдёт в фоне`);
        $('stream-st').textContent = 'Буферизую…';
        const ok = await animFrameProducer.waitForStart(10000);
        waitingForBuffer = false;
        if (streamStop) {
          cancelAnimProducer();
          streamRunning=false; streamStop=false;
          $('btn-stream').textContent='▶ Stream';
          $('stream-st').textContent='Остановлено';
          $('stream-prog').style.width='0%';
          return;
        }
        const s = animFrameProducer.stats();
        if (ok) {
          animTotalFrames = s.total;
          animLog(`▶ Старт с буфером ${s.ready}/${s.total}; лимит ahead=${s.maxAhead} кадров`);
        } else {
          animLog('⚠ Фоновое кодирование не стартовало за 10с — seek-режим');
          cancelAnimProducer();
        }
      }
    } else {
      animLog('⚠ requestVideoFrameCallback не поддерживается — seek-режим');
    }
    $('stream-prog').style.width = '0%';
  }

  if (DRIVE_LUTS[lut]) {
    // Host drive table: upload it, then start with CUSTOM — a plain start
    // resets the device to its builtin TURBO and would ignore the upload.
    await sendBytes(new TextEncoder().encode('LUTW:' + hexOf(DRIVE_LUTS[lut]).toUpperCase() + '\n'));
    await sleep(300);
    await sendBytes(new TextEncoder().encode('VSTREAM:start:CUSTOM\n'));
  } else {
    await sendBytes(new TextEncoder().encode(`LUTSET:${lut}\n`));
    await sleep(150);
    await sendBytes(new TextEncoder().encode(`VSTREAM:start:${lut}\n`));
  }
  // Device primes the display before replying ready (deep-sleep wake + HV
  // charge, ~1s) — don't blast frames into an unprimed controller.
  if (!await vsWaitReady(10000))
    animLog('⚠ нет VSTREAM:ready за 10с — старая прошивка? продолжаю');
  const renderInfo = toneVideo
    ? `tone ${toneState.kind} cell=${toneState.cell} lut=${lut}${toneServo ? ` step=${toneState.step.toFixed(2)} smooth=${toneState.smooth.toFixed(2)}` : ''}`
    : `1bpp ${dith ? 'dither' : 'threshold'} lut=${lut}${halfRes ? ' half-res' : ''}`;
  animLog(`▶ VSTREAM:start:${DRIVE_LUTS[lut] ? 'CUSTOM(' + lut + ')' : lut} | ${renderInfo} | mode=${syncMode} rot=${rot}° ${land?'landscape':'portrait'}`);

  // Запускаем аудио-сопровождение если включено
  await startStreamAudio(syncMode, fps);

  let prev=null, loop_n=0, sinceKey=0;
  const t0 = performance.now();
  let framesDone=0;  // frames sent (vs.acked = frames confirmed by device)
  let resyncs=0;
  let sendErrs=0;    // consecutive transient BLE send errors → resync, don't abort
  let lastUiUpdate=0; // throttle DOM updates to ~8fps to avoid layout thrashing

  const statusTxt = (head, pktLen, extra='') => {
    const elapsed=(performance.now()-t0)/1000;
    const cyc=vsCycleMs(), disp=vsDispMs(), net=Math.max(0,cyc-disp);
    // fps = frames the panel actually showed, over the last ~20 ACKs (the
    // cycle), not a since-start average of every ACK: an average hides stalls
    // and counts frames the device refused (CRC bad / waiting for a keyframe).
    const shown = vs.shown || 0, dropped = vs.acked - shown;
    const okShare = vs.acked ? shown / vs.acked : 1;
    const fps = cyc > 0 ? (1000 / cyc) * okShare : 0;
    const avg = elapsed > 0 ? shown / elapsed : 0;
    const buf = animFrameProducer ? (() => {
      const s = animFrameProducer.stats();
      return ` | enc=${s.ready}/${s.total} buf=${s.ahead}`;
    })() : '';
    return `${head} ${fps.toFixed(1)}fps (avg ${avg.toFixed(1)}${dropped ? `, drop ${dropped}` : ''}) | `+
           `cyc~${cyc.toFixed(0)} disp~${disp.toFixed(0)} net~${net.toFixed(0)}ms | pkt=${pktLen}B${buf}${extra}`;
  };

  // Pipelined send: waits only when the in-flight window (vs.window) is full,
  // i.e. the display is the bottleneck — BLE transfer overlaps the LUT wave.
  // 15s credit timeout rides out the worst device pauses (DC-balance full
  // refresh every 500 frames ≈ 1.5-2.5s, deep-sleep recovery up to 8s).
  // Returns packet length, or null when the stream must stop.
  async function sendFrame(fr) {
    if (!await vsAcquireCredit(15000)) {
      if (streamStop) return null;
      if (++resyncs > 3) { animLog('⚠ слишком много resync — стоп'); return null; }
      // No ACK in 15s: drop stale in-flight accounting, send full RLE keyframe
      animLog('↺ нет ACK 15с — resync RLE-кадром');
      vs.inflight = 0;
      await vsAcquireCredit(1); // grants instantly at inflight=0
      try { await sendBytesStream(encodeVSFrame(fr, null, 1/*RLE*/, vs.useCrc, true, VS_KEY_SLACK).pkt); }
      catch(e) { animLog('⚠ resync: '+e.message); return null; }
      prev = fr; sinceKey = 0; framesDone++;
      return 0;
    }
    // Force a keyframe when the device asked to resync (rs=1 / wire CRC bad) or
    // the periodic safety net is due; encodeVSFrame also picks a keyframe when a
    // full frame is as cheap as the delta (scene cut / static area).
    const wantResync = vs.resync; vs.resync = false;
    const force = wantResync || (VS_KEY_INTERVAL>0 && sinceKey>=VS_KEY_INTERVAL);
    const { pkt, isKey } = encodeVSFrame(fr, prev, encType, vs.useCrc, force, VS_KEY_SLACK);
    try { await sendBytesStream(pkt); }
    catch(e) {
      if (e.message === 'stopped' || streamStop) return null;   // genuine Stop
      // Transient BLE error (e.g. "GATT operation already in progress"): a single
      // hiccup must NOT kill a multi-thousand-frame stream. Drop the stale
      // in-flight accounting and force the NEXT frame to be a full keyframe —
      // it overwrites the whole FB and heals any partial write. Give Chrome's
      // GATT queue a moment to drain, then continue. Abort only if errors pile
      // up consecutively (device really gone).
      if (++sendErrs > 5) { animLog(`⚠ send error: ${e.message} — слишком много подряд, стоп`); return null; }
      animLog(`↺ send error: ${e.message} — resync #${sendErrs}`);
      vs.resync = true;
      vs.inflight = 0;
      await sleep(150);
      return 0;   // non-null → loop continues; this frame is skipped, next is a keyframe
    }
    sendErrs = 0;   // a clean send clears the consecutive-error streak
    prev = fr; sinceKey = isKey ? 0 : sinceKey+1; framesDone++;
    return pkt.length;
  }

  try {
    if (animFileType === 'screen') {
      // Real-time screen capture — no seeking, no frame count
      let lastSentMs = performance.now();
      const KEEPALIVE_MS = 5000; // re-send prev frame if stalled to beat device watchdog (30s)
      while (!streamStop) {
        const fr = await getAnimFrame(0, 1, fps, oct, tw, th, dith, inv, land, rot, scale, toneState);
        if (!fr) {
          if (prev && performance.now() - lastSentMs > KEEPALIVE_MS) {
            if (await sendFrame(prev) === null) break;
            lastSentMs = performance.now();
            animLog('↺ keepalive (screen stalled)');
          } else {
            await sleep(16);
          }
          continue;
        }
        const pktLen = await sendFrame(fr);
        if (pktLen === null) break;
        lastSentMs = performance.now();
        $('stream-st').textContent = statusTxt(`[${framesDone}]`, pktLen);
        if (framesDone%30===0)
          animLog(`Screen ${statusTxt(`frame ${framesDone} |`, pktLen)}`);
      }
    } else {
      outer: while (!streamStop) {
        loop_n++;
        if (animFileType==='video' && animVideoEl)
          if (!animPreEncoded && !animFrameProducer)
            animTotalFrames = Math.max(1, Math.round(animVideoEl.duration * fps));

        let i=0;
        while (i < animTotalFrames && !streamStop) {
          if (syncMode==='sync') {
            // Mirror vstream.py play_sync: check each frame's deadline sequentially.
            // Skip frames that are already late (don't send them), but always send
            // the latest non-late frame. This ensures even spacing and minimal
            // visual jumps — exactly how Python iterates through all frames.
            const now = performance.now();
            const deadline = t0 + (i/fps)*1000;

            if (now > deadline + 10) {
              // This frame is late — skip without sending (don't update prev)
              i++;
              continue;
            }

            // This frame is on time or early — get it and send
            const fr = await getAnimFrame(i, animTotalFrames, fps, oct, tw, th, dith, inv, land, rot, scale, toneState);
            if (!fr) { i++; continue; }

            // Sleep until deadline if we're early (frame ready, wait to send)
            const ahead = deadline - performance.now();
            if (ahead > 2) await sleep(ahead);

            const pktLen = await sendFrame(fr);
            if (pktLen === null) break outer;
            i++;
            syncStreamAudio(i, animTotalFrames, fps, syncMode);

            const pct = (i/animTotalFrames*100).toFixed(0);
            const _now2 = performance.now();
            if (_now2 - lastUiUpdate > 125) {
              lastUiUpdate = _now2;
              $('stream-prog').style.width = pct+'%';
              $('stream-st').textContent =
                statusTxt(`[${i}/${animTotalFrames}]`, pktLen);
            }
            if (framesDone%20===0)
              animLog(statusTxt(`Frame ${i}/${animTotalFrames} |`, pktLen));
          } else {
            // Guaranteed mode: send every frame, paced only by device ACKs
            const fr = await getAnimFrame(i, animTotalFrames, fps, oct, tw, th, dith, inv, land, rot, scale, toneState);
            if (!fr) { i++; continue; }
            const pktLen = await sendFrame(fr);
            if (pktLen === null) break outer;
            i++;
            syncStreamAudio(i, animTotalFrames, fps, syncMode);

            const pct = (i/animTotalFrames*100).toFixed(0);
            const _now2 = performance.now();
            if (_now2 - lastUiUpdate > 125) {
              lastUiUpdate = _now2;
              $('stream-prog').style.width = pct+'%';
              $('stream-st').textContent =
                statusTxt(`[${i}/${animTotalFrames}]`, pktLen);
            }
            if (framesDone%20===0)
              animLog(statusTxt(`Frame ${i}/${animTotalFrames} |`, pktLen));
          }
        }
        if (!loop || streamStop) break;
        animLog(`Loop ${loop_n} завершён`);
      }
    }
    // Let the device confirm everything still in flight before sending stop.
    if (!streamStop) await vsDrain(15000);
  } catch(e) { animLog('Ошибка: '+e.message); logE(e.message); }
  finally {
    cancelAnimProducer();
    stopStreamAudio();
    // Video/animation finish: stop binary stream and let firmware restore saver.
    try { await sendBytes(VS_STOP_VIDEO, true); } catch(e) {}
    await sleep(300);
    streamRunning=false; streamStop=false;
    liveStop();
    $('btn-stream').textContent='▶ Stream';
    $('stream-st').textContent='Завершено';
    $('stream-prog').style.width='0%';
    animLog('■ VSTREAM stopped');
    if (animFileType === 'screen') stopScreenShare();
  }
}

async function getAnimFrame(i, total, fps, ctx, tw, th, dith, inv, land, rot=0, scale='fit', toneState=null) {
  // Streaming video path: frames are produced sequentially in the background.
  if (animFrameProducer) {
    const buf = await animFrameProducer.get(i);
    if (buf) {
      if (toneState?.kind === 'servo') updateToneEstimatePreview(toneState);
      else updateAnimPreview(buf);
      return buf;
    }
    if (streamStop) return null;
    const s = animFrameProducer.stats();
    if (s.failed || s.canceled) cancelAnimProducer();
    else return null;
  }

  // Fast path: use pre-encoded cache if available (video only)
  if (!toneState && animPreEncoded && animPreEncoded[i]) {
    const buf = animPreEncoded[i];
    updateAnimPreview(buf);
    return buf;
  }

  ctx.fillStyle='white'; ctx.fillRect(0,0,tw,th);

  if (animFileType==='gif' && animDecoder) {
    const res = await animDecoder.decode({frameIndex:i});
    const bmp = res.image;
    drawScaledRotated(ctx, bmp, bmp.width, bmp.height, tw, th, rot, scale);
  } else if (animFileType==='video' && animVideoEl) {
    const t = (i/(Math.max(total-1,1))) * animVideoEl.duration;
    animVideoEl.currentTime = t;
    await new Promise(res => {
      let done=false;
      const timer = setTimeout(()=>{ if(!done){done=true;res();} }, 3000);
      animVideoEl.addEventListener('seeked', ()=>{ if(!done){done=true;clearTimeout(timer);res();} }, {once:true});
    });
    drawScaledRotated(ctx, animVideoEl, animVideoEl.videoWidth, animVideoEl.videoHeight, tw, th, rot, scale);
  } else if (animFileType==='img' && animImgEl) {
    drawScaledRotated(ctx, animImgEl, animImgEl.width, animImgEl.height, tw, th, rot, scale);
  } else if (animFileType==='screen' && animVideoEl) {
    if (animVideoEl.readyState < 2 || !animVideoEl.videoWidth) return null;
    drawScaledRotated(ctx, animVideoEl, animVideoEl.videoWidth, animVideoEl.videoHeight, tw, th, rot, scale);
  } else return null;

  const buf = toneState
    ? encodeToneVideoFrame(ctx, tw, th, land, toneState, inv)
    : canvasTo1bpp(ctx, tw, th, dith, inv, land);
  if (toneState?.kind === 'servo') updateToneEstimatePreview(toneState);
  else updateAnimPreview(buf);
  return buf;
}

function startVideoFrameProducer(tw, th, dith, inv, land, rot, scale, fps, onProgress, encodeFrame=null, cacheFrames=true, previewFrame=null) {
  if (animFileType !== 'video' || !animVideoEl) return null;
  if (!('requestVideoFrameCallback' in animVideoEl)) return null;

  const video = animVideoEl;
  const total = Math.max(1, Math.round(video.duration * fps));
  const interval = video.duration / total;
  const startBuffer = Math.min(total, clamp(Math.ceil(fps * 1.5), 6, 30));
  const maxAhead = Math.min(total, Math.max(startBuffer + 4, clamp(Math.ceil(fps * 5), 24, 90)));
  const resumeAhead = Math.max(startBuffer, Math.floor(maxAhead * 0.55));
  const frames = new Array(total).fill(null);
  const oc  = new OffscreenCanvas(tw, th);
  const oct = oc.getContext('2d', {willReadFrequently:true});

  let ready = 0;          // contiguous frames available from index 0
  let produced = 0;       // unique target slots filled
  let consumed = -1;      // last frame index requested by the sender
  let lastTarget = -1;
  let lastFrame = null;
  let done = false, canceled = false, failed = false, pausedForBuffer = false;
  let noThrottle = false; // set by waitForAll to disable pause-on-ahead
  let lastProgressAt = 0;
  const indexWaiters = new Map();
  const startWaiters = [];

  const ahead = () => Math.max(0, ready - consumed - 1);
  const stats = () => ({
    total, ready, produced, startBuffer, maxAhead, resumeAhead,
    ahead: ahead(), done, canceled, failed, paused: pausedForBuffer,
  });
  const emitProgress = (force=false) => {
    const now = performance.now();
    if (!force && now - lastProgressAt < 120) return;
    lastProgressAt = now;
    if (onProgress) onProgress(stats());
  };
  const wakeStartWaiters = ok => startWaiters.splice(0).forEach(fn => fn(ok));
  const maybeWakeStart = () => {
    if (ready >= startBuffer || done) wakeStartWaiters(ready > 0);
    else if (failed || canceled) wakeStartWaiters(false);
  };
  const blankFrame = () => new Uint8Array(fbSize()).fill(0xFF);

  function markConsumed(idx) {
    if (idx > consumed) consumed = idx;
    maybeThrottle();
  }

  function publish(idx, frame) {
    if (idx < 0 || idx >= total || frames[idx]) return;
    frames[idx] = frame;
    produced++;

    const waiters = indexWaiters.get(idx);
    if (waiters) {
      indexWaiters.delete(idx);
      waiters.forEach(fn => fn(frame));
    }

    while (ready < total && frames[ready]) ready++;
    maybeWakeStart();
    emitProgress();
  }

  function settleMissingTail() {
    const fill = lastFrame || blankFrame();
    for (let i = lastTarget + 1; i < total; i++) publish(i, fill);
  }

  function complete() {
    if (done || canceled || failed) return;
    settleMissingTail();
    done = true;
    pausedForBuffer = false;
    try { video.pause(); video.playbackRate = 1; } catch {}
    video.removeEventListener('ended', complete);
    if (cacheFrames && ready === total) animPreEncoded = frames.slice();

    indexWaiters.forEach((waiters, idx) => {
      const frame = frames[idx] || lastFrame || null;
      waiters.forEach(fn => fn(frame));
    });
    indexWaiters.clear();
    wakeStartWaiters(ready > 0);
    emitProgress(true);
  }

  function fail(err) {
    if (done || canceled || failed) return;
    failed = true;
    pausedForBuffer = false;
    try { video.pause(); video.playbackRate = 1; } catch {}
    video.removeEventListener('ended', complete);
    if (err && err.message) animLog('⚠ video encoder: '+err.message);

    indexWaiters.forEach(waiters => waiters.forEach(fn => fn(null)));
    indexWaiters.clear();
    wakeStartWaiters(false);
    emitProgress(true);
  }

  function maybeThrottle() {
    if (done || canceled || failed || noThrottle) return;
    const a = ahead();
    if (!pausedForBuffer && a >= maxAhead) {
      pausedForBuffer = true;
      try { video.pause(); } catch {}
      emitProgress(true);
    } else if (pausedForBuffer && a <= resumeAhead) {
      pausedForBuffer = false;
      video.play().catch(fail);
      emitProgress(true);
    }
  }

  function onFrame(_now, meta) {
    if (done || canceled || failed) return;
    const mediaTime = Number.isFinite(meta.mediaTime) ? meta.mediaTime : video.currentTime;
    const targetIdx = clamp(Math.floor(mediaTime / interval), 0, total - 1);

    if (targetIdx > lastTarget) {
      oct.fillStyle = 'white';
      oct.fillRect(0, 0, tw, th);
      drawScaledRotated(oct, video, video.videoWidth, video.videoHeight, tw, th, rot, scale);
      let frame = null;
      if (encodeFrame) {
        // Temporal tone masks depend on frame order, even when the decoded
        // source image is unchanged, so each output slot gets its own pulse map.
        for (let i = lastTarget + 1; i <= targetIdx; i++) {
          frame = encodeFrame(oct);
          publish(i, frame);
        }
      } else {
        frame = canvasTo1bpp(oct, tw, th, dith, inv, land);
        for (let i = lastTarget + 1; i <= targetIdx; i++) publish(i, frame);
      }
      lastTarget = targetIdx;
      lastFrame = frame;
      if (produced % 10 === 0 || targetIdx === total - 1) {
        if (previewFrame) previewFrame(frame);
        else updateAnimPreview(frame);
      }
    }

    if (lastTarget >= total - 1) complete();
    else {
      maybeThrottle();
      video.requestVideoFrameCallback(onFrame);
    }
  }

  async function prepareAndRun() {
    try {
      video.pause();
      video.playbackRate = 1;
      await new Promise(resolve => {
        let settled = false;
        const finish = () => {
          if (settled) return;
          settled = true;
          clearTimeout(timer);
          video.removeEventListener('seeked', finish);
          video.removeEventListener('loadeddata', finish);
          resolve();
        };
        const timer = setTimeout(finish, 3000);
        video.addEventListener('seeked', finish, {once:true});
        video.addEventListener('loadeddata', finish, {once:true});
        try { video.currentTime = 0; } catch {}
        if (video.readyState >= 2 && video.currentTime < 0.05) finish();
      });
      if (canceled) return;
      video.addEventListener('ended', complete, {once:true});
      video.playbackRate = 16;
      video.requestVideoFrameCallback(onFrame);
      await video.play();
    } catch(e) {
      fail(e);
    }
  }

  const api = {
    stats,
    waitForStart(timeoutMs=10000) {
      if (ready >= startBuffer || done) return Promise.resolve(ready > 0);
      if (failed || canceled) return Promise.resolve(false);
      return new Promise(resolve => {
        let settled = false;
        const finish = ok => {
          if (settled) return;
          settled = true;
          clearTimeout(timer);
          resolve(!!ok);
        };
        const timer = setTimeout(() => finish(ready > 0), timeoutMs);
        startWaiters.push(finish);
      });
    },
    get(i, timeoutMs=15000) {
      i = clamp(Math.floor(i), 0, total - 1);
      markConsumed(i - 1);
      if (frames[i]) { markConsumed(i); return Promise.resolve(frames[i]); }
      if (done) { markConsumed(i); return Promise.resolve(frames[i] || lastFrame); }
      if (failed || canceled) return Promise.resolve(null);
      return new Promise(resolve => {
        let settled = false;
        const finish = frame => {
          if (settled) return;
          settled = true;
          clearTimeout(timer);
          markConsumed(i);
          resolve(frame || null);
        };
        const timer = setTimeout(() => {
          const waiters = indexWaiters.get(i);
          if (waiters) {
            const pos = waiters.indexOf(finish);
            if (pos >= 0) waiters.splice(pos, 1);
            if (!waiters.length) indexWaiters.delete(i);
          }
          if (!done && !canceled && !failed) fail(new Error('frame encode timeout'));
          finish(frames[i] || null);
        }, timeoutMs);
        if (!indexWaiters.has(i)) indexWaiters.set(i, []);
        indexWaiters.get(i).push(finish);
        maybeThrottle();
      });
    },
    cancel() {
      if (canceled) return;
      canceled = true;
      pausedForBuffer = false;
      try { video.pause(); video.playbackRate = 1; } catch {}
      video.removeEventListener('ended', complete);
      indexWaiters.forEach(waiters => waiters.forEach(fn => fn(null)));
      indexWaiters.clear();
      wakeStartWaiters(false);
      emitProgress(true);
    },
    // Wait until ALL frames have been encoded (for pre-encode mode).
    // Disables the ahead-throttle so the video runs at max speed without pausing.
    waitForAll(timeoutMs=300000) {
      // Disable throttle: let video run at full playbackRate without pausing
      noThrottle = true;
      pausedForBuffer = false;
      if (done) { return Promise.resolve(ready === total); }
      if (failed || canceled) { return Promise.resolve(false); }
      // Resume video if it was paused by throttle
      try { video.play().catch(()=>{}); } catch {}
      return new Promise(resolve => {
        let settled = false;
        const finish = ok => {
          if (settled) return;
          settled = true;
          clearTimeout(timer);
          clearInterval(poll);
          resolve(!!ok);
        };
        const timer = setTimeout(() => finish(ready === total || done), timeoutMs);
        // Poll: check every 100ms if done
        const poll = setInterval(() => {
          if (done || ready === total) { finish(true); }
          else if (failed || canceled) { finish(false); }
        }, 100);
      });
    },
  };

  prepareAndRun();
  return api;
}

function drawScaledRotated(ctx, source, srcW, srcH, tw, th, rot, scale='fit') {
  if (!srcW || !srcH) return;
  const swapped = rot===90 || rot===270;
  const effW = swapped ? srcH : srcW;
  const effH = swapped ? srcW : srcH;
  let fw, fh;
  if (scale === 'stretch') {
    fw = tw; fh = th;
  } else if (scale === 'fill') {
    const sc = Math.max(tw/effW, th/effH);
    fw = effW*sc; fh = effH*sc;
  } else { // fit
    const sc = Math.min(tw/effW, th/effH);
    fw = effW*sc; fh = effH*sc;
  }
  ctx.save();
  ctx.translate(tw/2, th/2);
  ctx.rotate(rot * Math.PI/180);
  ctx.drawImage(source, swapped?-fh/2:-fw/2, swapped?-fw/2:-fh/2,
                        swapped? fh : fw,     swapped? fw : fh);
  ctx.restore();
}

// Size the stream preview to the frame geometry (tw×th) the encoder uses.
function animPreviewCtx(tw, th) {
  const canvas = $('anim-preview');
  if (!canvas) return null;
  if (canvas.width !== tw || canvas.height !== th) {
    canvas.width = tw; canvas.height = th;
    const sc = tw < 296 ? Math.ceil(296 / tw) : 1;   // half-res frames: integer upscale
    canvas.style.width = tw * sc + 'px';
    canvas.style.height = th * sc + 'px';
  }
  return canvas.getContext('2d');
}

function updateAnimPreview(buf) {
  const land = $('enc-land').checked;
  const [tw, th] = vsDims(land);
  const pctx = animPreviewCtx(tw, th);
  if (!pctx) return;
  const id = pctx.createImageData(tw, th);
  const data = id.data;
  for (let cy=0; cy<th; cy++) {
    for (let cx=0; cx<tw; cx++) {
      const [bi, m] = vsIdx(cx, cy, land);
      const v = (buf[bi] & m) ? 255 : 0;
      const pi = (cy*tw+cx)*4;
      data[pi]=data[pi+1]=data[pi+2]=v; data[pi+3]=255;
    }
  }
  pctx.putImageData(id, 0, 0);
  $('anim-prev-info').textContent = new Date().toTimeString().slice(0,8) +
    (currentAnimRenderMode() === 'tone-temporal' ? ' — tone temporal mask' : ' — обновлено');
}

function updateToneEstimatePreview(state) {
  if (!state) return;
  const tw = state.tw, th = state.th;
  const pctx = animPreviewCtx(tw, th);
  if (!pctx) return;
  const id = pctx.createImageData(tw, th);
  const data = id.data;

  for (let cy=0; cy<th; cy++) for (let cx=0; cx<tw; cx++) {
    const sx = clamp(Math.floor(cx / state.cell), 0, state.cw - 1);
    const sy = clamp(Math.floor(cy / state.cell), 0, state.ch - 1);
    const dark = state.cur[sy * state.cw + sx] || 0;
    const v = 255 - Math.round(dark * 255);
    const pi = (cy*tw+cx)*4;
    data[pi]=data[pi+1]=data[pi+2]=v; data[pi+3]=255;
  }

  pctx.putImageData(id, 0, 0);
  $('anim-prev-info').textContent = new Date().toTimeString().slice(0,8)+' — tone servo estimate';
}

async function previewFrame0() {
  if (!animFileType) { animLog('Загрузи файл'); return; }
  const land = $('enc-land').checked;
  const [tw, th] = vsDims(land);
  const rot   = parseInt($('enc-rot').value)||0;
  const scale = $('enc-scale').value;
  const renderMode = currentAnimRenderMode();
  const toneVideo = renderMode !== 'mono';
  const dith  = !toneVideo && $('enc-dith').checked;
  const inv   = $('enc-inv').checked;
  const toneState = toneVideo
    ? createToneVideoState(tw, th, parseInt($('enc-tone-cell')?.value) || 2, {
      kind: renderMode === 'tone-servo' ? 'servo' : 'temporal',
      step: parseFloat($('enc-tone-step')?.value) || 0.16,
      smooth: parseFloat($('enc-tone-smooth')?.value) || 0.35,
    })
    : null;
  const oc    = new OffscreenCanvas(tw, th);
  const oct   = oc.getContext('2d', {willReadFrequently:true});
  const buf   = await getAnimFrame(0, Math.max(animTotalFrames,1), 12, oct, tw, th, dith, inv, land, rot, scale, toneState);
  if (buf) animLog('Превью кадра 0 обновлено');
}

function createToneVideoState(tw, th, cell, opts={}) {
  cell = clamp(Math.round(cell) || 2, 1, 8);
  const cw = Math.ceil(tw / cell);
  const ch = Math.ceil(th / cell);
  const acc = new Float32Array(cw * ch);
  const cur = new Float32Array(cw * ch);
  const target = new Float32Array(cw * ch);
  const step = clamp(Number(opts.step) || 0.16, 0.01, 0.8);
  const smooth = clamp(Number(opts.smooth) || 0.35, 0.01, 1);
  for (let y=0; y<ch; y++) for (let x=0; x<cw; x++) {
    // Seed spreads the first temporal decisions spatially instead of flashing
    // the whole picture in phase.
    acc[y*cw+x] = ((x * 13 + y * 7) & 15) / 16;
  }
  return {kind: opts.kind === 'servo' ? 'servo' : 'temporal', tw, th, cell, cw, ch, acc, cur, target, step, smooth};
}

function encodeToneVideoFrame(ctx, tw, th, land, state, inv) {
  return state.kind === 'servo'
    ? canvasToToneServoFrame(ctx, tw, th, land, state, inv)
    : canvasToToneTemporalFrame(ctx, tw, th, land, state, inv);
}

function canvasToToneTemporalFrame(ctx, tw, th, land, state, inv) {
  const id = ctx.getImageData(0, 0, tw, th);
  const d = id.data;
  const buf = new Uint8Array(fbSize()).fill(0xFF);

  for (let cy=0; cy<state.ch; cy++) {
    const y0 = cy * state.cell;
    const y1 = Math.min(th, y0 + state.cell);
    for (let cx=0; cx<state.cw; cx++) {
      const x0 = cx * state.cell;
      const x1 = Math.min(tw, x0 + state.cell);
      let sum = 0, count = 0;

      for (let ly=y0; ly<y1; ly++) for (let lx=x0; lx<x1; lx++) {
        const p = (ly * tw + lx) * 4;
        sum += 0.299*d[p] + 0.587*d[p+1] + 0.114*d[p+2];
        count++;
      }

      const gray = count ? sum / count : 255;
      const dark = inv ? gray / 255 : (255 - gray) / 255;
      const si = cy * state.cw + cx;
      let a = state.acc[si] + dark;
      const blackPulse = a >= 1;
      if (blackPulse) a -= 1;
      state.acc[si] = a;

      if (!blackPulse) continue;
      for (let ly=y0; ly<y1; ly++) for (let lx=x0; lx<x1; lx++) {
        const [bi, m] = vsIdx(lx, ly, land);
        buf[bi] &= ~m;
      }
    }
  }

  return buf;
}

function canvasToToneServoFrame(ctx, tw, th, land, state, inv) {
  const id = ctx.getImageData(0, 0, tw, th);
  const d = id.data;
  const FB = fbSize();
  const buf = new Uint8Array(FB * 2).fill(0xFF);
  const bw = buf.subarray(0, FB);
  const red = buf.subarray(FB);
  const dead = state.step * 0.55;

  for (let cy=0; cy<state.ch; cy++) {
    const y0 = cy * state.cell;
    const y1 = Math.min(th, y0 + state.cell);
    for (let cx=0; cx<state.cw; cx++) {
      const x0 = cx * state.cell;
      const x1 = Math.min(tw, x0 + state.cell);
      let sum = 0, count = 0;

      for (let ly=y0; ly<y1; ly++) for (let lx=x0; lx<x1; lx++) {
        const p = (ly * tw + lx) * 4;
        sum += 0.299*d[p] + 0.587*d[p+1] + 0.114*d[p+2];
        count++;
      }

      const gray = count ? sum / count : 255;
      const desired = inv ? gray / 255 : (255 - gray) / 255;
      const si = cy * state.cw + cx;
      const tgt = state.target[si] + (desired - state.target[si]) * state.smooth;
      state.target[si] = tgt;

      const diff = tgt - state.cur[si];
      let action = 0; // 0=hold/no-op (11), 1=darken (00), -1=lighten (01)
      if (diff > dead) {
        action = 1;
        state.cur[si] = Math.min(1, state.cur[si] + state.step);
      } else if (diff < -dead) {
        action = -1;
        state.cur[si] = Math.max(0, state.cur[si] - state.step);
      }
      if (!action) continue;

      for (let ly=y0; ly<y1; ly++) for (let lx=x0; lx<x1; lx++) {
        const [off, bit] = vsIdx(lx, ly, land);
        if (action > 0) {
          // 00 selects LUT0: darken pulse.
          bw[off] &= ~bit;
          red[off] &= ~bit;
        } else {
          // 01 selects LUT1: lighten pulse. 11 stays hold/no-op via LUT3.
          red[off] &= ~bit;
        }
      }
    }
  }

  return buf;
}

function canvasTo1bpp(ctx, tw, th, dith, inv, land) {
  const id = ctx.getImageData(0, 0, tw, th);
  const d  = id.data;
  const n  = tw*th;
  const g  = new Float32Array(n);
  for (let i=0; i<n; i++) g[i] = 0.299*d[i*4] + 0.587*d[i*4+1] + 0.114*d[i*4+2];
  if (dith) {
    for (let y=0; y<th; y++) for (let x=0; x<tw; x++) {
      const i=y*tw+x, old=g[i], nw=old<128?0:255; g[i]=nw; const e=old-nw;
      if(x+1<tw) g[i+1]+=e*7/16;
      if(y+1<th){ if(x>0) g[i+tw-1]+=e*3/16; g[i+tw]+=e*5/16; if(x+1<tw) g[i+tw+1]+=e/16; }
    }
  }
  const buf = new Uint8Array(fbSize()).fill(0xFF);
  for (let ly=0; ly<th; ly++) for (let lx=0; lx<tw; lx++) {
    const blk = (g[ly*tw+lx]<128) !== inv;
    if (!blk) continue;
    const [bi, m] = vsIdx(lx, ly, land);
    buf[bi] &= ~m;
  }
  return buf;
}

// ── PackBits encoder ──────────────────────────────────────────────
function packBits(data) {
  const out=[]; let i=0, n=data.length;
  while (i<n) {
    let j=i;
    while (j<n-1 && data[j]===data[j+1] && j-i<127) j++;
    if (j-i+1>=3) { out.push(((j-i)&0x7F)|0x80, data[i]); i=j+1; }
    else {
      j=i;
      while (j<n && j-i<128) { if(j+2<n&&data[j]===data[j+1]&&data[j+1]===data[j+2]) break; j++; }
      out.push(j-i-1); for(let k=i;k<j;k++) out.push(data[k]);
      i=j;
    }
  }
  return new Uint8Array(out);
}

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, MSB-first). Must match the
// device's vs_crc16_update() and the Python host's crc16_ccitt().
function crc16ccitt(data) {
  let crc = 0xFFFF;
  for (let n=0; n<data.length; n++) {
    crc ^= data[n] << 8;
    for (let i=0; i<8; i++)
      crc = (crc & 0x8000) ? ((crc<<1) ^ 0x1021) & 0xFFFF : (crc<<1) & 0xFFFF;
  }
  return crc;
}

// Frame a payload. With useCrc the type byte gets the 0x40 flag and a 2-byte
// CRC-16 over the payload is inserted before the 0xBB flush.
// Downsample one physical plane 2×2 (majority, ties → white) to the
// (W/2)×(H/2) plane a VS_HALF_FLAG frame carries; the device doubles it back.
function halfPlane(buf) {
  const pw = PANEL.pw, ph = PANEL.ph, stride = pw >> 3, hstride = pw >> 4, hh = ph >> 1;
  const out = new Uint8Array(hstride * hh);
  for (let r = 0; r < hh; r++) {
    const a = 2 * r * stride, b = a + stride;
    for (let ob = 0; ob < hstride; ob++) {
      const wa = (buf[a + 2*ob] << 8) | buf[a + 2*ob + 1];
      const wb = (buf[b + 2*ob] << 8) | buf[b + 2*ob + 1];
      let o = 0;
      for (let k = 0; k < 8; k++) {
        const sh = 14 - 2 * k;
        const n = ((wa >> sh) & 1) + ((wa >> (sh + 1)) & 1) + ((wb >> sh) & 1) + ((wb >> (sh + 1)) & 1);
        if (n >= 2) o |= 0x80 >> k;
      }
      out[hstride * r + ob] = o;
    }
  }
  return out;
}

// What goes on the wire for a frame: the frame itself, or its half-res
// version (each plane of a dual frame separately) when the stream runs half.
function vsWireFrame(frame) {
  if (!vs.half) return frame;
  const fb = fbSize();
  if (frame.length === fb * 2) {
    const h = new Uint8Array(fb / 2);
    h.set(halfPlane(frame.subarray(0, fb)), 0);
    h.set(halfPlane(frame.subarray(fb)), fb / 4);
    return h;
  }
  return halfPlane(frame);
}
const wireFrameBytes = () => vs.half ? fbSize() / 4 : fbSize();

function vsWrap(type, payload, useCrc) {
  let t = useCrc ? (type | VS_CRC_FLAG) : type;
  if (vs.half) t |= VS_HALF_FLAG;
  const ln = payload.length, extra = useCrc ? 2 : 0;
  const pkt = new Uint8Array(6 + ln + extra);
  pkt[0]=0xAA; pkt[1]=0x55; pkt[2]=t;
  pkt[3]=(ln>>8)&0xFF; pkt[4]=ln&0xFF;
  pkt.set(payload, 5);
  let o = 5 + ln;
  if (useCrc) { const c = crc16ccitt(payload); pkt[o++]=(c>>8)&0xFF; pkt[o++]=c&0xFF; }
  pkt[o] = 0xBB;
  return pkt;
}

function makeVSPkt(frame, enc, prev, useCrc=false) {
  let payload, type;
  const dual = frame.length === fbSize() * 2;
  frame = vsWireFrame(frame);
  if (prev) prev = vsWireFrame(prev);
  if (enc===2) { // DRLE
    if (!prev) { payload=packBits(frame); type=dual ? VS_TYPE_RLE2 : VS_TYPE_RLE; }
    else {
      const d=new Uint8Array(frame.length);
      for(let i=0;i<frame.length;i++) d[i]=frame[i]^prev[i];
      payload=packBits(d); type=dual ? VS_TYPE_DRLE2 : VS_TYPE_DRLE;
    }
  } else if (enc===1) { payload=packBits(frame); type=dual ? VS_TYPE_RLE2 : VS_TYPE_RLE; }
  else { payload=frame; type=dual ? VS_TYPE_RAW2 : VS_TYPE_RAW; }
  return vsWrap(type, payload, useCrc);
}

// Encode one frame; report whether it's a keyframe (full, self-contained). A
// full keyframe is emitted when forced (resync/periodic/first) and when a full
// RLE frame is no larger than the XOR-delta plus keySlack (cheap/static frames
// AND scene cuts) — near-free resync points that bound corruption lifetime.
// Returns {pkt, isKey}.
function encodeVSFrame(frame, prev, enc, useCrc, forceKey, keySlack) {
  const dual = frame.length === fbSize() * 2;
  frame = vsWireFrame(frame);
  if (prev) prev = vsWireFrame(prev);
  if (enc===0) return { pkt: vsWrap(dual?VS_TYPE_RAW2:VS_TYPE_RAW, frame, useCrc), isKey:true };
  if (enc===1 || !prev || forceKey)
    return { pkt: vsWrap(dual?VS_TYPE_RLE2:VS_TYPE_RLE, packBits(frame), useCrc), isKey:true };
  // enc===2 DRLE with a prev: choose delta vs opportunistic cheap keyframe.
  const d = new Uint8Array(frame.length);
  for (let i=0;i<frame.length;i++) d[i]=frame[i]^prev[i];
  const dpay = packBits(d), fpay = packBits(frame);
  if (fpay.length <= dpay.length + keySlack)
    return { pkt: vsWrap(dual?VS_TYPE_RLE2:VS_TYPE_RLE, fpay, useCrc), isKey:true };
  return { pkt: vsWrap(dual?VS_TYPE_DRLE2:VS_TYPE_DRLE, dpay, useCrc), isKey:false };
}

// ═══════════════════════════════════════════════════════════════════
//  LUT editor
// ═══════════════════════════════════════════════════════════════════
const GRP_CLS = ['lc0','lc1','lc2','lc3','lc4'];

function buildLUTEditor() {
  buildVsGrid(); buildTimGrid(); buildDcBars();
}

function buildVsGrid() {
  const cont=$('vs-grid');
  // Headers
  let h='<div style="display:grid;grid-template-columns:80px repeat(7,37px);gap:2px;margin-bottom:3px">';
  h+='<div style="font-size:9px;color:var(--fg2)">Группа \\ Фаза</div>';
  for(let p=0;p<7;p++) h+=`<div style="font-size:9px;color:var(--fg2);text-align:center">${p}</div>`;
  h+='</div>';
  for(let g=0;g<N_GROUPS;g++){
    h+=`<div style="display:flex;align-items:center;gap:2px;margin-bottom:2px">
      <div style="font-size:9px;color:${GRP_COLORS[g]};width:80px;flex-shrink:0">${GRP_LABELS[g]}</div>`;
    for(let p=0;p<7;p++){
      const idx=g*7+p;
      h+=`<input type="text" maxlength="2" class="lc ${GRP_CLS[g]}" data-idx="${idx}"
            value="${hexB(lut[idx])}" oninput="onLC(this,${idx})">`;
    }
    h+='</div>';
  }
  cont.innerHTML=h;
}

function buildTimGrid(){
  const cont=$('tim-grid');
  let h='<div style="display:grid;grid-template-columns:48px repeat(5,37px);gap:2px;margin-bottom:3px">';
  h+='<div style="font-size:9px;color:var(--fg2)">Фаза</div>';
  for(const l of ['TP_A','TP_B','TP_C','TP_D','RP'])
    h+=`<div style="font-size:9px;color:var(--fg2);text-align:center">${l}</div>`;
  h+='</div>';
  for(let p=0;p<7;p++){
    h+=`<div style="display:flex;align-items:center;gap:2px;margin-bottom:2px">
      <div style="font-size:9px;color:var(--fg2);width:48px">Ph ${p}</div>`;
    for(let s=0;s<5;s++){
      const idx=35+p*5+s, cls=s<4?'tp':'rp';
      h+=`<input type="text" maxlength="2" class="lc ${cls}" data-idx="${idx}"
            value="${hexB(lut[idx])}" oninput="onLC(this,${idx})">`;
    }
    h+='</div>';
  }
  cont.innerHTML=h;
}

function buildDcBars(){
  let h='';
  for(let g=0;g<N_GROUPS;g++)
    h+=`<div class="dcrow">
      <div class="dclbl" style="color:${GRP_COLORS[g]}">${GRP_LABELS[g]}</div>
      <div class="dctrack" id="dct${g}"><div class="dccenter"></div><div class="dcfill" id="dcf${g}"></div></div>
      <div class="dcval" id="dcv${g}"></div>
    </div>`;
  $('dc-bars').innerHTML=h;
  renderDC();
}

function onLC(el, idx){
  const v=parseInt(el.value.trim(),16);
  if (isNaN(v)||v<0||v>255){ el.classList.add('bad'); return; }
  el.classList.remove('bad');
  lut[idx]=v;
  drawWf(); renderDC(); renderTicks();
}

function lutToUI(){
  document.querySelectorAll('.lc[data-idx]').forEach(el=>{
    el.value=hexB(lut[+el.dataset.idx]);
    el.classList.remove('bad');
  });
  renderDC(); renderTicks();
}

function renderTicks(){
  const fr=calcTicks(), pr=Math.round(fr*8+550);
  $('tt-fr').textContent=String(fr);
  $('tt-pr').textContent=`~${pr}ms`;
}

function calcTicks(){
  let t=0;
  for(let p=0;p<7;p++){
    const b=35+p*5;
    t+=(lut[b]+lut[b+1]+lut[b+2]+lut[b+3])*(lut[b+4]+1);
  }
  return t;
}

// ── Waveform ────────────────────────────────────────────────────────
function decodeWF(g){
  const segs=[];
  for(let p=0;p<7;p++){
    const vs=lut[g*7+p], b=35+p*5;
    const tp=[lut[b],lut[b+1],lut[b+2],lut[b+3]], rp=lut[b+4];
    for(let r=0;r<=rp;r++)
      for(let s=0;s<4;s++){
        const v=(vs>>((3-s)*2))&3, d=tp[s];
        if(d>0) segs.push({v,d});
      }
  }
  return segs;
}

const VS_V = [0,1,-1,1/3];
const VS_C = ['#4a4a5a','#e07070','#60a0f0','#d09040'];

function drawWf(){
  const canvas=$('wf-canvas');
  const W=canvas.clientWidth; if(!W){setTimeout(drawWf,50);return;}
  const H=260;
  // HiDPI: back the canvas at device resolution and draw in CSS px, otherwise
  // phones (DPR 2–3) upscale a 1× bitmap and the plot turns into a blur.
  const dpr=Math.min(window.devicePixelRatio||1,3);
  canvas.width=Math.round(W*dpr); canvas.height=Math.round(H*dpr);
  canvas.style.height=H+'px';
  const ctx=canvas.getContext('2d');
  ctx.setTransform(dpr,0,0,dpr,0,0);
  ctx.clearRect(0,0,W,H);

  const ml=68,mr=8,mt=7,mb=34;
  const pw=W-ml-mr, ph=H-mt-mb;
  const th=ph/N_GROUPS;

  const allS=Array.from({length:N_GROUPS},(_,g)=>decodeWF(g));
  const totT=Math.max(...allS.map(s=>s.reduce((a,{d})=>a+d,0)),1);

  const phF=[];
  for(let p=0;p<7;p++){const b=35+p*5; phF.push((lut[b]+lut[b+1]+lut[b+2]+lut[b+3])*(lut[b+4]+1));}

  for(let g=0;g<N_GROUPS;g++){
    const ty=mt+g*th, mid=ty+th/2;
    const segs=allS[g], col=GRP_COLORS[g];
    const vcomBad=g===4&&Array.from({length:7},(_,p)=>lut[28+p]).some(b=>b!==0);

    // Background
    ctx.fillStyle=vcomBad?'#2a0606':'#16161e';
    ctx.fillRect(ml,ty,pw,th-2);

    // Group label
    ctx.font='9px monospace'; ctx.textAlign='right';
    ctx.fillStyle=vcomBad?'#f05050':col;
    ctx.fillText(GRP_LABELS[g],ml-4,mid+3);

    // Reference lines
    const yv={vsh1:ty+th*.14, vsh2:ty+th*.28, vss:ty+th*.5, vsl:ty+th*.86};
    ctx.setLineDash([2,5]); ctx.lineWidth=.5;
    [[yv.vsh1,'#e0707044'],[yv.vsh2,'#d0904044'],[yv.vss,'#38384c'],[yv.vsl,'#60a0f044']].forEach(([y,c])=>{
      ctx.strokeStyle=c; ctx.beginPath(); ctx.moveTo(ml,y); ctx.lineTo(W-mr,y); ctx.stroke();
    });
    ctx.setLineDash([]);

    // Ref labels
    ctx.font='7px monospace'; ctx.textAlign='right';
    [[yv.vsh1,'+15V','#e0707060'],[yv.vsh2,'+5V','#d0904060'],[yv.vsl,'-15V','#60a0f060']].forEach(([y,l,c])=>{
      ctx.fillStyle=c; ctx.fillText(l,ml-2,y+2);
    });

    if(vcomBad){
      ctx.fillStyle='#f0505030'; ctx.fillRect(ml,ty,pw,th-2);
      ctx.fillStyle='#f05050'; ctx.textAlign='center'; ctx.font='bold 9px monospace';
      ctx.fillText('⚠ VCOM NON-ZERO — DISPLAY DISASTER',ml+pw/2,mid+3);
      continue;
    }
    if(!segs.length){
      ctx.fillStyle='#404050'; ctx.textAlign='center'; ctx.font='9px monospace';
      ctx.fillText('(inactive)',ml+pw/2,mid+3); continue;
    }

    // Waveform
    ctx.lineWidth=2;
    let x=ml, py_=null;
    const yM=[yv.vss,yv.vsh1,yv.vsl,yv.vsh2];
    for(const{v,d}of segs){
      const px=Math.max(1,Math.round(d/totT*pw)), y=yM[v], c=VS_C[v];
      if(py_!==null&&py_!==y){ctx.beginPath();ctx.strokeStyle=c;ctx.moveTo(x,py_);ctx.lineTo(x,y);ctx.stroke();}
      ctx.beginPath();ctx.strokeStyle=c;ctx.moveTo(x,y);ctx.lineTo(x+px,y);ctx.stroke();
      ctx.fillStyle=c+'3a';
      ctx.fillRect(x,Math.min(y,yv.vss),px,Math.abs(y-yv.vss));
      x+=px; py_=y;
    }
  }

  // Phase dividers + labels
  ctx.setLineDash([3,3]); ctx.lineWidth=1; let cum=0;
  for(let p=0;p<7;p++){
    const f=phF[p], xs=ml+Math.round(cum/totT*pw), xe=ml+Math.round((cum+f)/totT*pw), xm=(xs+xe)/2;
    if(p>0){ctx.strokeStyle='#383850';ctx.beginPath();ctx.moveTo(xs,mt);ctx.lineTo(xs,H-mb);ctx.stroke();}
    ctx.setLineDash([]);
    ctx.font='8px monospace'; ctx.textAlign='center';
    ctx.fillStyle=f>0?'#808090':'#404050';
    ctx.fillText('Ph'+p, f>0?xm:xs+5, H-mb+10);
    if(f>0){ctx.fillStyle='#505060'; ctx.fillText(f+'f',xm,H-mb+20);}
    ctx.setLineDash([3,3]); cum+=f;
  }
  ctx.setLineDash([]);
}

// ── DC balance bars ───────────────────────────────────────────────
function dcBal(g){
  const segs=decodeWF(g);
  let pos=0,neg=0,bal=0;
  for(const{v,d}of segs){const c=VS_V[v]*d; bal+=c; if(c>0)pos+=c; else neg-=c;}
  return{pos,neg,bal};
}

function renderDC(){
  for(let g=0;g<N_GROUPS;g++){
    const{pos,neg,bal}=dcBal(g);
    const tot=(pos+neg)||1, warn=Math.abs(bal)>10;
    const track=$(`dct${g}`), fill=$(`dcf${g}`), val=$(`dcv${g}`);
    const tw=track.offsetWidth||200, cx=tw/2, mh=cx-2;
    if(Math.abs(bal)<.5){
      fill.style.cssText=`left:${cx-2}px;width:4px;background:var(--green)`;
      val.textContent='OK (balanced)'; val.style.color='var(--green)';
    } else if(bal>0){
      const w=Math.min(mh,Math.round(bal/tot*mh*2));
      fill.style.cssText=`left:${cx}px;width:${Math.max(w,2)}px;background:${warn?'var(--red)':'var(--vgh)'}`;
      val.textContent=`+${bal.toFixed(1)} DC HIGH${warn?' ⚠':''}`;
      val.style.color=warn?'var(--red)':'var(--orange)';
    } else {
      const w=Math.min(mh,Math.round(-bal/tot*mh*2));
      fill.style.cssText=`left:${cx-Math.max(w,2)}px;width:${Math.max(w,2)}px;background:${warn?'var(--red)':'var(--vgl)'}`;
      val.textContent=`${bal.toFixed(1)} DC LOW${warn?' ⚠':''}`;
      val.style.color=warn?'var(--red)':'var(--orange)';
    }
  }
}

// ── LUT actions ────────────────────────────────────────────────────
function applyLut(){
  send('LUTW:'+hexOf(lut).toUpperCase()+'\nAPPLY\n');
}
function getLut(){ lgetBuf=Array(7).fill(null); send('LGET\n'); }
function loadFactory(){ lut.set(FACTORY_LUT); lutToUI(); drawWf(); logI('Factory LUT loaded'); }
function loadAnim()   { lut.set(ANIM_LUT);    lutToUI(); drawWf(); logI('Anim partial LUT loaded'); }
function adjTP(d){ for(let p=0;p<7;p++) for(let s=0;s<4;s++){const i=35+p*5+s; lut[i]=clamp(lut[i]+d,0,255);} lutToUI(); drawWf(); }
function adjRP(d){ for(let p=0;p<7;p++){const i=35+p*5+4; lut[i]=clamp(lut[i]+d,0,255);} lutToUI(); drawWf(); }

function togglePasteDlg(){
  const d=$('paste-dlg'); d.style.display=d.style.display==='none'?'':'none';
}
function doPaste(){
  const raw=$('paste-txt').value;
  const hex=raw.replace(/\/\/[^\n]*/g,'').replace(/\/\*[\s\S]*?\*\//g,'')
               .replace(/0[xX]/g,'').replace(/[^0-9a-fA-F]/g,'');
  if(hex.length!==LUT_SZ*2){
    alert(`Нужно ${LUT_SZ*2} hex-символов (${LUT_SZ} байт).\nПолучено: ${hex.length} (${hex.length/2} байт)`);
    return;
  }
  for(let i=0;i<LUT_SZ;i++) lut[i]=parseInt(hex.slice(i*2,i*2+2),16);
  lutToUI(); drawWf();
  togglePasteDlg();
  logI(`LUT paste OK (${LUT_SZ} bytes)`);
}
function exportLut(){
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([hexOf(lut).toUpperCase()+'\n'],{type:'text/plain'}));
  a.download='lut.hex'; a.click(); URL.revokeObjectURL(a.href);
}

// ═══════════════════════════════════════════════════════════════════
//  Init
// ═══════════════════════════════════════════════════════════════════
document.addEventListener('DOMContentLoaded', ()=>{
  clearCanvas();
  setOutputMode('dither', false);
  setAnimRenderMode('mono');
  buildLUTEditor();
  lutToUI(); renderTele(); renderTicks();
  const ro=new ResizeObserver(()=>drawWf());
  ro.observe($('wf-canvas'));
  initBotResize();
  if (isPhoneLayout()) toggleBotCon();   // phones: console is a closed sheet until asked for
  // Folded sections remember their state per device (keyed by their title).
  document.querySelectorAll('details.card, details.sub').forEach(d => {
    const sum = d.querySelector('summary');
    const key = 'fold:' + (sum ? sum.textContent.trim() : '');
    try { if (localStorage.getItem(key) === '1') d.open = true; } catch {}
    d.addEventListener('toggle', () => { try { localStorage.setItem(key, d.open ? '1' : '0'); } catch {} });
  });
  applyDeepLink();   // принять #n=…&k=… из NFC-метки
  nfcRefresh();
});

// ── Bottom console resize + collapse ─────────────────────────────
function initBotResize() {
  const bc=$('bot-con'), rh=$('bot-resize');
  let dragging=false, startY=0, startH=0;

  rh.addEventListener('pointerdown', e=>{
    if (bc.classList.contains('collapsed')) return;
    dragging=true; startY=e.clientY; startH=bc.offsetHeight;
    rh.classList.add('drag');
    rh.setPointerCapture?.(e.pointerId);
    document.body.style.userSelect='none';
    document.body.style.cursor='ns-resize';
    e.preventDefault();
  });

  document.addEventListener('pointermove', e=>{
    if (!dragging) return;
    const delta = startY - e.clientY;
    const maxH = Math.max(120, Math.min(600, window.innerHeight * 0.7));
    const newH = Math.max(24, Math.min(maxH, startH + delta));
    bc.style.height = newH+'px';
    bc.style.transition = 'none';
    if (newH <= 28) bc.classList.add('collapsed');
    else            bc.classList.remove('collapsed');
    $('bot-tog').textContent = bc.classList.contains('collapsed') ? '▲' : '▼';
  });

  document.addEventListener('pointerup', e=>{
    if (!dragging) return;
    rh.releasePointerCapture?.(e.pointerId);
    dragging=false; rh.classList.remove('drag');
    document.body.style.userSelect='';
    document.body.style.cursor='';
    bc.style.transition='';
  });
}

function toggleBotCon() {
  const bc=$('bot-con');
  if (bc.classList.contains('collapsed')) {
    bc.classList.remove('collapsed');
    bc.style.height='200px';
    $('bot-tog').textContent='▼';
  } else {
    bc.classList.add('collapsed');
    bc.style.height='24px';
    $('bot-tog').textContent='▲';
  }
}

// ═══════════════════════════════════════════════════════════════════
//  Virtual LUT (VLUT) — JS API для работы с сессионными LUT-слотами
// ═══════════════════════════════════════════════════════════════════

// Локальный кеш определённых слотов (отражает состояние МК в текущей сессии)
const vlutSlots = new Array(8).fill(null); // null = не определён

function vlutList() { send('VLUT:LIST\n'); }
function vlutClear() {
  send('VLUT:CLEAR\n');
  vlutSlots.fill(null);
}
function vlutOff() { send('VLUT:OFF\n'); }

function vlutDefine() {
  const slot    = parseInt($('vlut-slot').value) || 0;
  const base    = $('vlut-base').value;
  const patches = $('vlut-patches').value.trim();
  if (!patches) { alert('Укажи патчи: offset=hex,offset=hex,...'); return; }
  const cmd = `VLUT:${slot}:${base}:${patches}\n`;
  send(cmd);
  // Сохраняем в локальный кеш
  vlutSlots[slot] = { base: parseInt(base), patches };
  logI(`VLUT[${slot}] определён: base=${base}, patches=${patches}`);
}

function vlutActivate() {
  const slot = parseInt($('vlut-slot').value) || 0;
  send(`VLUT:${slot}\n`);
}

/**
 * Программно определить и активировать виртуальный слот.
 * Используется другими JS-функциями (например sendCanvasToneSoft может
 * создать свой VLUT-слот с настроенными параметрами вместо хардкода).
 *
 * @param {number} slot     - номер слота 0..7
 * @param {number} base     - базовый режим 0..9
 * @param {string} patches  - строка патчей "35=01,36=01"
 * @param {boolean} activate - активировать после определения
 * @returns {Promise<void>}
 */
async function vlutDefineAndActivate(slot, base, patches, activate = true) {
  const defCmd = `VLUT:${slot}:${base}:${patches}\n`;
  await sendBytes(new TextEncoder().encode(defCmd));
  vlutSlots[slot] = { base, patches };
  if (activate) {
    await sleep(50);
    await sendBytes(new TextEncoder().encode(`VLUT:${slot}\n`));
  }
}

// ═══════════════════════════════════════════════════════════════════
//  System Info & OTA DFU
// ═══════════════════════════════════════════════════════════════════

// --- Sysinfo state ---
const sysState = {
  fwVersion: null,  // "3.0.1"
  buildDate: null,  // "2025-06-12_14:30:00"
  uptimeSec: 0,
  battMv: 0,
  mahUsed: 0,       // mAh × 1000
  curUa: 0,
  lastUpdate: null,
  layout: null,     // 1=legacy, 2=v2; null = old fw that doesn't report it
  panel: '128x296', // SYSINFO panel=WxH (physical); see PANELS
  secOn: false,     // device access gate active
  authed: false,    // this connection authenticated
  picture: null,    // SYSINFO mode=pic|clock (3.4.22+); null = the firmware does not say
  wallUnix: null,   // device wall clock (unix sec), из STATS wall=
};

// Авто-выставление времени делаем не больше раза за подключение
// (TIME=… дёргает полную перерисовку e-ink — не хочется зря жечь refresh).
let autoTimeDoneThisConn = false;

const BATT_CAPACITY_MAH = 350;   // fallback only: what the first batch shipped with

// «Осталось»: считается от ЗАЯВЛЕННОЙ ёмкости (карточка «Энергосбережение») и
// от СРЕДНЕГО тока профиля (est из PWR) — не от мгновенного cur_ua, который
// при подключённом телефоне включает ток соединения и вдесятеро выше того,
// что ценник тратит в одиночестве. Без ответа PWR срок не выдумываем.
function renderRemaining() {
  const s = sysState;
  const pk = pwrLast;
  const declared = pk && pk.cell > 0 ? pk.cell * (pk.par || 1) : 0;
  const cap = declared || BATT_CAPACITY_MAH;
  const used = (pk && declared) ? pk.used : s.mahUsed / 1000;     // since the pack went in · or all-time
  const remain = Math.max(0, cap - used);
  const capEl = $('sys-cap');
  if (capEl) capEl.textContent = declared ? `${cap} mAh` : `${cap} mAh (не задана — взято по умолчанию)`;
  if (pk && pk.chem === PWR_CHEM_MAINS) {
    $('sys-remain').textContent = 'постоянное питание — не разряжается';
    $('sys-eta').textContent = '∞';
    return;
  }
  const est = pk && pk.est > 0 ? pk.est : 0;
  // Дни / месяцы / годы — та же шкала, что в карточке «Энергосбережение»,
  // чтобы два места на экране не расходились в формулировке.
  const days = est > 0 ? remain / (est * 24 / 1000) : -1;
  const etaStr = days < 0 ? null
               : days >= 1 ? pwrDaysText(Math.round(days))
               : `~${(days * 24).toFixed(1)} ч`;
  const why = !connected ? '—'
            : pwrSupported === false ? '— (нужна прошивка 3.4.13+)'
            : '— (жду ответ ценника)';

  // Оценка идёт СЮДА, в строку «Остаток» карточки «Устройство»: карточка
  // «Батарея» с полем «Осталось (ожид.)» лежит ниже по странице, и до неё
  // надо доскроллить — цифру искали и не находили именно из-за этого.
  $('sys-remain').textContent =
    `${remain.toFixed(0)} mAh` + (declared ? ` из ${cap}` : ' (от 350 по умолчанию)') +
    (etaStr ? ` · хватит ${etaStr}` : '');
  $('sys-eta').textContent = etaStr ? `${etaStr} (оценка при ${est} мкА)` : why;
}
// ADC ceiling is 3600 mV (gain 1/6, ref 0.6V). Above that = "full/charging".
// We can only track discharge below 3600. Map 3000–3600 → 0–100%.
const BATT_V_FULL = 3600;
const BATT_V_EMPTY = 3000;

let _sysinfoTimers = [];
function requestSysinfo() {
  if (!connected) return;
  // The tag's BLE TX pool holds 3 notifications and ble_printf gives up after
  // a short retry, less than one idle connection interval (100–200 ms), so a
  // burst of replies loses its tail silently — which is how the STATS fields
  // (and the wall clock that drives auto time-set) stayed «—» while SYSINFO,
  // first out of the gate, always arrived. SYSINFO alone is already four
  // notifications; everything else goes out spaced wider than an interval, so
  // no reply burst outgrows the pool. Firmware <3.4.1 answers no MESHRX at all.
  _sysinfoTimers.forEach(clearTimeout); _sysinfoTimers = [];
  const later = (ms, fn) => _sysinfoTimers.push(setTimeout(() => { if (connected) fn(); }, ms));
  send('SYSINFO\n');
  later(350,  () => { send('BATT\n'); send('NAME\n'); });
  later(700,  () => send('MESHRX\n'));
  later(1050, () => send('STATS\n'));       // two lines: live counters + flash snapshot
  later(1400, pwrQuery);
  // Re-ask what did not answer (firmware ≤3.4.2 never retried a dropped reply).
  later(2500, () => {
    const mrx = $('sys-meshrx'), fl = $('st-flash');
    if (mrx && mrx.textContent === '—') send('MESHRX\n');
    if (fl && fl.textContent === '—') later(350, () => send('STATS\n'));
  });
}

// ── Mesh-приём (MESHRX) ───────────────────────────────────────────
// Прошивка отвечает «meshrx=on» или «meshrx=off (re-enable over NUS only)».
function parseMeshrx(line) {
  const on = Proto.parseMeshrx(line).on;
  const el = $('sys-meshrx');
  if (!el) return;
  el.textContent = on ? 'включён' : 'выключен (экономия ~0.25 мА)';
  el.style.color = on ? 'var(--green)' : 'var(--orange)';
}

async function setMeshRx(on) {
  if (!connected) { alert('Не подключено'); return; }
  send(`MESHRX ${on ? 'ON' : 'OFF'}\n`);
  const reply = await waitForLine(/^meshrx=/, 4000);
  if (reply) logI('Mesh-приём: ' + reply.slice('meshrx='.length));
  else logE('MESHRX: нет ответа (прошивка старше v3.4.1? включена SEC-защита?)');
}

// ── Шаблон сна (PWR) ──────────────────────────────────────────────
// Прошивка отвечает «PWR:day=1 night=5 from=23 to=7 advc=2 advp=5 night_now=0 next=123»
// (next — секунд до ближайшей перерисовки по расписанию, -1 = не обновляется).
//
// Селекты — это ЗНАЧЕНИЯ С ЦЕННИКА, не предустановка: до первого ответа они
// приглушены и показывают умолчания, ответ PWR перезаписывает их и «оживляет».
// Пока подключены, профиль перечитывается раз в 30 с (и в момент, когда
// отсчёт до перерисовки дошёл до нуля), так что изменение с другого телефона
// или по mesh-рассылке здесь тоже появится. При отключении всё сбрасывается,
// чтобы следующий ценник не унаследовал чужие цифры.
const PWR_TICK_OPTS = [[1, 'каждую минуту'], [2, 'раз в 2 мин'], [5, 'раз в 5 мин'], [15, 'раз в 15 мин'], [0, 'выкл']];
const PWR_ADV_OPTS  = [[2, '2 с'], [5, '5 с'], [10, '10 с']];
const PWR_HOUR_OPTS = [...Array(24).keys()].map(h => [h, String(h).padStart(2, '0') + ':00']);
const PWR_DEFAULTS  = { day: 1, night: 5, from: 23, to: 7, advc: 2, advp: 5 };
const PWR_IDS       = ['pwr-day', 'pwr-night', 'pwr-from', 'pwr-to', 'pwr-advc', 'pwr-advp'];
// Химия → [id, подпись, мАч на элемент, S, P, номинал элемента мВ]. id = enum power_chem в прошивке.
const PWR_CHEMS = [
  [1, 'Li-ion / Li-Po (3.7 В)',              370, 1, 1, 3700],
  [2, 'Щелочные 1.5 В (AA/AAA/LR44…)',       2500, 2, 1, 1500],
  [5, 'Литиевая таблетка CR2032/CR2450 (3 В)', 230, 1, 1, 3000],
  [6, 'Литиевые AA/AAA 1.5 В (LiFeS2)',      3000, 2, 1, 1500],
  [3, 'LiFePO4 (3.2 В)',                      500, 1, 1, 3200],
  [4, 'NiMH (1.2 В)',                        2000, 2, 1, 1200],
  [7, 'Постоянное питание (USB / сеть)',        0, 1, 1, 0],
  [0, 'неизвестно / своё',                      0, 1, 1, 0],
];
const PWR_CHEM_MAINS = 7;
const PWR_ADC_CEIL_MV = 3600;
const PWR_CELLS = [[1, '1'], [2, '2'], [3, '3'], [4, '4']];
function pwrChem(id) { return PWR_CHEMS.find(c => c[0] === id) || PWR_CHEMS[PWR_CHEMS.length - 1]; }
let pwrPreviewT = null;
const PWR_POLL_MS   = 30000;
// Ценник занят надолго командой, после которой он не отвечает (CLEAN ~1 мин,
// NUKE:N ~45 с на цикл): до этого момента в эфир не лезем.
let deviceBusyUntil = 0;
function markDeviceBusy(ms) { deviceBusyUntil = Math.max(deviceBusyUntil, Date.now() + ms); }
// Опрос профиля живёт только на вкладке «Система» и только когда эфир свободен:
// не идёт OTA, не льётся картинка (sendLock), не крутится поток/анимация
// (liveTimer), ценник не занят чисткой. Любая NUS-запись поверх чужой — это
// «GATT operation already in progress» на телефоне.
// Занят ли эфир ПРЯМО СЕЙЧАС. Именно «сейчас»: ota.phase='reboot' со
// stalled=true означает, что обновление сдалось и ждёт человека — эфир при
// этом свободен, а раньше такой залипший статус навсегда выключал опрос
// профиля, и «Осталось (ожид.)» оставалось прочерком до перезагрузки страницы.
function airBusy() {
  if (ota.phase === 'download' || ota.phase === 'upload' || ota.phase === 'mark') return true;
  if (ota.phase === 'reboot' && !ota.stalled) return true;   // ждём возврата ценника
  return false;
}
function pwrMayPoll() {
  return connected && curTab === 'sys' && !airBusy() && !sendLock &&
         liveTimer === null && Date.now() >= deviceBusyUntil;
}
const PWR_MIN_FW    = '3.4.13';
let pwrSupported = null;       // null = ещё не знаем, false = прошивка старее PWR_MIN_FW
let pwrKnown = false;          // true = селекты отражают ответ ценника
// Пользователь начал править карточку и ещё не нажал «Применить»/«Сохранить».
// Пока так — ни один ответ ценника (а он приходит каждые 30 с) не трогает
// поля: переписывать чужой несохранённый ввод недопустимо. Сбрасывается
// применением, отключением и кнопкой «По умолчанию».
let pwrDirty = false;
let pwrLast  = null;           // последний разобранный ответ
let pwrNext  = -1;             // локальный отсчёт секунд до перерисовки
let pwrPollT = null, pwrTickT = null;

// Заполняет список один раз, потом только выставляет значение; незнакомое
// значение из прошивки (например day=30, заданное командой) добавляется как есть.
function pwrFill(id, opts, val) {
  const el = $(id); if (!el) return;
  if (!el.options.length) for (const [v, t] of opts) el.append(new Option(t, v));
  if (val === undefined || val === null || Number.isNaN(val)) return;
  if (![...el.options].some(o => +o.value === +val)) el.append(new Option(String(val), val));
  el.value = String(val);
}
function pwrSetAll(v) {
  pwrFill('pwr-day', PWR_TICK_OPTS, v.day);   pwrFill('pwr-night', PWR_TICK_OPTS, v.night);
  pwrFill('pwr-from', PWR_HOUR_OPTS, v.from); pwrFill('pwr-to', PWR_HOUR_OPTS, v.to);
  pwrFill('pwr-advc', PWR_ADV_OPTS, v.advc);  pwrFill('pwr-advp', PWR_ADV_OPTS, v.advp);
}
function pwrMarkKnown(known) {
  pwrKnown = known;
  for (const id of PWR_IDS) { const el = $(id); if (el) el.style.opacity = known ? '' : '0.45'; }
}
function pwrSetEnabled(on) {
  for (const id of PWR_IDS) { const el = $(id); if (el) el.disabled = !on; }
  for (const b of document.querySelectorAll('#pwr-btns button')) b.disabled = !on;
}
// Видимый признак, что в полях есть несохранённое.
function pwrDirtyHint() {
  const el = $('pwr-preview'); if (!el) return;
  if (pwrDirty && !el.innerHTML) {
    el.innerHTML = 'есть несохранённые изменения — нажми «Применить» (профиль) или «Сохранить» (батарея)';
    el.hidden = false;
  }
}
function pwrClearDirty() {
  pwrDirty = false;
  const el = $('pwr-preview'); if (el) { el.hidden = true; el.innerHTML = ''; }
}
function pwrStatus(text, color) {
  const el = $('sys-pwr'); if (!el) return;
  el.textContent = text; el.style.color = color || 'var(--fg2)';
}
function pwrTickText(m) { return m === 0 ? 'выкл' : m === 1 ? 'каждую минуту' : `раз в ${m} мин`; }
function pwrRender() {
  if (!pwrKnown || !pwrLast) return;
  const kv = pwrLast;
  const next = pwrNext >= 0
    ? `след. через ${Math.floor(pwrNext / 60)}:${String(pwrNext % 60).padStart(2, '0')}`
    : 'по расписанию не обновляется';
  pwrStatus(`${kv.night_now ? 'ночь' : 'день'} · ${pwrTickText(kv.night_now ? kv.night : kv.day)} · ${next}`, 'var(--green)');
}
function pwrStopTimers() {
  clearInterval(pwrPollT); pwrPollT = null;
  clearInterval(pwrTickT); pwrTickT = null;
}
function pwrDaysText(d) {
  if (d < 0) return null;
  if (d >= 365) return `~${(d / 365).toFixed(1)} г.`;
  if (d >= 60)  return `~${Math.round(d / 30)} мес.`;
  return `~${d} дн.`;
}
// Строка оценки: что предсказывает модель прошивки для этого профиля.
// Это модель, не измерение — та же, что считает mAh на экране ценника.
function pwrEstText(kv, prefix) {
  if (!(kv.est > 0)) return '';
  const mahDay = (kv.est * 24 / 1000).toFixed(2);
  let t = `${prefix} ≈ <b>${kv.est} мкА</b> · ${mahDay} мАч/сут`;
  const d = pwrDaysText(kv.days);
  if (d) t += ` · хватит ещё <b>${d}</b>`;
  return t;
}
function pwrPackSummary() {
  const el = $('pwr-pack'); if (!el) return;
  const c = pwrChem(+$('pwr-chem').value), ser = +$('pwr-ser').value, par = +$('pwr-par').value;
  const cell = +($('pwr-cell').value || 0);
  if (c[0] === PWR_CHEM_MAINS) { el.textContent = 'ничего не разряжается — срок не считается'; return; }
  const cap = cell * par, vnom = c[5] * ser;
  el.textContent = (cap ? `итого ${cap} мАч` : 'ёмкость не задана') + (vnom ? ` · ${(vnom / 1000).toFixed(1)} В номинал` : '') +
    ((vnom && vnom > PWR_ADC_CEIL_MV) ? ' · выше 3.6 В АЦП не видит' : '');
}
function pwrRenderEstimate(kv) {
  const el = $('pwr-est'); if (!el) return;
  const mains = kv.chem === PWR_CHEM_MAINS;
  const cap = (kv.cell || 0) * (kv.par || 1);
  let html = '';
  if (kv.est > 0) {
    html = mains
      ? `Потребление ≈ <b>${kv.est} мкА</b> · ${(kv.est * 24 / 1000).toFixed(2)} мАч/сут · <b>постоянное питание — срок не ограничен</b>`
      : pwrEstText(kv, 'Сейчас') + (cap > 0 ? ` (использовано ${kv.used} из ${cap} мАч)` : '');
    if (kv.base > 0) {
      const k = (kv.base / kv.est).toFixed(1);
      html += `<br>Без экономии (mesh вкл, каждую минуту круглосуточно, реклама 2 с) ≈ ${kv.base} мкА — <b>в ${k}× больше</b>`;
    }
  }
  if (!mains && kv.mv > 0) {
    const parts = [];
    if (kv.vsoc >= 0) parts.push(kv.vsat ? `по напряжению <b>≥ ${kv.vsoc}%</b> (АЦП в потолке 3.6 В, реально выше)` : `по напряжению <b>${kv.vsoc}%</b>`);
    if (cap > 0) parts.push(`по счётчику <b>${Math.max(0, Math.min(100, Math.round((cap - kv.used) / cap * 100)))}%</b>`);
    if (kv.soc >= 0 && cap > 0) parts.push(`на экране ценника <b>${kv.soc}%</b> (меньшее из двух)`);
    if (parts.length) html += `<br>Заряд: ${parts.join(' · ')}`;
  }
  el.innerHTML = html || '—';
  const sb = $('sys-bat');
  if (sb) {
    const c = pwrChem(kv.chem);
    sb.textContent = mains ? c[1] : (cap > 0 ? `${c[1]}, ${kv.ser}S${kv.par}P, ${cap} мАч` : 'ёмкость не задана — срок не считается');
    sb.style.color = (mains || cap > 0) ? 'var(--green)' : 'var(--orange)';
  }
}
// Батарейная карточка сверху: пока прошивка не знала химию, процент был
// линейкой 3.0–3.6 В. С кривой — честный процент там, где АЦП видит, и
// «≥ N%» там, где он упёрся в потолок.
// Батарея как бухгалтерия, а не как проценты от напряжения: первым числом —
// сколько ушло с момента установки и сколько заявлено, полоса — остаток.
// Напряжение остаётся как страховка снизу (кривая, где АЦП её видит), а
// «дней» — как оценка по среднему току профиля, с пометкой. Тот же soc, что
// рисует сам ценник (battery_percent), чтобы страница и панель не расходились.
function pwrRenderBatteryCard(kv) {
  if (!(kv.mv > 0) || !(kv.soc >= 0)) return;
  const b = $('sys-batt'), bar = $('sys-batt-bar'), pl = $('sys-batt-pct');
  if (kv.chem === PWR_CHEM_MAINS) {
    if (b) b.innerHTML = `<span style="color:var(--green)">постоянное питание · ${kv.mv} mV</span>`;
    if (bar) bar.style.width = '100%';
    if (pl) pl.textContent = '∞';
    battChip(100, kv.mv, false);
    return;
  }
  const cap = (kv.cell || 0) * (kv.par || 1);
  const pct = kv.soc;
  const color = pct > 50 ? 'var(--green)' : pct > 20 ? 'var(--orange)' : 'var(--red)';
  let head;
  if (cap > 0) {
    const left = Math.max(0, cap - kv.used);
    head = `использовано <b>${kv.used}</b> из ${cap} мАч · осталось <b>${left} мАч</b>`;
    if (kv.vsoc >= 0 && !kv.vsat && kv.vsoc < pct) head += ` · по напряжению меньше: ${kv.vsoc}%`;
    else if (kv.vsoc >= 0) head += kv.vsat ? ` · напряжение ≥ ${kv.mv} mV` : ` · ${kv.mv} mV`;
  } else {
    head = kv.vsat ? `≥${kv.mv} mV (по кривой ≥ ${kv.vsoc}%) · ёмкость не задана` : `${kv.mv} mV (по кривой ${pct}%) · ёмкость не задана`;
  }
  const ceilingOnly = cap === 0 && kv.vsat;
  if (b) b.innerHTML = `<span style="color:${color}">${head}</span>`;
  if (bar) bar.style.width = (ceilingOnly ? 100 : pct) + '%';
  if (pl) pl.textContent = ceilingOnly ? `≥${kv.vsoc}%` : `${pct}%`;
  battChip(ceilingOnly ? 100 : pct, kv.mv, false);
}
function pwrChemPreset() {
  const c = pwrChem(+$('pwr-chem').value);
  pwrFill('pwr-ser', PWR_CELLS, c[3]); pwrFill('pwr-par', PWR_CELLS, c[4]);
  const ci = $('pwr-cell'); if (ci) ci.value = c[2] || '';
  pwrPackSummary(); pwrPreview();
}
function pwrPackKeys() {
  const v = id => +$(id).value;
  return `chem=${v('pwr-chem')},ser=${v('pwr-ser')},par=${v('pwr-par')},cell=${+($('pwr-cell').value || 0)}`;
}
async function saveBattery(newbat) {
  if (!connected) { alert('Не подключено'); return; }
  if (pwrSupported === false) { alert(`Эта прошивка (${sysState.fwVersion}) батарею не хранит — обновите до ${PWR_MIN_FW}+`); return; }
  send(`PWR:${pwrPackKeys()}${newbat ? ',newbat=1' : ''}\n`);
  const reply = await waitForLine(/^PWR:(?!.*dry=1)/, 4000);
  if (reply) { pwrClearDirty(); logI((newbat ? 'Комплект заменён: ' : 'Батарея: ') + reply.slice('PWR:'.length)); }
  else logE('PWR: нет ответа');
}
let pwrDryAcc = {};   // a dry preview arrives as two lines too
function parsePwr(line) {
  const r = Proto.parsePwr(line);
  if (!r || r.error) return;           // "PWR: unknown key …" is a refusal, not a record
  const second = r.second, fresh = r.kv;
  // PWR: opens a record, PWRB: completes it; either way render what we have.
  let kv;
  if (fresh.dry) { kv = second ? Object.assign(pwrDryAcc, fresh) : (pwrDryAcc = fresh); }
  else           { kv = second ? Object.assign(pwrLast || {}, fresh) : fresh; }
  if (kv.dry) {
    // Предпросмотр несохранённого выбора: только строка оценки, селекты не трогаем.
    const pv = $('pwr-preview'); if (!pv) return;
    pv.innerHTML = pwrEstText(kv, 'С этими настройками') || '';
    pv.hidden = !pv.innerHTML;
    pwrDirtyHint();          /* нечего показать — но про несохранённое скажем */
    return;
  }
  pwrLast = kv; pwrNext = kv.next;
  pwrSupported = true;
  pwrSetEnabled(true);
  // Значения полей трогаем только если пользователь ничего не правит: иначе
  // ответ опроса затрёт несохранённый ввод прямо под руками.
  if (!pwrDirty) {
    pwrSetAll(kv);
    pwrFill('pwr-chem', PWR_CHEMS.map(c => [c[0], c[1]]), kv.chem);
    pwrFill('pwr-ser', PWR_CELLS, kv.ser); pwrFill('pwr-par', PWR_CELLS, kv.par);
    const ci = $('pwr-cell');
    if (ci && document.activeElement !== ci) ci.value = kv.cell > 0 ? kv.cell : '';
    pwrPackSummary();
  }
  pwrMarkKnown(true);
  pwrRender();
  pwrRenderEstimate(kv);
  pwrRenderBatteryCard(kv);
  renderRemaining();
  const pv = $('pwr-preview'); if (pv) { pv.hidden = true; pv.innerHTML = ''; }
  // Локальный отсчёт: раз в секунду вниз, на нуле — перечитать (ценник как
  // раз перерисовался, и у него уже новое next).
  clearInterval(pwrTickT);
  if (curTab !== 'sys') return;      // late reply: card is off-screen, no timers
  pwrTickT = setInterval(() => {
    if (!connected) { pwrStopTimers(); return; }
    if (pwrNext > 0) { pwrNext--; pwrRender(); }
    // -2: ждём ответа, не спамим. Во время OTA не шлём вовсе: любая NUS-запись
    // параллельно SMP-передаче — это «GATT operation failed» на телефоне.
    if (pwrNext === 0) { pwrNext = -2; if (pwrMayPoll()) send('PWR\n'); }
  }, 1000);
  if (!pwrPollT) pwrPollT = setInterval(() => {
    if (!connected) { pwrStopTimers(); return; }
    if (pwrMayPoll()) send('PWR\n');
  }, PWR_POLL_MS);
}
// Предпросмотр: любой сдвиг селекта → через 400 мс спросить прошивку «а если так?»
// (dry=1 — считает, но не сохраняет и не применяет).
function pwrPreview() {
  clearTimeout(pwrPreviewT);
  pwrPreviewT = setTimeout(() => {
    if (!pwrMayPoll() || pwrSupported === false) return;
    const v = id => +$(id).value;
    pwrPackSummary();
    send(`PWR:day=${v('pwr-day')},night=${v('pwr-night')},from=${v('pwr-from')},to=${v('pwr-to')},advc=${v('pwr-advc')},advp=${v('pwr-advp')},${pwrPackKeys()},dry=1\n`);
  }, 400);
}
// Запрос с одним повтором: TX-пул прошивки в 3 буфера может уронить ответ,
// а старая прошивка (< 3.4.13) не ответит вовсе — тогда честно так и пишем.
function pwrQuery() {
  if (!pwrMayPoll()) return;
  // SYSINFO уходит первым и к этому моменту уже разобран: версия известна,
  // и на старой прошивке мы не ждём таймаута, а сразу говорим, в чём дело.
  if (sysState.fwVersion && versionCompare(sysState.fwVersion, PWR_MIN_FW) < 0) {
    pwrSupported = false;
    pwrSetEnabled(false);
    pwrStatus(`не поддерживается: прошивка ${sysState.fwVersion}, нужна ${PWR_MIN_FW}+ — ценник работает по старым правилам`, 'var(--orange)');
    return;
  }
  pwrSupported = null;
  pwrSetEnabled(true);
  send('PWR\n');
  setTimeout(() => { if (pwrMayPoll() && !pwrKnown) send('PWR\n'); }, 2200);
  setTimeout(() => { if (connected && curTab === 'sys' && !pwrKnown) pwrStatus('нет ответа — прошивка старше 3.4.13?', 'var(--orange)'); }, 5500);
}
// Отключение: чужие цифры не наследуем, таймеры гасим.
function pwrReset() {
  pwrStopTimers();
  clearTimeout(pwrPreviewT);
  pwrDirty = false;
  pwrLast = null; pwrNext = -1;
  pwrSupported = null;
  pwrSetEnabled(true);
  pwrSetAll(PWR_DEFAULTS);
  pwrFill('pwr-chem', PWR_CHEMS.map(c => [c[0], c[1]]), 0);
  pwrFill('pwr-ser', PWR_CELLS, 1); pwrFill('pwr-par', PWR_CELLS, 1);
  const bi = $('pwr-cell'); if (bi) bi.value = '';
  const pk = $('pwr-pack'); if (pk) pk.textContent = '';
  pwrMarkKnown(false);
  pwrStatus('—');
  const est = $('pwr-est'); if (est) est.textContent = '—';
  const pv = $('pwr-preview'); if (pv) { pv.hidden = true; pv.innerHTML = ''; }
  const sb = $('sys-bat'); if (sb) { sb.textContent = '—'; sb.style.color = 'var(--fg2)'; }
  if (!pwrReset.wired) {
    pwrReset.wired = true;
    const touched = () => { pwrDirty = true; pwrDirtyHint(); };
    for (const id of PWR_IDS) { const el = $(id); if (el) el.addEventListener('change', () => { touched(); pwrPreview(); }); }
    if (bi) bi.addEventListener('input', () => { touched(); pwrPackSummary(); pwrPreview(); });
    for (const id of ['pwr-ser', 'pwr-par', 'pwr-chem']) {
      const el = $(id);
      if (el) el.addEventListener('change', () => { touched(); pwrPackSummary(); pwrPreview(); });
    }
  }
}
async function applyPwr() {
  if (!connected) { alert('Не подключено'); return; }
  if (pwrSupported === false) {
    alert(`Эта прошивка (${sysState.fwVersion}) шаблон сна не понимает — обновите ценник до ${PWR_MIN_FW}+`);
    return;
  }
  const v = id => +$(id).value;
  send(`PWR:day=${v('pwr-day')},night=${v('pwr-night')},from=${v('pwr-from')},to=${v('pwr-to')},advc=${v('pwr-advc')},advp=${v('pwr-advp')}\n`);
  const reply = await waitForLine(/^PWR:(?!.*dry=1)/, 4000);
  if (reply) { pwrClearDirty(); logI('Шаблон сна: ' + reply.slice('PWR:'.length)); }
  else logE('PWR: нет ответа (прошивка старше v3.4.13?)');
}
function resetPwr() { pwrSetAll(PWR_DEFAULTS); applyPwr(); }
// Списки живут в DOM выше этого скрипта, но заполняем их так же осторожно, как тост.
if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', pwrReset);
else pwrReset();

// ── Имя ценника (NAME) ────────────────────────────────────────────
// Прошивка отвечает «NAME:<полное имя в эфире>». Кастомное имя всегда
// рекламируется как «<пользовательское> (XXXXXX)» — хвост несъёмный.
function parseName(line) {
  const { full, user } = Proto.parseName(line);
  if (full) currentDeviceName = full;   // живое имя (в т.ч. после переименования)
  const el = $('sys-name');
  if (el) el.textContent = full || '—';
  // Подставляем в поле редактирования только пользовательскую часть.
  const inp = $('name-inp');
  if (inp && document.activeElement !== inp) inp.value = user;
  nfcRefresh();   // имя обновилось → пересобрать ссылку метки
}

async function applyName() {
  if (!connected) { alert('Не подключено'); return; }
  const v = $('name-inp').value.trim();
  if (!v) { resetName(); return; }
  send(`NAME ${v}\n`);
  const reply = await waitForLine(/^NAME:/, 4000);
  if (reply) logI('Имя обновлено: ' + reply.slice('NAME:'.length));
  else logE('Имя: нет ответа (включена защита SEC? разблокируй ценник)');
}

async function resetName() {
  if (!connected) { alert('Не подключено'); return; }
  send('NAME -\n');
  const reply = await waitForLine(/^NAME:/, 4000);
  if (reply) logI('Имя сброшено к стандартному: ' + reply.slice('NAME:'.length));
  else logE('Имя: нет ответа (включена защита SEC? разблокируй ценник)');
}

// STATS:uptime=<s> wall=<unix> boots=<n> fwupd=<n> refr=<n> refrfw=<n>
// STATS:flash=<none|valid|consumed> fl_uptime=<s> fl_wall=<unix>
// Persisted counters (Загрузок / Обновлений ПО / Обновл. экрана / …с прошивки):
// fed by both SYSINFO and STATS, whichever arrives.
const STAT_IDS = { boots: 'st-boots', fwupd: 'st-fwupd', refr: 'st-refr', refrfw: 'st-refrfw' };
function setStatCounters(v) {
  for (const [k, id] of Object.entries(STAT_IDS)) {
    if (v[k] != null) { const el = $(id); if (el) el.textContent = v[k]; }
  }
}

function parseStats(line) {
  const r = Proto.parseStats(line);
  if (!r) return;
  if (r.kind === 'live') {
    setStatCounters(r.counters);
    if (r.wall !== null) {
      sysState.wallUnix = r.wall;
      maybeAutoSetTime();   // часы устройства известны → можно проверить «не выставлены ли»
    }
  } else if (r.kind === 'flash') {
    const fl = r.flash;
    if (fl) {
      const el = $('st-flash');
      el.textContent = fl;
      el.style.color = fl === 'valid'    ? 'var(--green)'
                     : fl === 'consumed' ? 'var(--orange)'
                     :                     'var(--fg2)';
    }
  }
}

function parseSysinfo(line) {
  // SYSINFO:fw=3.0.1 build=2024-06-12_14:30:00 uptime=12345 bat=3850 mah=1.234 cur_ua=900 …
  // Every field is optional (older firmware reports fewer); protocol.js does the reading.
  const r = Proto.parseSysinfo(line);
  if (!r) return;
  sysState.picture = r.mode ? r.mode === 'pic' : null;
  dfuSilentFollowTag();

  // Firmware before 3.4.4 does not report the panel: it only ever drove 128x296.
  sysState.panel = r.panel || '128x296';
  setPanel(sysState.panel);

  if (r.fw) sysState.fwVersion = r.fw;
  if (r.build) sysState.buildDate = r.build;
  if (r.uptime !== null) sysState.uptimeSec = r.uptime;
  if (r.bat !== null) sysState.battMv = r.bat;
  if (r.mahX1000 !== null) sysState.mahUsed = r.mahX1000;
  if (r.curUa !== null) sysState.curUa = r.curUa;
  if (r.layout !== null) sysState.layout = r.layout;
  if (r.sec !== null) sysState.secOn = r.sec;
  if (r.authed !== null) sysState.authed = r.authed;
  sysState.lastUpdate = new Date();
  // Since the persisted-stats firmware SYSINFO repeats boots/fwupd/refr/refrfw;
  // STATS used to be their only source and rode at the tail of the connect burst.
  setStatCounters(r.counters);

  updateSysUI();
  updateAuthUI();
  otaOnSysinfo();         // back after the OTA reboot? decide done / failed first
  checkVersionMismatch();
  maybeAutoSetTime();   // build/uptime могли прийти позже STATS — пробуем и тут

  // Gate is on and we're not authed yet → kick off the handshake (once).
  if (sysState.secOn && !sysState.authed) maybeAutoAuth();
}

function updateSysUI() {
  const s = sysState;
  $('sys-fw-ver').textContent = s.fwVersion || '—';
  $('sys-build').textContent = s.buildDate ? s.buildDate.replace('_', ' ') : '—';

  // Uptime formatting
  if (s.uptimeSec > 0) {
    const d = Math.floor(s.uptimeSec / 86400);
    const h = Math.floor((s.uptimeSec % 86400) / 3600);
    const m = Math.floor((s.uptimeSec % 3600) / 60);
    const sec = s.uptimeSec % 60;
    $('sys-uptime').textContent = d > 0 ? `${d}d ${h}h ${m}m` : `${h}h ${m}m ${sec}s`;
  }

  // Battery
  if (s.battMv > 0) {
    const atCeiling = s.battMv >= 3590; // ADC saturated → actually higher
    const pct = atCeiling ? 100 : Math.max(0, Math.min(100, Math.round((s.battMv - BATT_V_EMPTY) / (BATT_V_FULL - BATT_V_EMPTY) * 100)));
    const color = pct > 50 ? 'var(--green)' : pct > 20 ? 'var(--orange)' : 'var(--red)';
    const label = atCeiling ? `≥${s.battMv} mV (полный)` : `${s.battMv} mV (${pct}%)`;
    $('sys-batt').innerHTML = `<span style="color:${color}">${label}</span>`;
    $('sys-batt-bar').style.width = pct + '%';
    $('sys-batt-pct').textContent = atCeiling ? '100%+' : pct + '%';
    battChip(pct, s.battMv, s.battLow);   // header chip: charge without leaving the tab
  }

  // Energy
  const mahUsedReal = s.mahUsed / 1000;
  $('sys-mah').textContent = mahUsedReal.toFixed(3) + ' mAh';
  $('sys-used').textContent = mahUsedReal.toFixed(2) + ' mAh';
  $('sys-cur').textContent = s.curUa > 0 ? (s.curUa / 1000).toFixed(2) + ' mA' : '—';

  // mWh (V_avg ≈ 3.7V — a Li-ion figure; the pack's nominal is used when declared)
  const vnom = (pwrLast && pwrLast.chem) ? (pwrChem(pwrLast.chem)[5] * (pwrLast.ser || 1)) / 1000 : 3.7;
  $('sys-mah').textContent = `${mahUsedReal.toFixed(3)} mAh (${(mahUsedReal * vnom).toFixed(1)} mWh)`;
  renderRemaining();

  if (s.lastUpdate) {
    $('sys-last-update').textContent = 'Обновлено ' + s.lastUpdate.toLocaleTimeString();
  }
}

// --- Battery alerts from firmware ---
function parseBattAlert(line) {
  const alert = Proto.parseBattAlert(line);
  if (!alert) return;
  const mv = alert.mv;
  if (alert.state === 'low') {
    sysState.battLow = true;
    if (mv) { sysState.battMv = mv; updateSysUI(); }   // repaint the header chip red
    logE(`⚠ BATTERY LOW: ${mv} mV — display updates inhibited`);
    dfuLog(`⚠ Battery low: ${mv} mV`, 'wrn');
  } else if (alert.state === 'shutdown') {
    sysState.battLow = true;
    if (mv) { sysState.battMv = mv; updateSysUI(); }
    logE(`🔴 BATTERY CRITICAL: ${mv} mV — device entering deep sleep!`);
    dfuLog(`🔴 Shutdown: ${mv} mV — deep sleep`, 'err');
  } else if (alert.state === 'ok') {
    sysState.battLow = false;
    if (mv) { sysState.battMv = mv; updateSysUI(); }
    logI(`✓ Battery recovered: ${mv} mV`);
  }
}

// --- Firmware update server ---
let serverManifest = null;  // {version, file, size, notes, sha256, variants:[...]}

// ═══════════════════════════════════════════════════════════════════
//  Basic device auth (shared-key challenge-response) + variant select
//  Mirrors src/app/secauth.c. Deliberately basic — it stops random peers /
//  garbage; the hard firmware-authenticity guarantee is the MCUboot key.
// ═══════════════════════════════════════════════════════════════════

// Must match the compiled default in src/app/secauth.c — the shared "base"
// fleet key common to all un-rotated devices. It lives in the sources on
// purpose; rotate per-device with a password (SETKEY) when you need isolation.
const SECAUTH_DEFAULT_KEY = new Uint8Array([
  0x9e,0x1c,0x47,0xb3,0x52,0xa8,0x0f,0xd6,
  0x71,0x2b,0xc4,0x88,0x3a,0xe5,0x96,0x10,
]);
let authKeyBytes = SECAUTH_DEFAULT_KEY;  // key for the next handshake
let authBusy = false;
let autoAuthTried = false;               // default-key auto-try is once-per-connection

// ── Deep-link из NFC-метки ──────────────────────────────────────────
// Метка несёт hash «#n=<полное имя (КОД)>&k=<пароль>». n → точный фильтр
// chooser (имя+уникальный код = один ценник); k → авто-разблокировка.
const deepLink = (() => {
  try {
    const h = (location.hash || '').replace(/^#/, '');
    if (!h) return null;
    const p = new URLSearchParams(h);
    const n = (p.get('n') || '').trim();
    const k = (p.get('k') || '').trim();
    return (n || k) ? { name: n, key: k } : null;
  } catch { return null; }
})();
let pendingDeepKey = '';     // пароль из метки для авто-AUTH (см. maybeAutoAuth)
let deepLinkExactName = '';  // имя из метки → точный фильтр chooser (см. bleConnect)
// Единственный источник истины для имени подключённого ценника. bleDevice.name
// «замораживается» в момент выбора и НЕ меняется при переименовании по GATT;
// поэтому ссылку метки строим отсюда: init из bleDevice.name, далее обновляем
// каждым ответом NAME: (parseName), сбрасываем при отключении (onBleDisc).
let currentDeviceName = '';

function applyDeepLink() {
  if (!deepLink) return;
  if (deepLink.name) {
    deepLinkExactName = deepLink.name;
    const sel = $('dev-name');
    if (sel) {
      const opt = document.createElement('option');
      opt.value = deepLink.name;
      opt.textContent = '🔗 ' + deepLink.name;
      sel.insertBefore(opt, sel.firstChild);
      sel.value = deepLink.name;
    }
  }
  if (deepLink.key) {
    pendingDeepKey = deepLink.key;
    const pf = $('dfu-pass');
    if (pf) pf.value = deepLink.key;
  }
  logI('🔗 Параметры из NFC-метки приняты — нажми «Подключить».');
  setStatus('🔗 Ценник из метки готов — нажми «Подключить»', false);
}

function hexToBytes(h) {
  h = (h || '').trim();
  const out = new Uint8Array(h.length >> 1);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(h.substr(i * 2, 2), 16);
  return out;
}
function bytesToHex(b) {
  return Array.from(b, x => x.toString(16).padStart(2, '0')).join('');
}

// AES-128-ECB of one 16-byte block via WebCrypto AES-CBC with IV=0 (the first
// ciphertext block equals ECB of the first plaintext block). Matches the
// device's bt_encrypt_be(key, nonce).
async function aesEcbBlock(keyBytes, block16) {
  const k = await crypto.subtle.importKey('raw', keyBytes, { name: 'AES-CBC' }, false, ['encrypt']);
  const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-CBC', iv: new Uint8Array(16) }, k, block16));
  return ct.slice(0, 16);
}

// A user "password" → 16-byte key. An exact 32-hex string is used verbatim;
// anything else becomes key = SHA-256(password)[:16]. dfuSetPassword sets the
// device key the same way, so a password round-trips.
async function deriveKeyFromInput(input) {
  const s = (input || '').trim();
  if (/^[0-9a-fA-F]{32}$/.test(s)) return hexToBytes(s);
  const dig = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(s)));
  return dig.slice(0, 16);
}

function doAuth() { if (connected) { authBusy = true; send('AUTH\n'); } }

async function maybeAutoAuth() {
  if (authBusy || sysState.authed || autoAuthTried) return;
  autoAuthTried = true;
  if (pendingDeepKey) {
    authKeyBytes = await deriveKeyFromInput(pendingDeepKey);
    logI('🔑 Ценник запаролен — пробую пароль из NFC-метки…');
  } else {
    authKeyBytes = SECAUTH_DEFAULT_KEY;
    logI('🔒 Ценник запаролен — пробую общий ключ по умолчанию…');
  }
  doAuth();
}

async function handleAuthLine(line) {
  const auth = Proto.parseAuth(line);
  if (!auth) return;
  if (auth.kind === 'chal') {
    const nonceHex = auth.nonce;
    try {
      const resp = await aesEcbBlock(authKeyBytes, hexToBytes(nonceHex));
      send('AUTH ' + bytesToHex(resp) + '\n');
    } catch (e) { logE('AUTH: ' + e.message); authBusy = false; }
    return;
  }
  if (auth.kind === 'ok') {
    authBusy = false; sysState.authed = true;
    logI('🔓 Авторизация пройдена');
    updateAuthUI();
    requestSysinfo();   // re-read: gated commands (BATT/STATS) now allowed
    return;
  }
  if (auth.kind === 'fail') {
    authBusy = false; sysState.authed = false;
    logI('🔒 Ключ не подошёл — введи пароль ценника и нажми «Разблокировать»');
    updateAuthUI();
    return;
  }
}

// Manual unlock: take the password field (or prompt), derive the key, retry.
async function dfuUnlock() {
  const el = $('dfu-pass');
  const pw = el ? el.value : prompt('Пароль ценника:');
  if (pw == null) return;
  authKeyBytes = await deriveKeyFromInput(pw);
  logI('🔑 Пробую введённый пароль…');
  doAuth();
}

// Rotate the device key to a new password (requires being authed first).
async function dfuSetPassword() {
  if (!sysState.authed) { alert('Сначала разблокируй ценник — нужно знать текущий ключ.'); return; }
  const el = $('dfu-pass');
  const pw = el ? el.value : prompt('Новый пароль:');
  if (!pw) { alert('Пустой пароль. Введи новый пароль в поле и повтори.'); return; }
  const k = await deriveKeyFromInput(pw);
  send('SETKEY ' + bytesToHex(k) + '\n');
  authKeyBytes = k;   // subsequent handshakes use the new key
  logI('🔑 Новый ключ отправлён (SETKEY). Сохрани пароль — он общий для этого ценника.');
}

function updateAuthUI() {
  const el = $('dfu-auth-status');
  if (!el) return;
  if (!sysState.secOn)        el.innerHTML = '<span style="color:var(--fg2)">🔓 без пароля (или старая прошивка)</span>';
  else if (sysState.authed)   el.innerHTML = '<span style="color:var(--green)">🔓 разблокировано</span>';
  else                        el.innerHTML = '<span style="color:var(--orange)">🔒 запаролено — введи пароль</span>';
}

// ═══════════════════════════════════════════════════════════════════
//  NFC-метка — записать deep-link на конкретный ценник (Web NFC, Android)
// ═══════════════════════════════════════════════════════════════════
function nfcStatus(msg, color) {
  const el = $('nfc-status');
  if (el) el.innerHTML = '<span style="color:var(--' + (color || 'fg2') + ')">' + msg + '</span>';
}

// Ссылка на это же приложение + полное имя ценника (+ опц. пароль) в hash.
// bleDevice.name — рекламируемое имя «Имя (КОД)», известно сразу после коннекта.
function nfcBuildUrl() {
  // currentDeviceName — единственный достоверный источник (обновляется при
  // переименовании и переключении ценников). bleDevice.name тут не годится: он
  // заморожен на момент выбора в chooser и после NAME <new> остаётся старым.
  let fullName = (currentDeviceName || '').trim();
  if (!fullName) { const sn = $('sys-name'); if (sn) fullName = (sn.textContent || '').trim(); }
  if (!fullName || fullName === '—') return null;
  const p = new URLSearchParams();
  p.set('n', fullName);
  if ($('nfc-include-pass') && $('nfc-include-pass').checked) {
    const np = $('nfc-pass'), dp = $('dfu-pass');
    const pw = ((np && np.value) || (dp && dp.value) || '').trim();
    if (pw) p.set('k', pw);
  }
  return location.origin + location.pathname + '#' + p.toString();
}

// Пересобрать предпросмотр ссылки + показать/скрыть поле пароля.
function nfcRefresh() {
  const inc = $('nfc-include-pass'), pf = $('nfc-pass'), u = $('nfc-url');
  if (pf) pf.style.display = (inc && inc.checked) ? '' : 'none';
  if (u) u.value = nfcBuildUrl() || '';
}

async function nfcWrite() {
  const url = nfcBuildUrl();
  if (!url) { nfcStatus('Сначала подключись к ценнику — нужно его имя.', 'orange'); return; }
  if (!('NDEFReader' in window)) {
    nfcStatus('Запись метки доступна только в Chrome на Android. Скопируй URL и запиши приложением (напр. NFC Tools).', 'orange');
    return;
  }
  try {
    nfcStatus('Поднеси телефон к метке…', 'fg2');
    const ndef = new NDEFReader();
    await ndef.write({ records: [{ recordType: 'url', data: url }] });
    nfcStatus('✅ Метка записана: ' + url, 'green');
  } catch (e) {
    nfcStatus('Ошибка записи: ' + e.message, 'red');
  }
}

async function nfcRead() {
  if (!('NDEFReader' in window)) { nfcStatus('Чтение метки — только Chrome на Android.', 'orange'); return; }
  try {
    nfcStatus('Поднеси телефон к метке для проверки…', 'fg2');
    const ndef = new NDEFReader();
    await ndef.scan();
    ndef.onreading = ev => {
      for (const r of ev.message.records) {
        if (r.recordType === 'url' || r.recordType === 'text') {
          nfcStatus('На метке: ' + new TextDecoder().decode(r.data), 'green');
          return;
        }
      }
      nfcStatus('На метке нет URL-записи.', 'orange');
    };
  } catch (e) {
    nfcStatus('Ошибка чтения: ' + e.message, 'red');
  }
}

async function nfcCopy() {
  const url = nfcBuildUrl();
  if (!url) { nfcStatus('Сначала подключись к ценнику.', 'orange'); return; }
  try { await navigator.clipboard.writeText(url); nfcStatus('URL скопирован в буфер.', 'green'); }
  catch { prompt('Скопируй URL:', url); }
}

// Pick the OTA image variant matching this device's layout. Falls back to the
// legacy single-image manifest for old devices / old manifests. Returns null
// when the device's layout has no image (so we never push a wrong-variant one).
function pickVariant(manifest, layout) { return Proto.pickVariant(manifest, layout); }
let dfuFirmwareData = null;
let dfuFileName = '';

// Manifest format expected at <server_url>/manifest.json:
// {
//   "version": "3.1.0",
//   "file": "app_update.bin",
//   "size": 225000,
//   "sha256": "abcdef...",
//   "notes": "Исправлен баг со сном",
//   "date": "2025-06-12"
// }

async function checkForUpdate() {
  const baseUrl = $('dfu-server-url').value.trim().replace(/\/+$/, '');
  if (!baseUrl) {
    $('dfu-server-status').innerHTML = '<span style="color:var(--red)">Укажи URL сервера</span>';
    return;
  }

  $('dfu-server-status').innerHTML = '<span style="color:var(--fg2)">Проверка…</span>';

  try {
    const resp = await fetch(baseUrl + '/manifest.json', { cache: 'no-store' });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    serverManifest = await resp.json();
    serverManifest._baseUrl = baseUrl;

    const sizeKb = serverManifest.size ? (serverManifest.size / 1024).toFixed(1) + ' KB' : '?';
    $('dfu-server-status').innerHTML = `<span style="color:var(--green)">✓ Сервер: v${serverManifest.version} · ${sizeKb}</span>` +
      (serverManifest.notes ? `<br><span style="color:var(--fg2)">${esc(serverManifest.notes)}</span>` : '');

    // Save URL for next time
    try { localStorage.setItem('dfu-server-url', baseUrl); } catch {}

    checkVersionMismatch();
  } catch (e) {
    $('dfu-server-status').innerHTML = `<span style="color:var(--red)">Ошибка: ${esc(e.message)}</span>`;
    serverManifest = null;
  }
}

// ═══════════════════════════════════════════════════════════════════
//  OTA state — one object drives the top toast (#fw-toast), the card in
//  the System tab (#dfu-banner) and the dot on the System tab:
//    idle → avail → download → upload → mark → reboot → done
//                                   ↘ error (retry / ✕) ↗
//  Everything that used to poke those elements directly goes through
//  otaSet()/otaRender(), so a SYSINFO arriving mid-update can no longer
//  repaint the toast as "update available" while the tag still runs the
//  old version — that is why it used to come back right after «Обновить».
// ═══════════════════════════════════════════════════════════════════
const ota = {
  phase: 'idle',
  ver: '', from: '', size: 0, notes: '',   // target version · what the tag runs now
  src: 'server',          // server (manifest) | file (hand-picked app_update.bin)
  label: '',              // image file name
  pct: 0, msg: '',        // progress · one-line detail under the title
  aborted: false,         // error came from the Stop button
  stalled: false,         // reboot: the tag did not come back / did not reboot
  muted: false,           // ✕ during a busy phase: keep working, hide the toast
  toastDismissedVer: null, bannerDismissedVer: null,
  hideT: null,
};
const OTA_BUSY = ['download', 'upload', 'mark', 'reboot'];
const OTA_ICON = { avail: '⬆', download: '⇣', upload: '↑', mark: '⟳', reboot: '⟳', done: '✓', error: '✗' };
function otaBusy() { return OTA_BUSY.includes(ota.phase); }

function otaSet(phase, patch = {}) {
  clearTimeout(ota.hideT);
  if (phase !== ota.phase) { ota.muted = false; ota.aborted = false; ota.stalled = false; ota.msg = ''; }
  const wasBusy = otaBusy();
  Object.assign(ota, patch, { phase });
  // Обновление кончилось на живой связи (ошибка / отмена): опрос профиля
  // был погашен на время передачи — вернуть его.
  if (wasBusy && !otaBusy() && connected) setTimeout(pwrQuery, 1500);
  if (phase === 'done') {
    ota.hideT = setTimeout(() => {
      if (ota.phase !== 'done') return;
      otaSet('idle');
      checkVersionMismatch();              // a yet-newer image may have appeared meanwhile
    }, 8000);
  }
  otaRender();
}
function otaProgress(pct, msg) {           // upload hot path: no phase change
  ota.pct = pct; ota.msg = msg;
  otaRender();
}

function otaRender() {
  const p = ota.phase;
  const kb = ota.size ? (ota.size / 1024).toFixed(1) + ' KB' : '';
  // System-tab card + nav dot = "there is something to install"
  const offer = (p === 'avail' || p === 'error') && ota.src === 'server' && !!ota.ver
                && ota.bannerDismissedVer !== ota.ver;
  const banner = $('dfu-banner');
  if (banner) {
    banner.style.display = offer ? '' : 'none';
    if (offer) {
      $('dfu-banner-ver').textContent = `v${ota.ver}`;
      $('dfu-banner-size').textContent = kb;
      $('dfu-banner-notes').textContent = ota.notes || '';
    }
  }
  navBadge('nav-sys', offer || otaBusy());

  const el = $('fw-toast');
  if (!el) return;                         // markup sits below this script — wireOtaToast() renders again
  let show = true, cls = '', go = '', goCls = 'acc', pct = null, pulse = false, sub = ota.msg;
  let title = ota.ver ? `Обновление → v${ota.ver}` : `Загрузка ${ota.label || 'прошивки'}`;
  switch (p) {
    case 'idle':
      show = false; break;
    case 'avail':
      show = ota.toastDismissedVer !== ota.ver;
      title = `Обновление ценника v${ota.ver}`;
      sub = [ota.notes, kb].filter(Boolean).join(' · ') || 'Нажми «Обновить»';
      go = 'Обновить'; break;
    case 'download':
      sub = sub || 'скачиваю образ с сервера…'; pct = 100; pulse = true; break;
    case 'upload':
      title += ` · ${ota.pct}%`; pct = ota.pct; go = 'Стоп'; goCls = 'bad'; break;
    case 'mark':
      pct = 100; pulse = true; break;
    case 'reboot':
      pct = 100; pulse = !ota.stalled; cls = ota.stalled ? 'warn' : '';
      if (ota.stalled) go = connected ? 'Перезагрузить' : 'Подключить';
      break;
    case 'done':
      cls = 'ok'; pct = 100;
      title = ota.ver ? `Ценник обновлён до v${ota.ver}` : `Прошивка загружена · v${sysState.fwVersion || '?'}`;
      break;
    case 'error':
      cls = 'err'; title = ota.aborted ? 'Загрузка остановлена' : 'Обновление не удалось';
      go = 'Повторить'; break;
  }
  if (ota.muted) show = false;
  const wasHidden = el.hidden;
  el.hidden = !show;
  if (!show) return;
  el.className = cls + (pulse ? ' busy' : '');
  $('fw-toast-ico').textContent = OTA_ICON[p] || '⬆';
  $('fw-toast-ver').textContent = title;
  $('fw-toast-sub').textContent = sub || '';
  const goBtn = $('fw-toast-go');
  goBtn.hidden = !go; goBtn.textContent = go; goBtn.className = `btn ${goCls} big`;
  $('fw-toast-track').hidden = pct == null;
  $('fw-toast-fill').style.width = (pct ?? 0) + '%';
  if (wasHidden) {                         // fresh appearance: undo a previous swipe
    el.style.transition = ''; el.style.transform = 'translateX(-50%)'; el.style.opacity = '1';
  }
}

// ✕ or a sideways swipe on the toast.
function otaDismiss() {
  switch (ota.phase) {
    case 'avail':  ota.toastDismissedVer = ota.ver; break;          // known — stop nagging with this version
    case 'error':  ota.toastDismissedVer = ota.ver;
                   ota.phase = ota.src === 'server' && ota.ver ? 'avail' : 'idle'; break;
    case 'done':   clearTimeout(ota.hideT); ota.phase = 'idle'; break;
    case 'reboot':                                                   // stop waiting; a manual connect still finishes the story
      if (!ota.stalled) { otaReconnectCancel(); if (!connected) setStatus('Не подключено', false); }
      ota.muted = true; break;
    default:       ota.muted = true;                                 // download/upload/mark: keep going, the result pops back up
  }
  otaRender();
}
function otaDismissBanner() {              // ✕ on the System-tab card
  ota.bannerDismissedVer = ota.ver;
  otaRender();
}
function otaGo() {                         // the toast's action button
  switch (ota.phase) {
    case 'avail':  startOtaUpdate(); break;
    case 'upload': dfuAbort(); break;
    case 'reboot': if (connected) { otaSet('reboot', { stalled: false, msg: 'перезагрузка ценника…' }); dfuReset(); }
                   else otaReconnect();
                   break;
    case 'error':  if (ota.src === 'server') startOtaUpdate(); else dfuStartUpload(); break;
  }
}
// Take the user to where the update is visible: System tab, DFU card in view.
function otaShowSys() {
  tab('sys', { quiet: true, keepScroll: true });   // no SYSINFO burst on top of the SMP traffic; one scroll, ours
  const card = $('dfu-card'), toast = $('fw-toast');
  if (!card) return;
  // Instant and synchronous, like tab()'s own scroll: a smooth scroll or a rAF
  // never runs in a backgrounded tab, and the jump is the whole point here.
  const under = isPhoneLayout() && toast && !toast.hidden ? toast.getBoundingClientRect().bottom + 12 : 10;
  card.style.scrollMarginTop = under + 'px';       // the fixed toast must not cover the card
  card.scrollIntoView({ behavior: 'auto', block: 'start' });
}

// First SYSINFO after the post-update reboot: did the new image take?
function otaOnSysinfo() {
  if (ota.phase !== 'reboot' || !sysState.fwVersion) return;
  otaReconnectCancel();
  if (!ota.ver || versionCompare(sysState.fwVersion, ota.ver) >= 0) {
    otaSet('done', { msg: ota.from && ota.from !== sysState.fwVersion ? `было v${ota.from}` : '' });
    dfuLog(`✓ Ценник вернулся на v${sysState.fwVersion}`, 'ok');
  } else {
    otaSet('error', { msg: `ценник всё ещё на v${sysState.fwVersion} — образ не применился, смотри лог DFU` });
    dfuLog(`✗ После перезагрузки прошивка v${sysState.fwVersion}, ожидалась v${ota.ver} — образ не применился`, 'err');
  }
}

function checkVersionMismatch() {
  if (!serverManifest || !sysState.fwVersion) return;
  if (otaBusy()) return;                   // progress owns the toast; re-evaluated when it ends
  if (ota.phase === 'error') return;       // keep the failure on screen until ✕ / Повторить
  const v = pickVariant(serverManifest, sysState.layout);
  if (v && v.version && versionCompare(v.version, sysState.fwVersion) > 0) {
    otaSet('avail', { ver: v.version, from: sysState.fwVersion, size: v.size || 0, notes: v.notes || '',
                      src: 'server', label: v.file || '', pct: 0, msg: '' });
  } else if (ota.phase === 'avail') {
    otaSet('idle');                        // installed (or the manifest went back): nothing to offer
  }
  // 'done' stays until its own timer clears it
}

// ── Reconnect after the OTA reboot ──────────────────────────────────
// Web Bluetooth lets us call gatt.connect() on a device the user already
// picked, so after REBOOT we give MCUboot time to swap the image and dial
// back in without a chooser. The first SYSINFO closes the loop in
// otaOnSysinfo(): same-or-newer version = done, older = failed.
let otaReconnGen = 0, otaReconnActive = false;
function otaReconnectCancel() { otaReconnGen++; otaReconnActive = false; }
async function otaReconnect() {
  const gen = ++otaReconnGen, dev = bleDevice;
  if (!dev) { otaSet('reboot', { stalled: true, msg: 'подключись к ценнику, чтобы проверить обновление' }); return; }
  // Три минуты: прошивка 3.4.23+ первые 90 с после старта рекламирует быстро,
  // но ценник на старой прошивке в режиме картинки с рекламой 10 с ловится
  // долго — 90 с ему не хватало.
  const t0 = Date.now(), budget = 180000, delay = 7000;
  otaReconnActive = true;
  otaSet('reboot', { stalled: false, msg: 'ценник перезагружается и меняет образ…' });
  setStatus('Ценник перезагружается…', false, 'busy');
  const tick = setInterval(() => {
    if (gen !== otaReconnGen || ota.phase !== 'reboot' || connected) { clearInterval(tick); return; }
    const sec = Math.round((Date.now() - t0) / 1000);
    if (sec * 1000 >= delay) otaSet('reboot', { msg: `жду ценник после перезагрузки… ${sec} с` });
  }, 1000);
  try {
    await sleep(delay);
    for (let n = 1; ; n++) {
      if (gen !== otaReconnGen || connected) return;
      try {
        await bleAttach(dev, { quiet: true });
        return;                            // parseSysinfo → otaOnSysinfo() decides done / failed
      } catch (e) {
        if (gen !== otaReconnGen) return;
        try { dev.gatt.disconnect(); } catch {}
        logI(`Переподключение после обновления (${n}): ${e.message}`);
        if (Date.now() - t0 >= budget) break;
        await sleep(2500);
      }
    }
    if (gen !== otaReconnGen || connected) return;
    setStatus('Не подключено', false);
    otaSet('reboot', { stalled: true, msg: 'ценник не вернулся за 3 мин — проверь его в сканере (nRF Connect); если не виден, передёрни питание и нажми «Подключить»' });
  } finally {
    clearInterval(tick);
    if (gen === otaReconnGen) otaReconnActive = false;
  }
}

function wireOtaToast() {
  const el = $('fw-toast');
  if (!el) return;   // #fw-toast объявлен ниже по документу — ждём DOMContentLoaded
  $('fw-toast-go').addEventListener('click', otaGo);
  $('fw-toast-x').addEventListener('click', otaDismiss);

  // Свайп-в-сторону для скрытия (pointer = mouse + touch).
  let startX = 0, dx = 0, dragging = false;
  const onDown = e => {
    if (e.target.closest('button')) return;   // клики по кнопкам не перехватываем
    dragging = true; startX = e.clientX; dx = 0;
    el.style.transition = '';
    el.setPointerCapture?.(e.pointerId);
  };
  const onMove = e => {
    if (!dragging) return;
    dx = e.clientX - startX;
    el.style.transform = `translateX(calc(-50% + ${dx}px))`;
    el.style.opacity = String(Math.max(0, 1 - Math.abs(dx) / 240));
  };
  const onUp = () => {
    if (!dragging) return;
    dragging = false;
    if (Math.abs(dx) > 90) {                   // достаточно далеко — смахнуть
      el.style.transition = 'transform .2s ease, opacity .2s ease';
      el.style.transform = `translateX(calc(-50% + ${dx > 0 ? 500 : -500}px))`;
      el.style.opacity = '0';
      setTimeout(otaDismiss, 200);
    } else {                                    // вернуть на место
      el.style.transition = 'transform .2s ease, opacity .2s ease';
      el.style.transform = 'translateX(-50%)';
      el.style.opacity = '1';
    }
  };
  el.addEventListener('pointerdown', onDown);
  el.addEventListener('pointermove', onMove);
  el.addEventListener('pointerup', onUp);
  el.addEventListener('pointercancel', onUp);
  otaRender();      // state set before the markup existed (early manifest check)
}
// #fw-toast в DOM появляется ниже этого скрипта → откладываем подключение.
if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', wireOtaToast);
else wireOtaToast();

function versionCompare(a, b) { return Proto.versionCompare(a, b); }

async function startOtaUpdate() {
  if (!serverManifest) { uiToast('Сначала проверь сервер обновлений (Система → Проверить)', 'err'); return; }
  if (dfuUploading) { otaShowSys(); return; }            // already running — just show it
  const v = pickVariant(serverManifest, sysState.layout);
  if (!v || !v.file) {
    const m = 'Для layout=' + (sysState.layout ?? '?') + ' нет образа в манифесте — обновление отменено';
    dfuLog(m, 'err'); uiToast(m, 'err');
    return;
  }
  otaSet('download', { ver: v.version, from: sysState.fwVersion || '', size: v.size || 0, notes: v.notes || '',
                       src: 'server', label: v.file, pct: 0, msg: '' });
  otaShowSys();
  // Download the image matching THIS device's layout.
  const url = serverManifest._baseUrl + '/' + v.file;
  dfuLog('Образ для layout=' + v.layout + ': ' + v.file, 'inf');
  dfuLog('Скачивание с сервера: ' + url, 'inf');
  $('dfu-prog').style.width = '0%';
  $('dfu-prog-text').textContent = 'Скачивание…';

  try {
    // no-store like the manifest above: /firmware/ carries no Cache-Control, so
    // the browser is free to reuse a heuristically-fresh copy of the previous
    // image. That stale copy uploads cleanly and hashes fine against itself,
    // leaving the tag on the old firmware with no error anywhere.
    const resp = await fetch(url, { cache: 'no-store' });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const blob = await resp.blob();
    const bytes = new Uint8Array(await blob.arrayBuffer());

    // Trust the manifest, not the download: size and SHA-256 must match the
    // entry we decided to install, or we are about to flash something else.
    if (v.size && bytes.length !== v.size) {
      throw new Error(`размер ${bytes.length} B ≠ ${v.size} B из манифеста — образ устарел или обрезан`);
    }
    if (v.sha256) {
      const dig = [...new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))]
        .map(b => b.toString(16).padStart(2, '0')).join('');
      if (dig !== String(v.sha256).toLowerCase()) {
        throw new Error(`SHA-256 ${dig.slice(0, 12)}… ≠ ${String(v.sha256).slice(0, 12)}… из манифеста — скачался не тот образ`);
      }
      dfuLog('✓ SHA-256 совпал с манифестом', 'ok');
    }

    dfuFirmwareData = bytes;
    dfuFileName = v.file;
    $('dfu-file-info').textContent = `${dfuFileName} — ${dfuFirmwareData.length} B (${(dfuFirmwareData.length/1024).toFixed(1)} KB) · с сервера`;
    dfuLog(`✓ Скачано: ${dfuFirmwareData.length} байт`, 'ok');
    $('dfu-btn-upload').disabled = !connected;

    // Auto-start upload if connected
    if (connected) {
      await sleep(200);
      dfuStartUpload();
    } else {
      $('dfu-prog-text').textContent = 'Образ скачан — подключись к ценнику';
      otaSet('error', { msg: 'образ скачан, но ценник не подключён — подключись и нажми «Загрузить в ценник»' });
    }
  } catch (e) {
    dfuLog('Ошибка скачивания: ' + e.message, 'err');
    $('dfu-prog-text').textContent = 'Ошибка';
    otaSet('error', { msg: 'скачивание: ' + e.message });
  }
}

// --- DFU via SMP (inline in NUS or standalone SMP) ---
// For simplicity and compatibility, DFU uses the standalone SMP BLE service.
// The user needs to connect via the DFU page for actual SMP transfer.
// Here we provide the UI for file selection + link to dedicated DFU page.

function dfuLoadFile(file) {
  if (!file) return;
  dfuFileName = file.name;
  const reader = new FileReader();
  reader.onload = (e) => {
    dfuFirmwareData = new Uint8Array(e.target.result);
    $('dfu-file-info').textContent = `${dfuFileName} — ${dfuFirmwareData.length} B (${(dfuFirmwareData.length/1024).toFixed(1)} KB)`;
    dfuLog(`Файл: ${dfuFileName}, ${dfuFirmwareData.length} B`, 'inf');
    $('dfu-btn-upload').disabled = !connected;
  };
  reader.readAsArrayBuffer(file);
}

// DFU drop zone
{
  const dz = $('dfu-drop');
  dz.addEventListener('dragover', e => { e.preventDefault(); dz.classList.add('over'); });
  dz.addEventListener('dragleave', () => dz.classList.remove('over'));
  dz.addEventListener('drop', e => { e.preventDefault(); dz.classList.remove('over'); dfuLoadFile(e.dataTransfer.files[0]); });
}

// --- SMP DFU over BLE (uses separate SMP service) ---
const SMP_SVC_UUID  = '8d53dc1d-1db7-4cd3-868b-8a527460aa84';
const SMP_CHAR_UUID = 'da2e7828-fbce-4e01-ae9e-261174997c48';

let smpChar = null;
let dfuUploading = false;
let dfuAbortFlag = false;
let smpSeq = 0;
let smpPendingResolve = null;
let smpPendingReject = null;     // onBleDisc fails the in-flight request instead of waiting out its timeout
let smpReassembly = null;
let smpExpectedLen = 0;

// CBOR codec for SMP (supports indefinite-length arrays/maps — zcbor default when ZCBOR_CANONICAL=n)
const CBOR={encode(o){const p=[];function e(v){if(v===null||v===undefined){p.push(0xf6);return}if(v===true){p.push(0xf5);return}if(v===false){p.push(0xf4);return}if(typeof v==='number'){if(Number.isInteger(v)&&v>=0){u(0,v);return}if(Number.isInteger(v)&&v<0){u(1,-1-v);return}const fb=new ArrayBuffer(8);new DataView(fb).setFloat64(0,v);p.push(0xfb,...new Uint8Array(fb));return}if(typeof v==='string'){const b=new TextEncoder().encode(v);u(3,b.length);p.push(...b);return}if(v instanceof Uint8Array||v instanceof ArrayBuffer){const b=v instanceof ArrayBuffer?new Uint8Array(v):v;u(2,b.length);p.push(...b);return}if(Array.isArray(v)){u(4,v.length);v.forEach(e);return}const k=Object.keys(v);u(5,k.length);k.forEach(x=>{e(x);e(v[x])})}function u(m,n){const M=m<<5;if(n<24)p.push(M|n);else if(n<256)p.push(M|24,n);else if(n<65536)p.push(M|25,n>>8,n&0xff);else if(n<0x100000000)p.push(M|26,(n>>24)&0xff,(n>>16)&0xff,(n>>8)&0xff,n&0xff);else{const h=Math.floor(n/0x100000000),l=n>>>0;p.push(M|27,(h>>24)&0xff,(h>>16)&0xff,(h>>8)&0xff,h&0xff,(l>>24)&0xff,(l>>16)&0xff,(l>>8)&0xff,l&0xff)}}e(o);return new Uint8Array(p)},decode(d){let pos=0;const dv=d instanceof DataView?d:new DataView(d.buffer||d,d.byteOffset||0,d.byteLength||d.length);function r(){const b=dv.getUint8(pos++),mj=b>>5,inf=b&0x1f;if(mj===4){if(inf===31){const a=[];while(dv.getUint8(pos)!==0xff)a.push(r());pos++;return a}const v=ru(inf);const a=[];for(let i=0;i<v;i++)a.push(r());return a}if(mj===5){if(inf===31){const o={};while(dv.getUint8(pos)!==0xff){const k=r();o[k]=r();}pos++;return o}const v=ru(inf);const o={};for(let i=0;i<v;i++){const k=r();o[k]=r();}return o}let v=ru(inf);switch(mj){case 0:return v;case 1:return-1-v;case 2:{const x=new Uint8Array(dv.buffer,dv.byteOffset+pos,v);pos+=v;return x}case 3:{const x=new TextDecoder().decode(new Uint8Array(dv.buffer,dv.byteOffset+pos,v));pos+=v;return x}case 7:if(inf===20)return false;if(inf===21)return true;if(inf===22)return null;if(inf===31)return undefined;if(inf===27){const f=dv.getFloat64(pos);pos+=8;return f}if(inf===26){const f=dv.getFloat32(pos);pos+=4;return f}return undefined}}function ru(inf){if(inf<24)return inf;if(inf===24)return dv.getUint8(pos++);if(inf===25){const v=dv.getUint16(pos);pos+=2;return v}if(inf===26){const v=dv.getUint32(pos);pos+=4;return v}if(inf===27){const h=dv.getUint32(pos);pos+=4;const l=dv.getUint32(pos);pos+=4;return h*0x100000000+l}return 0}return r()}};

async function dfuEnsureSmp() {
  if (smpChar) return true;
  if (!bleDevice || !bleDevice.gatt.connected) {
    dfuLog('Нужно подключение к устройству', 'err');
    return false;
  }
  // 2 попытки: discovery часто падает с "GATT operation failed", если в этот
  // момент идёт параллельная NUS-операция (SYSINFO/BATT) — Chrome не умеет
  // одновременные GATT-запросы.
  for (let attempt = 1; attempt <= 2; attempt++) {
    try {
      dfuLog('Подключение к SMP сервису…', 'inf');
      const server = bleDevice.gatt;
      const svc = await server.getPrimaryService(SMP_SVC_UUID);
      smpChar = await svc.getCharacteristic(SMP_CHAR_UUID);
      await smpChar.startNotifications();
      smpChar.addEventListener('characteristicvaluechanged', onSmpNotify);
      dfuLog('SMP сервис подключён', 'ok');
      return true;
    } catch (e) {
      smpChar = null;
      if (attempt < 2) { await sleep(800); continue; }
      dfuLog('SMP сервис недоступен: ' + e.message, 'err');
      dfuLog('Убедись, что прошивка собрана с CONFIG_NCS_SAMPLE_MCUMGR_BT_OTA_DFU=y', 'wrn');
    }
  }
  return false;
}

// Достаёт SHA256-хеш образа (TLV 0x10) из подписанного MCUboot-бинаря.
// Это тот же hash, который устройство отдаёт в image list — позволяет
// пометить slot 1 для test boot, даже когда list не работает (старые
// прошивки с NETBUF=256 отвечают rc=8 на двухслотовый список).
function mcubootImageHash(bin) {
  if (!bin || bin.length < 32) return null;
  const dv = new DataView(bin.buffer, bin.byteOffset, bin.byteLength);
  if (dv.getUint32(0, true) !== 0x96f3b83d) return null;   // IMAGE_MAGIC
  const hdrSize = dv.getUint16(8, true);
  const imgSize = dv.getUint32(12, true);
  let off = hdrSize + imgSize;
  if (off + 4 > bin.length) return null;
  if (dv.getUint16(off, true) === 0x6908) {                // protected TLVs
    off += dv.getUint16(off + 2, true);
    if (off + 4 > bin.length) return null;
  }
  if (dv.getUint16(off, true) !== 0x6907) return null;     // TLV_INFO_MAGIC
  const end = Math.min(off + dv.getUint16(off + 2, true), bin.length);
  off += 4;
  while (off + 4 <= end) {
    const type = dv.getUint16(off, true);
    const len = dv.getUint16(off + 2, true);
    if (type === 0x10 && len === 32) return bin.slice(off + 4, off + 36); // IMAGE_TLV_SHA256
    off += 4 + len;
  }
  return null;
}

let smpVerbose = false;  // enable via dfuToggleVerbose()

function onSmpNotify(ev) {
  const chunk = new Uint8Array(ev.target.value.buffer);
  if (!smpReassembly) {
    if (chunk.length < 8) return;
    const payloadLen = (chunk[2] << 8) | chunk[3];
    smpExpectedLen = 8 + payloadLen;
    smpReassembly = new Uint8Array(smpExpectedLen);
    const copyLen = Math.min(chunk.length, smpExpectedLen);
    smpReassembly.set(chunk.slice(0, copyLen));
    smpReassembly._written = copyLen;
  } else {
    const w = smpReassembly._written || 0;
    const remaining = smpExpectedLen - w;
    smpReassembly.set(chunk.slice(0, remaining), w);
    smpReassembly._written = w + Math.min(chunk.length, remaining);
  }
  if ((smpReassembly._written || 0) >= smpExpectedLen) {
    const payload = smpReassembly.slice(8, smpExpectedLen);
    smpReassembly = null;
    smpExpectedLen = 0;
    if (smpVerbose) {
      dfuLog('SMP raw (' + payload.length + 'B): ' +
        Array.from(payload).map(b=>b.toString(16).padStart(2,'0')).join(' '), 'inf');
    }
    try {
      const decoded = CBOR.decode(payload);
      if (smpPendingResolve) { smpPendingResolve(decoded); smpPendingResolve = null; }
    } catch (e) {
      dfuLog('CBOR decode error: ' + e.message + ' raw: ' +
        Array.from(payload.slice(0, 64)).map(b=>b.toString(16).padStart(2,'0')).join(' '), 'err');
      if (smpPendingResolve) { smpPendingResolve({ rc: -1 }); smpPendingResolve = null; }
    }
  }
}

function dfuToggleVerbose() {
  smpVerbose = !smpVerbose;
  dfuLog('SMP verbose logging: ' + (smpVerbose ? 'ВКЛ' : 'ВЫКЛ'), 'inf');
}

function smpRequest(op, group, cmdId, payload, timeoutMs = 30000) {
  return new Promise(async (resolve, reject) => {
    const cborData = CBOR.encode(payload);
    const hdr = new Uint8Array(8);
    const dv = new DataView(hdr.buffer);
    dv.setUint8(0, op); dv.setUint8(1, 0);
    dv.setUint16(2, cborData.length);
    dv.setUint16(4, group);
    dv.setUint8(6, smpSeq++ & 0xFF);
    dv.setUint8(7, cmdId);
    const msg = new Uint8Array(8 + cborData.length);
    msg.set(hdr); msg.set(cborData, 8);

    const timer = setTimeout(() => { smpPendingResolve = smpPendingReject = null; reject(new Error('SMP timeout')); }, timeoutMs);
    smpPendingResolve = (data) => { clearTimeout(timer); smpPendingReject = null; resolve(data); };
    smpPendingReject  = (err)  => { clearTimeout(timer); smpPendingResolve = smpPendingReject = null; reject(err); };
    smpReassembly = null; smpExpectedLen = 0;

    try {
      const chunkSz = 240;
      for (let i = 0; i < msg.length; i += chunkSz) {
        await smpChar.writeValueWithoutResponse(msg.slice(i, i + chunkSz));
      }
    } catch (e) { clearTimeout(timer); smpPendingResolve = smpPendingReject = null; reject(e); }
  });
}

// «Тихое обновление»: DFU:START/DONE с атрибутом SILENT — прошивка не рисует
// экраны обновления и, если была в режиме картинки, после ребута остаётся в нём.
// По умолчанию галочка повторяет режим ценника (SYSINFO mode=): стояла
// картинка — тихо, часы — обычно. Пока вы её не тронули в этой сессии, она
// следует за ценником; тронули — ваше слово до отключения. Старая прошивка
// режим не сообщает — тогда берётся то, что запомнил браузер.
let dfuSilentTouched = false;
function dfuSilent() { const el = $('dfu-silent'); return !!(el && el.checked); }
function dfuSilentSave() {
  dfuSilentTouched = true;
  try { localStorage.setItem('dfu-silent', dfuSilent() ? '1' : '0'); } catch {}
}
function dfuSilentFollowTag() {
  const el = $('dfu-silent'); if (!el || dfuSilentTouched) return;
  if (sysState.picture !== null) el.checked = sysState.picture;
  else { try { el.checked = localStorage.getItem('dfu-silent') === '1'; } catch {} }
}

async function dfuStartUpload() {
  if (!dfuFirmwareData || dfuUploading) return;
  // A hand-picked file («Загрузить в ценник») gets the same progress toast as a
  // server update; a server image that is re-sent by hand keeps its version.
  if (!otaBusy()) {
    const sameImage = ota.src === 'server' && ota.ver && dfuFileName === ota.label;
    otaSet('upload', sameImage ? { pct: 0, msg: '' }
      : { ver: '', from: sysState.fwVersion || '', src: 'file', label: dfuFileName,
          size: dfuFirmwareData.length, notes: '', pct: 0, msg: '' });
  }
  if (sysState.secOn && !sysState.authed) {
    dfuLog('Ценник запаролен — сначала разблокируй (введи пароль → 🔓 Разблокировать)', 'wrn');
    otaSet('error', { msg: 'ценник запаролен — сначала разблокируй его в «Системе»' });
    return;
  }
  if (!await dfuEnsureSmp()) {
    otaSet('error', { msg: connected ? 'SMP сервис недоступен — смотри лог DFU' : 'ценник не подключён' });
    return;
  }

  dfuUploading = true;
  dfuAbortFlag = false;
  // Тишина в эфире на время передачи: периодические опросы (PWR и его
  // отсчёт) гасим, иначе они влезают между SMP-записями и валят обновление.
  pwrStopTimers(); clearTimeout(pwrPreviewT);
  $('dfu-btn-upload').disabled = true;
  $('dfu-btn-abort').disabled = false;
  $('dfu-prog').style.width = '0%';
  dfuLog('Начало загрузки…', 'inf');
  otaSet('upload', { pct: 0, msg: 'стираю slot 1…' });

  const total = dfuFirmwareData.length;
  let offset = 0;
  const chunkSize = 128; // smaller chunks = less flash erase blocking per packet
  const t0 = performance.now();
  const sha = new Uint8Array(await crypto.subtle.digest('SHA-256', dfuFirmwareData));

  try {
    // Erase secondary slot before upload — clears leftover MCUboot trailer data
    // (sector 56 = 0x7D000–0x7E000 is NOT erased by progressive erase since the
    //  image only spans sectors 0–55; stale BOOT_MAGIC from prior attempts stays
    //  and confuses boot_set_next / MCUboot validation).
    dfuLog('Стираю slot 1 (очистка трейлера)…', 'inf');
    try {
      const erResp = await smpRequest(2, 1, 5, {}, 30000);
      if ((erResp.rc || 0) !== 0) {
        dfuLog(`Предупреждение: erase slot 1 вернул rc=${erResp.rc}`, 'wrn');
      } else {
        dfuLog('✓ Slot 1 очищен', 'ok');
      }
    } catch (e) {
      dfuLog('Erase slot 1 не прошёл: ' + e.message + ' — продолжаем без очистки', 'wrn');
    }

    // Notify firmware to show DFU screen
    if (connected && rxChar) {
      try { await sendBytes(new TextEncoder().encode(`DFU:START${dfuSilent() ? ' SILENT' : ''}\n`)); } catch {}
    }
    while (offset < total) {
      if (dfuAbortFlag) throw new Error('Отменено');
      const end = Math.min(offset + chunkSize, total);
      const chunk = dfuFirmwareData.slice(offset, end);
      const payload = { data: chunk, off: offset };
      if (offset === 0) { payload.len = total; payload.sha = sha; }

      const resp = await smpRequest(2, 1, 1, payload, 60000); // 60s timeout: flash erase can block
      if (resp.rc !== undefined && resp.rc !== 0) throw new Error(`rc=${resp.rc}`);
      offset = resp.off !== undefined ? resp.off : end;

      const pct = Math.round(offset / total * 100);
      const elapsed = (performance.now() - t0) / 1000;
      const speed = offset / elapsed / 1024;
      const eta = elapsed / offset * (total - offset);
      $('dfu-prog').style.width = pct + '%';
      $('dfu-prog-text').textContent = `${pct}% · ${(offset/1024).toFixed(1)}/${(total/1024).toFixed(1)} KB · ${speed.toFixed(1)} KB/s · ETA ${eta.toFixed(0)}s`;
      otaProgress(pct, `${Math.round(offset/1024)}/${Math.round(total/1024)} KB · ${speed.toFixed(1)} KB/s · ещё ${eta.toFixed(0)} с`);
    }
    const elapsed = ((performance.now() - t0) / 1000).toFixed(1);
    dfuLog(`✓ Загрузка завершена за ${elapsed}s`, 'ok');
    $('dfu-prog-text').textContent = `✓ Готово (${elapsed}s)`;
    otaSet('mark', { msg: `загружено за ${elapsed} с · проверяю образ…` });

    // Notify firmware: DFU complete
    if (connected && rxChar) {
      try { await sendBytes(new TextEncoder().encode(`DFU:DONE${dfuSilent() ? ' SILENT' : ''}\n`)); } catch {}
    }

    // Auto-test new image. Пауза: после DFU:DONE устройство несколько секунд
    // занято полной перерисовкой e-ink; dfuTest дополнительно ретраит.
    await sleep(1500);
    const marked = await dfuTest();

    // Авто-перезагрузка: иначе ценник «висит» на экране пост-обновления
    // (прогресс-бар + «Reboot to apply») и ждёт ручного Reboot. После
    // перезагрузки MCUboot свапнет образ, а новый main.c сам его confirm-нёт.
    if (marked) {
      dfuLog('Авто-перезагрузка для применения обновления через 2с…', 'inf');
      $('dfu-prog-text').textContent = '✓ Готово — перезагрузка…';
      otaSet('reboot', { msg: 'образ принят · перезагрузка через 2 с…' });
      await sleep(2000);
      otaSet('reboot', { msg: 'перезагрузка ценника…' });
      await dfuReset();   // REBOOT по NUS → BLE отвалится → onBleDisc() запустит otaReconnect()
      dfuLog('Ценник перезагружается — переподключусь сам, как только он вернётся.', 'ok');
      setTimeout(() => {  // the link never dropped: the tag ignored REBOOT
        if (ota.phase === 'reboot' && connected && !otaReconnActive)
          otaSet('reboot', { stalled: true, msg: 'ценник не перезагрузился — нажми «Перезагрузить»' });
      }, 10000);
    } else {
      dfuLog('Слот не помечен — авто-перезагрузка пропущена. Проверь лог и при необходимости нажми Test/Reboot вручную.', 'wrn');
      otaSet('error', { msg: 'образ не помечен для загрузки — смотри лог DFU (Test / Reboot вручную)' });
    }
  } catch (e) {
    dfuLog('✕ ' + e.message, 'err');
    $('dfu-prog-text').textContent = 'Ошибка: ' + e.message;
    otaSet('error', { msg: e.message, aborted: dfuAbortFlag });
  } finally {
    dfuUploading = false;
    $('dfu-btn-upload').disabled = false;
    $('dfu-btn-abort').disabled = true;
  }
}

function dfuAbort() {
  dfuAbortFlag = true;
  dfuLog('Отмена…', 'wrn');
}

async function dfuQueryState() {
  if (!await dfuEnsureSmp()) return;
  try {
    const resp = await smpRequest(0, 1, 0, {});
    if (resp.rc) {
      dfuLog(`Ошибка чтения слотов: rc=${resp.rc}` +
        (resp.rc === 8 ? ' — EMSGSIZE: ответ не влез в MCUMGR_TRANSPORT_NETBUF_SIZE (прошивка ≤3.0.3, буфер 256Б). Образ в slot 1 скорее всего ЕСТЬ — жми «Test».' : ''), 'err');
      return;
    }
    const images = resp.images || [];
    let html = '';
    images.forEach(img => {
      const flags = [];
      if (img.active) flags.push('<span style="color:var(--green)">ACTIVE</span>');
      if (img.confirmed) flags.push('<span style="color:var(--accent)">CONFIRMED</span>');
      if (img.pending) flags.push('<span style="color:var(--orange)">PENDING</span>');
      html += `<div style="margin:4px 0">Slot ${img.slot}: <span style="color:var(--yellow)">v${img.version}</span> ${flags.join(' ')}</div>`;
    });
    $('dfu-slots').innerHTML = html || '<span style="color:var(--fg2)">Нет данных</span>';
    dfuLog(`Слоты: ${images.map(i => `slot${i.slot}=v${i.version}`).join(', ')}`, 'inf');
  } catch (e) {
    dfuLog('Ошибка: ' + e.message, 'err');
  }
}

async function dfuTest() {
  if (!await dfuEnsureSmp()) return;
  try {
    // До 3 попыток: после DFU:DONE устройство ~8 c занято полной перерисовкой
    // e-ink и GATT-операции в это время могут падать.
    let state = null;
    for (let attempt = 1; attempt <= 3; attempt++) {
      try { state = await smpRequest(0, 1, 0, {}); break; }
      catch (e) {
        if (attempt === 3) throw e;
        dfuLog(`Устройство занято (${e.message}), повтор через 3с…`, 'wrn');
        await sleep(3000);
      }
    }
    if (state.rc) {
      dfuLog(`Чтение слотов: rc=${state.rc}` +
        (state.rc === 8 ? ' (EMSGSIZE — старая прошивка с NETBUF=256, список из 2 слотов не влезает в ответ)' : ''), 'wrn');
    }
    const images = state.images || [];
    let hash = null, verNote = '';
    if (images.length > 1) {
      hash = images[1].hash;
      verNote = ` (v${images[1].version})`;
    } else if (dfuFirmwareData) {
      // Список слотов недоступен/пуст — берём ожидаемый hash прямо из
      // скачанного app_update.bin (TLV SHA256). Если образ в slot 1 есть,
      // устройство найдёт его по hash; если нет — вернёт ошибку.
      hash = mcubootImageHash(dfuFirmwareData);
      if (hash) dfuLog('Список слотов пуст — использую hash из app_update.bin', 'inf');
    }
    if (!hash) {
      dfuLog('Нет образа в slot 1. Сначала скачай и загрузи прошивку.', 'wrn');
      return false;
    }
    const resp = await smpRequest(2, 1, 0, { hash, confirm: true });
    const rc = resp.rc || 0;
    if (rc === 0) {
      dfuLog(`✓ Slot 1${verNote} помечен для постоянного обновления — жми Reboot`, 'ok');
      return true;
    } else if (rc === 8) {
      // EMSGSIZE: устройство пометило слот ДО кодирования ответа (см.
      // img_mgmt_state_write), не влез только сам ответ. Помечено успешно.
      dfuLog('✓ Slot 1 помечен для постоянного обновления (ответ не влез в буфер — норма для прошивки ≤3.0.3). Жми Reboot.', 'ok');
      return true;
    } else if (rc === 5 || rc === 3) {
      dfuLog(`Ошибка test: rc=${rc} — образ с таким hash не найден в slot 1. Загрузи прошивку заново.`, 'err');
    } else if (rc !== 0) {
      dfuLog(`Ошибка test: rc=${rc}`, 'err');
    }
  } catch (e) { dfuLog('Ошибка: ' + e.message, 'err'); }
  return false;
}

// Verify: диагностика — image list + hash comparison + raw CBOR dump
async function dfuVerify() {
  if (!await dfuEnsureSmp()) return;
  dfuLog('─── Verify: диагностика ───', 'inf');

  // Temporarily enable verbose to capture raw bytes for this request
  const wasVerbose = smpVerbose;
  smpVerbose = true;
  try {
    const resp = await smpRequest(0, 1, 0, {});
    smpVerbose = wasVerbose;

    const rc = resp.rc ?? 0;
    const images = resp.images || [];
    dfuLog(`Image list: rc=${rc}, images=${images.length}`, rc === 0 && images.length > 0 ? 'ok' : 'wrn');

    if (rc === 8) {
      dfuLog('rc=8 = EMSGSIZE — ответ не влез в NETBUF. ' +
        'Прошивка собрана с NETBUF=256 (нужно 1024). ' +
        'Пересобери с CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE=1024 и перепрошей по проводу.', 'err');
    } else if (rc !== 0) {
      dfuLog(`img_mgmt ошибка rc=${rc}`, 'err');
    }

    images.forEach((img, i) => {
      const hashHex = img.hash instanceof Uint8Array
        ? Array.from(img.hash).map(b=>b.toString(16).padStart(2,'0')).join('')
        : '(no hash)';
      const flags = ['slot'+img.slot, 'v'+(img.version||'?')];
      if (img.active)    flags.push('ACTIVE');
      if (img.confirmed) flags.push('CONFIRMED');
      if (img.pending)   flags.push('PENDING');
      dfuLog(`  [${i}] ${flags.join(' ')} hash:${hashHex}`, 'inf');
    });

    if (images.length === 0 && rc === 0) {
      dfuLog('⚠ images[] пуст при rc=0 — img_mgmt_read_info упал для ВСЕХ слотов. ' +
        'Возможные причины: (1) неверный FLASH_AREA_ID в pm_config.h, ' +
        '(2) IMAGE_MAGIC не найден по 0xC000, (3) flash_area_open вернул ошибку. ' +
        'Посмотри raw CBOR выше и пересобери прошивку по проводу.', 'err');
    }

    // Compare slot-1 hash with locally downloaded binary
    if (dfuFirmwareData) {
      const localHash = mcubootImageHash(dfuFirmwareData);
      const localHashHex = localHash
        ? Array.from(localHash).map(b=>b.toString(16).padStart(2,'0')).join('') : null;
      if (localHashHex) {
        dfuLog('Hash из app_update.bin: ' + localHashHex, 'inf');
        if (images.length > 1 && images[1].hash instanceof Uint8Array) {
          const devHashHex = Array.from(images[1].hash).map(b=>b.toString(16).padStart(2,'0')).join('');
          if (devHashHex === localHashHex) {
            dfuLog('✓ Hash slot 1 = app_update.bin — образ записан корректно', 'ok');
          } else {
            dfuLog('✗ Hash НЕ совпадает! Образ в slot 1 повреждён или это другая версия.', 'err');
            dfuLog('  device: ' + devHashHex, 'err');
            dfuLog('  local:  ' + localHashHex, 'err');
          }
        } else if (images.length <= 1) {
          dfuLog('Slot 1 не виден — загрузи прошивку заново (или нажми Erase + Upload)', 'wrn');
        }
      } else {
        dfuLog('⚠ Не удалось извлечь hash из app_update.bin — проверь формат файла (нужен подписанный MCUboot binary)', 'wrn');
      }
    } else {
      dfuLog('app_update.bin не загружен — скачай прошивку для сравнения hash', 'wrn');
    }
  } catch (e) {
    smpVerbose = wasVerbose;
    dfuLog('Ошибка: ' + e.message, 'err');
  }
  dfuLog('─── Verify завершён ───', 'inf');
}

async function dfuErase() {
  if (!await dfuEnsureSmp()) return;
  dfuLog('Стираю slot 1…', 'inf');
  try {
    const resp = await smpRequest(2, 1, 5, {}, 30000);
    const rc = resp.rc || 0;
    if (rc === 0) dfuLog('✓ Slot 1 стёрт', 'ok');
    else dfuLog(`Erase вернул rc=${rc}`, 'err');
  } catch (e) { dfuLog('Ошибка: ' + e.message, 'err'); }
}

async function dfuConfirm() {
  if (!await dfuEnsureSmp()) return;
  try {
    const resp = await smpRequest(2, 1, 0, { confirm: true });
    const rc = resp.rc || 0;
    if (rc === 0) dfuLog('✓ Образ подтверждён', 'ok');
    else if (rc === 8) dfuLog('✓ Образ подтверждён (ответ не влез в буфер — норма для прошивки ≤3.0.3)', 'ok');
    else dfuLog(`Ошибка: rc=${rc}`, 'err');
  } catch (e) { dfuLog('Ошибка: ' + e.message, 'err'); }
}

async function dfuReset() {
  // SMP OS-reset (group 0) отключён в прошивке (CONFIG_MCUMGR_GRP_OS=n),
  // поэтому перезагружаем через NUS-команду REBOOT.
  if (connected && rxChar) {
    try {
      await sendBytes(new TextEncoder().encode('REBOOT\n'));
      dfuLog('⟳ REBOOT отправлен', 'ok');
      return;
    } catch (e) {
      dfuLog('REBOOT через NUS не прошёл: ' + e.message, 'wrn');
    }
  }
  if (!await dfuEnsureSmp()) return;
  try {
    await smpRequest(2, 0, 5, {}, 3000);
    dfuLog('⟳ Reset отправлен (SMP)', 'ok');
  } catch (e) {
    dfuLog('Reset не отправлен: ' + e.message, 'err');
  }
}

function dfuLog(msg, cls = 'inf') {
  const el = $('dfu-log');
  if (!el) return;
  const ts = new Date().toLocaleTimeString();
  el.innerHTML += `<div class="${cls}"><span class="ts">${ts}</span> ${msg}</div>`;
  el.scrollTop = el.scrollHeight;
}

// Auto-request sysinfo on connect
const _origSetStatus = typeof setStatus !== 'undefined' ? setStatus : null;
// We'll hook into the connection callback instead — look for connected=true
(function hookConnect() {
  const origOnNotify = onNotify;
  // Already hooked via parseSysinfo in the line parser
})();

// On connect, auto-request sysinfo after a short delay
function onSysConnect() {
  setTimeout(() => {
    if (connected) {
      requestSysinfo();
      // Also try SMP for slot info
      dfuEnsureSmp().then(ok => { if (ok) dfuQueryState(); });
    }
  }, 1500);
}

// Load saved server URL
try {
  try { const el = $('dfu-silent'); if (el) el.checked = localStorage.getItem('dfu-silent') === '1'; } catch {}
  const savedUrl = localStorage.getItem('dfu-server-url');
  if (savedUrl) {
    $('dfu-server-url').value = savedUrl;
    // Auto-check on load
    setTimeout(checkForUpdate, 3000);
  }
} catch {}
