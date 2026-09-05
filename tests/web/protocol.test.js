// node --test tests/web   — parsers for the tag's reply lines (lut_tester_host/web/protocol.js).
// Sample lines are the firmware's own format strings (src/app/commands.c) with values filled in.
const test = require('node:test');
const assert = require('node:assert/strict');
const Proto = require('../../lut_tester_host/web/protocol.js');

test('SYSINFO: every field, current firmware', () => {
  const r = Proto.parseSysinfo('SYSINFO:fw=3.4.29 build=2026-09-05_12:34:56 uptime=12345 bat=3612 mah=1.234 cur_ua=900'
    + ' boots=12 fwupd=3 refr=400 refrfw=20 layout=2 serial=T0042 sec=1 authed=0 panel=400x300 phy=2 mode=pic');
  assert.deepEqual(r, {
    fw: '3.4.29', build: '2026-09-05_12:34:56', uptime: 12345, bat: 3612, mahX1000: 1234, curUa: 900,
    layout: 2, sec: true, authed: false, panel: '400x300', mode: 'pic',
    counters: { boots: 12, fwupd: 3, refr: 400, refrfw: 20 },
  });
});

test('SYSINFO: old firmware reports less, missing fields are null not wrong', () => {
  const r = Proto.parseSysinfo('SYSINFO:fw=3.0.1 uptime=5 bat=3300 mah=0.005 cur_ua=0');
  assert.equal(r.fw, '3.0.1');
  assert.equal(r.build, null);
  assert.equal(r.mahX1000, 5);
  assert.equal(r.layout, null);
  assert.equal(r.sec, null);          // "unknown", so the UI does not assume a gate
  assert.equal(r.panel, null);        // the caller defaults to 128x296
  assert.equal(r.mode, null);
  assert.deepEqual(r.counters, { boots: null, fwupd: null, refr: null, refrfw: null });
  assert.equal(Proto.parseSysinfo('STATS:boots=1'), null);
});

test('SYSINFO: refr= must not match refrfw=, sec= must not match a word ending in sec', () => {
  const r = Proto.parseSysinfo('SYSINFO:fw=1.0.0 refrfw=7 refr=9 msec=1 sec=1');
  assert.equal(r.counters.refr, 9);
  assert.equal(r.counters.refrfw, 7);
  assert.equal(r.sec, true);
});

test('STATS: live line and flash line', () => {
  assert.deepEqual(Proto.parseStats('STATS:uptime=99 wall=1757000000 boots=4 fwupd=1 refr=10 refrfw=2 mah=0.100'),
    { kind: 'live', counters: { boots: 4, fwupd: 1, refr: 10, refrfw: 2 }, wall: 1757000000, uptime: 99 });
  assert.deepEqual(Proto.parseStats('STATS:flash=consumed fl_uptime=1 fl_wall=2'), { kind: 'flash', flash: 'consumed' });
  assert.equal(Proto.parseStats('STATS:garbage'), null);
});

test('PWR / PWRB records, dry previews and error replies', () => {
  const a = Proto.parsePwr('PWR:day=1 night=5 from=23 to=7 advc=2 advp=5 night_now=0 next=37 est=210 base=450 days=-1');
  assert.equal(a.second, false);
  assert.equal(a.error, false);
  assert.equal(a.kv.days, -1);
  assert.equal(a.kv.next, 37);
  const b = Proto.parsePwr('PWRB:chem=1 ser=1 par=1 cell=370 mv=3600 vsoc=25 vsat=1 used=12 soc=25 dry=1');
  assert.equal(b.second, true);
  assert.equal(b.kv.dry, 1);
  assert.equal(b.kv.vsat, 1);
  const e = Proto.parsePwr('PWR: out of range (day/night 0-60, from/to 0-23, adv 1-10)');
  assert.equal(e.error, true);
  assert.deepEqual(e.kv, {});
  assert.equal(Proto.parsePwr('PWR: unknown key foo').error, true);
  assert.equal(Proto.parsePwr('PWRX:1'), null);
});

test('NAME: user name with the permanent id, and the default name without it', () => {
  assert.deepEqual(Proto.parseName('NAME:Кухня (A1b2C3)'), { full: 'Кухня (A1b2C3)', user: 'Кухня', id: 'A1B2C3' });
  assert.deepEqual(Proto.parseName('NAME:nrf52-E-ink-clock-A1B2C3'),
    { full: 'nrf52-E-ink-clock-A1B2C3', user: '', id: null });
  assert.deepEqual(Proto.parseName('NAME:'), { full: '', user: '', id: null });
});

test('meshrx= on/off', () => {
  assert.deepEqual(Proto.parseMeshrx('meshrx=on'), { on: true });
  assert.deepEqual(Proto.parseMeshrx('meshrx=off (re-enable over NUS only)'), { on: false });
  assert.equal(Proto.parseMeshrx('MESHRX'), null);
});

test('BATT alerts', () => {
  assert.deepEqual(Proto.parseBattAlert('BATT:LOW mv=3200'), { state: 'low', mv: 3200 });
  assert.deepEqual(Proto.parseBattAlert('BATT:SHUTDOWN mv=3050'), { state: 'shutdown', mv: 3050 });
  assert.deepEqual(Proto.parseBattAlert('BATT:OK mv=3400'), { state: 'ok', mv: 3400 });
  assert.equal(Proto.parseBattAlert('bat: 3612 mv'), null);   // the BATT command's reply, not an alert
});

test('LGET rows', () => {
  assert.deepEqual(Proto.parseLgetLine('LUT:3:00112233445566778899'), { idx: 3, hex: '00112233445566778899' });
  assert.equal(Proto.parseLgetLine('LUT:7:00112233445566778899'), null);   // only rows 0..6
  assert.equal(Proto.parseLgetLine('LUT:3:0011'), null);                   // short
  assert.equal(Proto.parseLgetLine('LUTSET:TURBO (mode=0, custom=off)'), null);
});

test('TELE refresh timings', () => {
  assert.deepEqual(Proto.parseTele('TELE:full time=8707ms lut=BALANCED'), { fapply: null, fast: null, full: 8707, lut: 'BALANCED' });
  assert.deepEqual(Proto.parseTele('TELE:fast time=112ms lut=TURBO'), { fapply: null, fast: 112, full: null, lut: 'TURBO' });
  assert.deepEqual(Proto.parseTele('TELE:fapply time=700ms bw=4736 rw=0 lut=TONE_DARK'), { fapply: 700, fast: null, full: null, lut: 'TONE_DARK' });
  assert.deepEqual(Proto.parseTele('STAT:lut=custom host=1 frame=3 last=110 min=100 max=120'), { fapply: null, fast: null, full: null, lut: 'custom' });
});

test('FW/RW upload progress', () => {
  assert.deepEqual(Proto.parseFwRx('FW:rx 4096/15000'), { plane: 'FW', got: 4096, total: 15000 });
  assert.deepEqual(Proto.parseFwRx('RW:rx 96/4736'), { plane: 'RW', got: 96, total: 4736 });
  assert.equal(Proto.parseFwRx('FW:err no colon'), null);
});

test('vstream ACK and ready', () => {
  assert.deepEqual(Proto.parseVsAck('TELE:vs f=3 ms=110 dec=1 crc=1a wc=ok rs=0 w=1 s=1 r=0'), { ms: 110, wc: 'ok', rs: 0 });
  assert.deepEqual(Proto.parseVsAck('TELE:vs f=4 wc=bad rs=1'), { ms: 0, wc: 'bad', rs: 1 });
  assert.deepEqual(Proto.parseVsReady('VSTREAM:ready lut=TURBO type=RAW/RLE/DRLE crc=opt(+0x40) half=opt(+0x20)'), { crc: true });
  assert.deepEqual(Proto.parseVsReady('VSTREAM:ready lut=TURBO'), { crc: false });
});

test('AUTH handshake lines', () => {
  assert.deepEqual(Proto.parseAuth('AUTH:CHAL 00112233445566778899aabbccddeeff'), { kind: 'chal', nonce: '00112233445566778899aabbccddeeff' });
  assert.deepEqual(Proto.parseAuth('AUTH:OK'), { kind: 'ok' });
  assert.deepEqual(Proto.parseAuth('AUTH:FAIL'), { kind: 'fail' });
  assert.deepEqual(Proto.parseAuth('AUTH:ERR'), { kind: 'fail' });
  assert.equal(Proto.parseAuth('SETKEY:OK'), null);
});

test('versionCompare is numeric per component', () => {
  assert.equal(Proto.versionCompare('3.4.10', '3.4.9'), 1);
  assert.equal(Proto.versionCompare('3.4.9', '3.4.10'), -1);
  assert.equal(Proto.versionCompare('3.4', '3.4.0'), 0);
  assert.equal(Proto.versionCompare('3.1.49', '3.1.49'), 0);
});

test('buildDateToUnix reads the naive build stamp as UTC', () => {
  assert.equal(Proto.buildDateToUnix('2025-06-12_14:30:00'), Date.UTC(2025, 5, 12, 14, 30, 0) / 1000);
  assert.equal(Proto.buildDateToUnix('2025-06-12 14:30:00'), null);
  assert.equal(Proto.buildDateToUnix(null), null);
});

test('pickVariant: by layout, legacy fallback, and old single-image manifests', () => {
  const m = { version: '9.9.9', file: 'x.bin', variants: [
    { layout: 1, version: '3.4.29', file: 'app_update.bin' },
    { layout: 2, version: '3.4.29', file: 'app_update_v2.bin' } ] };
  assert.equal(Proto.pickVariant(m, 2).file, 'app_update_v2.bin');
  assert.equal(Proto.pickVariant(m, 1).file, 'app_update.bin');
  assert.equal(Proto.pickVariant(m, null).file, 'app_update.bin');   // old fw: legacy image
  assert.equal(Proto.pickVariant(m, 3), null);                        // no image: never guess
  const legacy = { version: '3.1.0', file: 'app_update.bin', size: 1, sha256: 'a', date: 'd', notes: 'n' };
  assert.equal(Proto.pickVariant(legacy, null).version, '3.1.0');
  assert.equal(Proto.pickVariant(legacy, 1).layout, 1);
  assert.equal(Proto.pickVariant(legacy, 2), null);                   // a v2 tag must not get a legacy image
  assert.equal(Proto.pickVariant(null, 1), null);
});
