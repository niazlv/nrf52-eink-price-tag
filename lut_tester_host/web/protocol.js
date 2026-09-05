/* protocol.js — the tag's reply lines, decoded.
 *
 * Pure functions only: no DOM, no app state, no globals but `Proto`. That is
 * what lets the same file run under node (tests/web/protocol.test.js) and in
 * the page, so the one place the firmware's wire format is interpreted is
 * the one place that is tested. Loaded as a classic script before app.js.
 * Line formats: docs/protocol.md. */
const Proto = (() => {
  const grab = (str, re) => { const m = str.match(re); return m ? m[1] : null; };
  const num  = s => (s == null ? null : +s);
  const flag = s => (s == null ? null : s === '1');

  // SYSINFO:fw=3.4.29 build=2026-09-05_12:34:56 uptime=12345 bat=3612 mah=1.234
  //   cur_ua=900 boots=12 fwupd=3 refr=400 refrfw=20 layout=1 serial= sec=0
  //   authed=0 panel=128x296 phy=2 mode=clock
  // Every field is optional: older firmware reports fewer of them.
  function parseSysinfo(line) {
    if (!line.startsWith('SYSINFO:')) return null;
    const d = line.slice('SYSINFO:'.length);
    const panelM = d.match(/panel=(\d+)x(\d+)/);
    const mahM   = d.match(/mah=(\d+)\.(\d+)/);
    return {
      fw:       grab(d, /fw=([\d.]+)/),
      build:    grab(d, /build=([\d\-_:]+)/),
      uptime:   num(grab(d, /uptime=(\d+)/)),
      bat:      num(grab(d, /bat=(\d+)/)),
      mahX1000: mahM ? +mahM[1] * 1000 + +mahM[2] : null,
      curUa:    num(grab(d, /cur_ua=(\d+)/)),
      layout:   num(grab(d, /layout=(\d+)/)),
      sec:      flag(grab(d, /\bsec=(\d+)/)),
      authed:   flag(grab(d, /authed=(\d+)/)),
      panel:    panelM ? `${panelM[1]}x${panelM[2]}` : null,
      mode:     grab(d, /\bmode=(pic|clock)/),
      counters: counters(d),
    };
  }

  function counters(d) {
    return { boots:  num(grab(d, /boots=(\d+)/)),  fwupd:  num(grab(d, /fwupd=(\d+)/)),
             refr:   num(grab(d, /\brefr=(\d+)/)), refrfw: num(grab(d, /refrfw=(\d+)/)) };
  }

  // STATS:uptime=… wall=… boots=… fwupd=… refr=… refrfw=… mah=…   (live)
  // STATS:flash=valid|consumed|none fl_uptime=… fl_wall=…          (flash record)
  function parseStats(line) {
    if (!line.startsWith('STATS:')) return null;
    const d = line.slice('STATS:'.length);
    if (/boots=/.test(d)) {
      return { kind: 'live', counters: counters(d),
               wall: num(grab(d, /\bwall=(\d+)/)), uptime: num(grab(d, /uptime=(\d+)/)) };
    }
    if (/flash=/.test(d)) return { kind: 'flash', flash: grab(d, /flash=(\w+)/) };
    return null;
  }

  // PWR:day=1 night=5 … days=-1 [dry=1]  opens a record, PWRB:chem=… closes it.
  // "PWR: unknown key x" / "PWR: out of range …" are errors (note the space).
  function parsePwr(line) {
    const second = line.startsWith('PWRB:');
    if (!second && !line.startsWith('PWR:')) return null;
    const rest = line.slice(second ? 'PWRB:'.length : 'PWR:'.length);
    const kv = {};
    for (const m of rest.matchAll(/(\w+)=(-?\d+)/g)) kv[m[1]] = +m[2];
    return { second, kv, error: rest.startsWith(' ') };
  }

  // NAME:<user name> (A1B2C3)   or   NAME:nrf52-E-ink-clock-A1B2C3 (default)
  function parseName(line) {
    if (!line.startsWith('NAME:')) return null;
    const full = line.slice('NAME:'.length).trim();
    const m = full.match(/^(.*) \(([0-9A-Fa-f]{6})\)$/);
    return { full, user: m ? m[1] : '', id: m ? m[2].toUpperCase() : null };
  }

  // meshrx=on   /   meshrx=off (re-enable over NUS only)
  function parseMeshrx(line) {
    if (!line.startsWith('meshrx=')) return null;
    return { on: line.slice('meshrx='.length).trim().startsWith('on') };
  }

  // BATT:LOW mv=3200 / BATT:SHUTDOWN mv=3050 / BATT:OK mv=3400
  function parseBattAlert(line) {
    const state = line.startsWith('BATT:LOW')      ? 'low'
                : line.startsWith('BATT:SHUTDOWN') ? 'shutdown'
                : line.startsWith('BATT:OK')       ? 'ok' : null;
    if (!state) return null;
    return { state, mv: num(grab(line, /mv=(\d+)/)) || 0 };
  }

  // LUT:N:XXXXXXXXXXXXXXXXXXXX — one of seven 10-byte rows of the 70-byte table
  function parseLgetLine(line) {
    if (!line.startsWith('LUT:') || line.length < 26 || line[5] !== ':') return null;
    const idx = parseInt(line[4]);
    if (isNaN(idx) || idx < 0 || idx >= 7) return null;
    return { idx, hex: line.slice(6, 26).trim() };
  }

  // TELE:full time=8707ms lut=BALANCED / TELE:fast time=112ms lut=TURBO /
  // TELE:fapply time=700ms bw=4736 rw=0 lut=… — refresh timings, by kind.
  function parseTele(line) {
    const ms = key => num(grab(line, new RegExp(`${key}=(\\d+)ms?`)));
    const r = { fapply: null, fast: null, full: null, lut: grab(line, /lut=(\S+)/) };
    if (line.includes('fapply') || line.includes('FAPPLY')) r.fapply = ms('time');
    if (line.includes(':fast')) r.fast = ms('time');
    if (line.includes(':full') || line.includes('UPDATE')) r.full = ms('time');
    return r;
  }

  // FW:rx 4096/15000 — frame-buffer upload progress, one line per chunk
  function parseFwRx(line) {
    const m = line.match(/^(FW|RW):rx (\d+)\/(\d+)/);
    return m ? { plane: m[1], got: +m[2], total: +m[3] } : null;
  }

  // TELE:vs f=3 ms=110 dec=1 crc=1a wc=ok rs=0 w=1 s=1 r=0 — one vstream frame ACK
  function parseVsAck(line) {
    return { ms: num(grab(line, /ms=(\d+)/)) || 0,
             wc: grab(line, /wc=(\w+)/),
             rs: num(grab(line, /rs=(\d+)/)) || 0 };
  }

  // VSTREAM:ready lut=… type=… crc=opt(+0x40) … — what the stream may use
  function parseVsReady(line) {
    return { crc: line.includes('crc=opt') };
  }

  // AUTH:CHAL <32 hex> / AUTH:OK / AUTH:FAIL / AUTH:ERR
  function parseAuth(line) {
    if (line.startsWith('AUTH:CHAL ')) return { kind: 'chal', nonce: line.slice('AUTH:CHAL '.length).trim() };
    if (line.startsWith('AUTH:OK')) return { kind: 'ok' };
    if (line.startsWith('AUTH:FAIL') || line.startsWith('AUTH:ERR')) return { kind: 'fail' };
    return null;
  }

  // "3.4.10" vs "3.4.9": numeric per component, missing components are 0
  function versionCompare(a, b) {
    const pa = a.split('.').map(Number);
    const pb = b.split('.').map(Number);
    for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
      const na = pa[i] || 0, nb = pb[i] || 0;
      if (na > nb) return 1;
      if (na < nb) return -1;
    }
    return 0;
  }

  // "2025-06-12_14:30:00" → unix seconds of those components taken as UTC.
  // The tag keeps naive time (mktime/gmtime without a zone), so this is the
  // scale its wall= values live in.
  function buildDateToUnix(s) {
    const m = s && s.match(/^(\d+)-(\d+)-(\d+)_(\d+):(\d+):(\d+)$/);
    if (!m) return null;
    return Math.floor(Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]) / 1000);
  }

  // The OTA image for a tag: manifest.variants[] keyed by layout id, with the
  // pre-variant single-image manifest still accepted for legacy/unknown tags.
  function pickVariant(manifest, layout) {
    if (!manifest) return null;
    if (Array.isArray(manifest.variants) && manifest.variants.length) {
      if (layout != null) return manifest.variants.find(v => v.layout === layout) || null;
      return manifest.variants.find(v => v.layout === 1) || null;  // old fw → legacy
    }
    // Legacy single-image manifest: valid only for legacy/unknown devices.
    if (layout == null || layout === 1) {
      return { layout: 1, file: manifest.file, version: manifest.version,
               size: manifest.size, sha256: manifest.sha256,
               date: manifest.date, notes: manifest.notes };
    }
    return null;
  }

  return { parseSysinfo, parseStats, parsePwr, parseName, parseMeshrx, parseBattAlert,
           parseLgetLine, parseTele, parseFwRx, parseVsAck, parseVsReady, parseAuth,
           versionCompare, buildDateToUnix, pickVariant };
})();

if (typeof module !== 'undefined' && module.exports) module.exports = Proto;
