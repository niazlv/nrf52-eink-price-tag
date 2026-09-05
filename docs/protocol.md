# Command protocol

Everything a host can say to a tag and everything a tag says back, as of
firmware 3.4.29. The registry that defines it is `src/app/commands.c`
(`commands[]`), the opcode numbers are `src/app/cmd_opcodes.h`, and the web
app's reading of the reply lines is `lut_tester_host/web/protocol.js` (tested
in `tests/web/`). When this page and the code disagree, the code wins and
this page is wrong.

## Transport

Commands travel over the Nordic UART Service (NUS). Three encodings share the
RX characteristic and are told apart by their first byte:

| Encoding | Shape | Use |
| --- | --- | --- |
| Text line | ASCII, terminated by `\n` (`\r` ignored), at most 255 characters. | Everything a person types. |
| Opcode frame | `A5 <opcode> <len> <payload…>` — `0xA5` is only a frame start at a line boundary. Payload = the same text arguments the handler would parse. | Saves bytes; the same inner PDU the mesh carries. |
| vstream | `AA 55 <type> <len16> <payload> [crc16] BB` after `VSTREAM:start`. | Binary video frames, see `lut_tester_host/vstream.py`. |

Replies are text lines ending in `\r\n`, each one NUS notification of at most
127 characters (`ble_printf`'s buffer). A long reply is several lines; `SYSINFO`
is four fragments of one logical line, so buffer until `\n`. Notifications can
be lost when a burst outruns the TX pool — a host that needs a reply re-asks.

### Dispatch rules

- A text name is an alias for an opcode; both reach the same handler.
  Names ending in `:` or `=` take the rest of the line as the argument;
  others take what follows a space.
- Commands run on a dedicated thread, two deep. While both slots are busy the
  tag answers `BUSY:cmd` and drops the line. `REBOOT` and `SYSINFO` run inline
  on the receive thread and always get through.
- During a firmware transfer only `DFU:`, `REBOOT`, `SYSINFO`, `HOST:` are
  accepted; everything else is answered `BUSY:dfu`. The window opens on
  `DFU:START` or the first SMP upload chunk and closes on `DFU:DONE`, the end
  of the transfer, disconnect, or ten idle minutes.
- With the access gate on (`SEC ON`) an unauthenticated connection may only run
  `AUTH` and `SYSINFO`; the rest are refused with `ERR:auth required (send AUTH)`.
- An unknown line gets `unknown cmd`; an unknown opcode `ERR:bad opcode XX`.
- On disconnect: a running stream is torn down and the screensaver restored,
  session LUT slots are cleared, the auth session ends, queued commands are
  discarded.

## Commands

Flags: **M** = broadcast-safe, may arrive over the mesh (`CMD_MESH`);
**N** = allowed before authentication (`CMD_NOAUTH`).

### System and identity (0x01–0x0F)

| Op | Text | Args | Reply | Flags |
| --- | --- | --- | --- | --- |
| 01 | `HELP` | | `cmds (op A5 <op> <len> <payload>):` then one `  XX NAME — help` line per command | |
| 02 | `REBOOT` | | `REBOOT` then a cold reboot | M |
| 03 | `SYSINFO` | | see [SYSINFO](#sysinfo) | N |
| 04 | `STATS` | | see [STATS](#stats) | |
| 05 | `DFU:` | `START` / `DONE`, optional ` SILENT` | `DFU:ACK`, `DFU:DONE_ACK` (+ ` silent`), or `DFU:usage START|DONE [SILENT]`. Without SILENT the panel shows an update screen; with it the picture is left alone. | |
| 06 | `AUTH` | none, or 32 hex | `AUTH:CHAL <32 hex nonce>` / `AUTH:OK` / `AUTH:FAIL` / `AUTH:ERR`. Response = AES-128-ECB(key, nonce). One try per challenge. | N |
| 07 | `SETKEY` | 32 hex | `SETKEY:OK` / `SETKEY:ERR auth required` / `SETKEY:ERR rc=N`. Persists; must be authed. | |
| 08 | `SEC` | none, `ON`, `OFF` | `SEC:ON authed=1` (query) / `SEC:ON` / `SEC:OFF` / `SEC:ERR` | M |
| 09 | `NAME` | none, text (≤16), or `-` to clear | `NAME:<full advertised name>` — the user part plus the permanent ` (XXXXXX)` id | M |
| 0A | `BATT` | | `bat: <mV> mv` (0 on a failed read) | |
| 0B | `TIME` | `HH:MM:SS DD.MM.YYYY` | `Time Set` / `parse error` / `usage: …` | M |
| 0B | `TIME=` | `HH:MM:SS` (keeps the date) | `TIME set H:MM:SS` | M |
| 0C | `HOST:` | `0` / `1` | `HOST:0` / `HOST:1` — machine mode turns on `TELE:` lines | |
| 0D | `STAT` | | `STAT:lut=custom|builtin host=0|1 frame=N last=ms min=ms max=ms` | |
| 0E | `BCAST` | `<all|gN|6hex> <CMD…>` | `BCAST op=XX -> queued|args too long|full`, or `BCAST: …` errors (bad target, unknown cmd, not broadcast-safe) | |
| 0F | `GROUP` | none or 0–255 | `group=N` / `group: 0..255` | M |

### Display (0x10–0x3F)

| Op | Text | Args | Reply | Flags |
| --- | --- | --- | --- | --- |
| 10 | `SAVER` | | `saver enabled` | M |
| 11 | `SS:` | `0` / `1` | `screensaver on|off` / `usage: SS:0/1` | M |
| 12 | `DSAVER` | `0` / `1` | `Dynamic Saver: …` / `usage: DSAVER 0/1` | M |
| 13 | `CLEAR` | | `cleared` — clears the buffer and refreshes | M |
| 14 | `CLEAN` | | `cleaning (7 cycles)...` … `done` (~60 s, blocks the queue) | |
| 15 | `NUKE:` | cycles (default 20) | `NUKE: N cycles (~M min) — clearing ghost...` … `NUKE: done` | |
| 16 | `UPDATE` / `APPLY` | | `updating...`, `TELE:full time=<ms>ms lut=<name>`, `done <ms>ms` | M |
| 17 | `FAST` | | `fast update...`, `TELE:fast time=<ms>ms lut=<name>` | M |
| 18 | `FAPPLY` | none, or `RED` | `FAPPLY bw=N rw=N...`, `TELE:fapply time=<ms>ms bw=N rw=N lut=<name>`, `FAPPLY done <ms>ms`; `FAPPLY:red staged rw=N` for `RED` | M |
| 19 | `MODE:` | 0–9 | `Mode Set: N (…)` / `usage: MODE: 0-7` | M |
| 1A | `PALTEST` | | `PALTEST: rendering …` / `PALTEST: done` | |
| 1B | `TONETEST` | | `TONETEST: …` / `TONETEST: done (…)` | |
| 1C | `TEXT:` | text | `drawn` | |
| 1D | `ROT:` | 0–3 | `rotation: N` / `usage: ROT: 0-3` | M |
| 1E | `ANIM` | | `Starting Animation (Reset to stop)...`, then `Anim #N  <ms>ms/frame` | |
| 1F | `TEST` | | `Starting Partial Stress Test (Infinite)...` — never returns | |
| 20 | `VSTREAM:` | `start[:preset]` / `stop` | `VSTREAM:ready lut=<name> type=RAW/RLE/DRLE/RAW2/RLE2/DRLE2 crc=opt(+0x40) half=opt(+0x20) fmt=AA55 tt LL LL [payload] [crcHi crcLo] BB stop=CCDD/CCDE`; then per frame `TELE:vs f=N ms=N dec=N crc=XX wc=ok|bad rs=N w=N s=N r=N`; `VSTREAM:stopped` / `VSTREAM:photo_stopped` / `VSTREAM:timeout` / `VSTREAM:bad lut '…'` / `VSTREAM:unknown` | |

### Waveform editor (0x40–0x5F)

| Op | Text | Args | Reply |
| --- | --- | --- | --- |
| 40 | `LUTW:` | 140 hex | `LUT written (70 bytes) — custom LUT active` / `LUTW: need N hex, got M` / `LUTW: bad hex at N` |
| 41 | `LW:` | `idx:HH..` | `LW: wrote N bytes from [idx] — custom LUT active` / `LW: out of range` / `LW: bad hex at N` |
| 42 | `L:` | `n=HH` / `DUMP` / `RESET` | `L[n]=0xHH — custom LUT active` / `LUT[70]:` + hex / `LUT reset` |
| 43 | `LUTUSE:` | none, `0`, `1` | `LUTUSE:1 (custom…)` / `LUTUSE:0 (builtin…)` |
| 44 | `LUTSET:` | preset name | `LUTSET:<NAME> (mode=N, custom=off)` / `LUTSET: unknown preset '…'` / usage |
| 45 | `VLUT:` | `slot:base:off=val,…` / `slot` / `OFF` / `LIST` / `CLEAR` | `VLUT[N]:defined base=N patches=N`, `VLUT:N active`, `VLUT:off`, `VLUT:cleared`, `VLUT:slot N not defined`, `VLUT: none active`, `VLUT:err …` |
| 46 | `LGET` | | seven lines `LUT:N:<20 hex>` (N = 0…6), the 70-byte working table |
| 47 | `LTEST` | none or `0` | `LUT test started (LTEST 0 to stop)` / `LUT test stopped`; then `TELE:ltest frame=N last=N min=N max=N lut=<name>` per frame |

`LUTSET:` and `LUTUSE:` are broadcast-safe.

### Frame buffers (0x60–0x6F)

| Op | Text | Args | Reply |
| --- | --- | --- | --- |
| 60 | `FW:` | `offset:HH..` (≤96 bytes per line) | `FW:rx <bytes so far>/<plane size>` per line; `FW:err no colon`, `FW:err odd len N`, `FW:OOB off=N n=N`, `FW:no plane`, `FW:badchar off=N pos=N c=XX` |
| 61 | `RW:` | same, red plane | same with `RW:` |

A picture is `FW:` lines (and `RW:` lines on a two-plane panel) followed by
`FAPPLY`. On a single-plane panel `RW:` bytes are staged as red with
`FAPPLY RED` before the `FW:` lines.

### Debug (0x70–0x7F)

| Op | Text | Args | Reply |
| --- | --- | --- | --- |
| 70 | `VCOM=` / `DEBUG:VCOM=` | HH | `VCOM=0xHH` |
| 71 | `DEBUG:LUT=` | `idx:HH` | `LUT[idx]=0xHH` |
| 72 | `PROBE` | | `PROBE:<key>=<hex>` read-backs, `PROBE:status=XX chipid=N busy=N`, `PROBE:ram bytes=N match=N first_mismatch=N`, `PROBE:class=<controller> panel=<WxH>` |
| 73 | `RULER` | | `RULER WxH panel=<name>` — geometry test screen |
| 74 | `OTPLUT` | none, `:0`, `:1` | `OTPLUT=N` — full refresh from the factory OTP waveform (1) or the working table (0) |
| 75 | `SCAN:` / `SCAN` | `dummyHH,gateH` | `SCAN:dummy=XX gate=XX` |
| 76 | `SPITEST` | | `SPITEST: nop15000=<us>us (<us/B>) plane=<B> <us>us (<us/B>) clk=<Hz>Hz` |

### Radio and power (0x80–0x8F)

| Op | Text | Args | Reply | Flags |
| --- | --- | --- | --- | --- |
| 80 | `MESHRX` | none, `ON`/`1`, `OFF`/`0`, `FORGET` | `meshrx=on` / `meshrx=off (re-enable over NUS only)`; `FORGET` drops the replay list and answers `meshrx=on rpl=clearing` — over the connected link only, a flooded `FORGET` is ignored | M |
| 81 | `PWR` / `PWR:` | none, or `key=val,…` | see [PWR](#pwr) | M |

## Reply lines a host parses

### SYSINFO

One logical line, sent in four notifications:

```
SYSINFO:fw=3.4.29 build=2026-09-05_12:34:56 uptime=12345 bat=3612 mah=1.234 cur_ua=900 boots=12 fwupd=3 refr=400 refrfw=20 layout=1 serial= sec=0 authed=0 panel=128x296 phy=2 mode=clock
```

| Field | Meaning | Since |
| --- | --- | --- |
| `fw` | firmware version | |
| `build` | build stamp, naive UTC `YYYY-MM-DD_HH:MM:SS` | 3.4.28 (was missing before, see CMakeLists) |
| `uptime` | cumulative seconds, all boots | |
| `bat` | VDD in mV as the ADC sees it; saturates at 3600 | |
| `mah` | energy estimate since the model epoch, mAh with 3 decimals | |
| `cur_ua` | current the power model assigns to the present state, µA | |
| `boots` `fwupd` `refr` `refrfw` | persisted counters (also in STATS, which is less reliable at the tail of a burst) | 3.4.x |
| `layout` | 1 legacy, 2 v2 (own signing key, factory_data) — picks the OTA image | v2 batch |
| `serial` | factory serial, empty if unprovisioned | v2 batch |
| `sec` `authed` | access gate on; this connection authenticated | |
| `panel` | physical panel `WxH` | 3.4.4 |
| `phy` | 1 or 2 (2M PHY negotiated) | |
| `mode` | `clock` or `pic` — what the panel is doing; a host defaults a silent update on for `pic` | 3.4.22 |

### STATS

```
STATS:uptime=<s> wall=<unix> boots=N fwupd=N refr=N refrfw=N mah=1.234
STATS:flash=valid|consumed|none fl_uptime=<s> fl_wall=<unix>
```

`wall` is the tag's clock in the naive-UTC scale the build stamp uses; the
web app compares it with `build` + `uptime` to decide whether anyone ever set
the time.

### PWR

```
PWR:day=1 night=5 from=23 to=7 advc=2 advp=5 night_now=0 next=37 est=210 base=450 days=-1 [dry=1]
PWRB:chem=1 ser=1 par=1 cell=370 mv=3600 vsoc=25 vsat=1 used=12 soc=25 [dry=1]
```

Set keys: `day`/`night` redraw interval in minutes (0 = never by schedule),
`from`/`to` night window hours, `advc`/`advp` idle advertising seconds for
clock / picture mode (1–10), `chem` 0–7 (unknown, Li-ion, alkaline, LiFePO4,
NiMH, Li coin, LiFeS2, mains), `ser`/`par` cells in series / strings in
parallel (1–4), `cell` mAh per cell, `newbat=1` restarts the used-since
counter, `dry=1` previews without saving. Report: `night_now`, `next` seconds
to the next scheduled redraw (-1 none), `est`/`base` average µA for this
profile and for the everything-on baseline, `days` left (-1 unknown capacity,
-2 mains), `mv` pack voltage, `vsoc` charge from the chemistry curve (-1 no
curve), `vsat` 1 when the ADC is at its ceiling and `vsoc` is a lower bound,
`used` mAh since the pack epoch, `soc` the combined gauge the panel shows.
Errors are `PWR: unknown key …`, `PWR: out of range (…)`, `PWR: bad pack (…)`
— note the space after the colon.

### Unsolicited lines

| Line | When |
| --- | --- |
| `TELE:conn int=<ms> lat=N to=<ms>` | connection parameters granted |
| `TELE:phy tx=1M|2M rx=1M|2M` | PHY changed |
| `TELE:dynamic frame=N last=<ms>ms` | dynamic screensaver, per frame, HOST:1 only |
| `BATT:LOW mv=N` | display inhibited, warning screen shown |
| `BATT:SHUTDOWN mv=N` | farewell screen, then system off |
| `BATT:OK mv=N` | recovered from LOW |
| `BUSY:cmd`, `BUSY:dfu` | see dispatch rules |

## Mesh PDU

Carried as manufacturer-specific advertising data, company id `0xFFFF`, at
most 27 bytes:

```
[E5][ver<<4 | ttl][seq lo][seq hi][src 0..2][dst_type][dst 0/1/3][opcode][payload…][mac 0..3]
```

- `dst_type` 0 = all (no address), 1 = group (1 byte), 2 = id (3 bytes,
  the same hex shown in the device name). Payload room is 14, 13 or 11
  bytes respectively; the payload is the command's text arguments.
- `mac` = the first 4 bytes of AES-CMAC (RFC 4493) under the fleet's secauth
  key over everything before it, with the TTL nibble zeroed. Relays
  decrement TTL and re-beacon; TTL starts at 4.
- `seq` is per source. The originator saves the last value it used on every
  broadcast and resumes one past it after a reboot; a tag with no saved value
  starts at 1024. Receivers keep a 32-entry sliding window per source (8
  sources, least recently heard evicted), persisted as (src, highest seq),
  and drop anything already seen or older than the window, so a captured PDU
  cannot be replayed. A source not heard from for seven days of the
  receiver's uptime is forgotten and re-admitted at whatever it sends next;
  `MESHRX FORGET` over the connected link forgets all of them at once. Both
  exist for a source whose counter went backwards (old firmware, wiped
  settings). Serial arithmetic on 16 bits has one blind spot: a PDU captured
  more than 32768 broadcasts ago reads as new again.
- Only broadcast-safe commands run from a flood, and their replies are
  discarded. Heavy commands (DFU, VSTREAM, the LUT editor, FW/RW) stay on
  the connected link.

Codec and replay window: `src/app/mesh_pdu.c`, tested on the host with the
RFC 4493 vectors in `tests/host/test_mesh_pdu.c`.
