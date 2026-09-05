# Firmware architecture: threads, locks, boot, persistence

What runs where, what may block what, and what survives a reboot. The
mechanisms themselves are commented at their definitions; this page is the
map. Sizes and priorities are from `prj.conf` and the `K_THREAD_DEFINE`s as
of firmware 3.4.29.

## Threads

| Thread | Stack | Prio | Runs | Defined in |
| --- | --- | --- | --- | --- |
| `main` | 1024 | 0 | Init in dependency order, then parks forever. | `src/main.c` |
| BT RX workqueue | 1280 | 8 | The Bluetooth host's receive path. `bt_receive_cb` → `commands_process`: assembles text lines and opcode frames into a queue slot, decodes vstream bytes straight into the frame buffer. Two commands run here inline as the escape hatch: `REBOOT`, `SYSINFO`. | Zephyr host; `src/app/commands.c` |
| `cmd` | 2048 | 7 | Every other text command and opcode frame, two deep. A slow handler (`CLEAN` ~60 s, `NUKE` minutes) stalls this queue, not the radio; a third command gets `BUSY:cmd`. | `commands.c`, `cmd_thread` |
| `mesh` | 2560 | 7 | Verifies, deduplicates, relays and executes flooded PDUs; builds and signs originated ones; writes the replay list to settings. Only `CMD_MESH`-flagged commands run from here, and their replies are dropped. | `src/app/mesh.c` |
| `screensaver` | 1536 | 7 | The display thread. Every 30 s: battery protection, stats roll-up, advertising self-heal. Then the scheduled redraw (clock mode), or a requested frame (picture mode), then sleeps to the next boundary. | `src/app/display_manager.c` |
| system workqueue | 1280 | -1 (cooperative) | The Bluetooth host's **TX processor**, advertising start / retry / settle, the mesh beacon pump, the vstream watchdog, the DFU busy timeout, the 15-s image confirm, the replay-list save trigger. | Zephyr; `ble_service.c`, `mesh.c`, `commands.c`, `main.c` |

Two consequences of that table drive most of the rules below:

- **Nothing that talks to the panel may run on the BT RX thread.** A panel
  refresh holds a thread for 0.1–9 s; on the RX thread that stalls every GATT
  exchange and the phone drops the link. That is why the command thread and
  the mesh thread exist and why DFU screens are drawn on the RX thread but
  refreshed by the display thread (`display_manager_request_frame_update`).
- **The system workqueue must never wait on the BLE TX pool.** The TX
  processor that frees the pool is itself a work item. `ble_printf` retries
  for up to 300 ms on every thread except this one.

## Locks

| Lock | Kind | Protects | Rules |
| --- | --- | --- | --- |
| `display_lock` | recursive mutex | The panel bus and the frame buffers, across the display, command, mesh threads and the workqueue. | Held across a full refresh, so a waiter can block ~15 s. Never hold it across `ble_printf`. The display thread takes it with a 2 s timeout (`DISPLAY_LOCK_OR_SKIP`) and skips a frame rather than freeze battery protection behind a 15-minute `NUKE`. `update_partial_nowait` releases it with BUSY still high, by design. |
| `persist` spinlock | spinlock | The retained-RAM stats block. | Short critical sections only; CRC resealed on every write. |
| `tx_lock`, `rpl_lock` | spinlocks | Mesh beacon ring; replay-list snapshot for the save job. | The replay list is written only on the mesh thread; the lock covers the copy the save takes. |
| `cmd_free` / `cmd_ready` | k_fifo pair | The two 256-byte command slots. | The RX thread fills a slot in place, the command thread executes it in place: one copy, no queue buffer. Disconnect purges `cmd_ready`. |

Reply delivery: `ble_printf` formats into a 128-byte buffer and sends one
notification. The ATT/L2CAP TX pools hold three buffers each and drain once
per connection interval (100–200 ms idle), so a burst of replies — `SYSINFO`
alone is four fragments — overruns them; the retry loop covers one idle
interval. A host that must have a reply on an old fleet re-asks; the web
app staggers its connect-time queries for this reason.

## Boot order (`main.c`)

1. `k_work_schedule(confirm_work, 15 s)` — the MCUboot image is confirmed
   only after it has demonstrably run. A crashing image resets
   (`CONFIG_RESET_ON_FATAL_ERROR`) before that, and MCUboot swaps the
   previous image back.
2. LEDs, battery ADC, system time from the build stamp, `persist_init`
   (validate the retained-RAM stats).
3. `graphics_init`, `display_manager_init` — the panel probe picks the frame
   layout (128×296 B/W/R or 400×300 B/W) out of the static arena.
4. `commands_init`, then `ble_service_init` → `bt_enable`, `settings_load`
   (every `SETTINGS_STATIC_HANDLER_DEFINE` below runs here), device name,
   NUS, advertising with the 90-s boot burst.
5. `secauth_init` resolves the effective key (NVS > factory_data > compiled
   default); `mesh_init` needs the node id and the key, so it comes after
   both. BLE failure skips only the mesh.
6. `persist_post_settings` — restore from the flash snapshot if RAM was lost,
   adopt the saved wall clock, count the boot, detect a firmware change.
7. `display_manager_recalibrate_energy`, `power_profile_apply` (advertising
   interval for the mode in force).
8. First screen: the clock, or nothing at all if the stored mode is picture —
   the panel is bistable and still shows it.

## Radio states

| State | Advertising | Connection params | When |
| --- | --- | --- | --- |
| boot burst | 100–150 ms, connectable | — | First 90 s after any boot, whatever the profile says: the phone that just rebooted the tag is waiting. |
| idle, clock | 2 s (profile `advc`, 1–10 s) | — | Screensaver on, nobody connected. |
| idle, picture | 5 s (profile `advp`) | — | Screensaver off. |
| connected | none | 100–200 ms, latency 4 | Normal command traffic. |
| streaming | none | 15–30 ms, 2M PHY | `VSTREAM:start`, frame-buffer uploads. |
| beacon | 30–50 ms, non-connectable, 1.2 s per PDU | unchanged | The single legacy advertising instance is borrowed for a mesh PDU, then restored. Legal while connected. |
| mesh scan | — | — | Passive, 30 ms every 1000 ms (3 % duty), the dominant idle consumer; `MESHRX OFF` stops it. |

Advertising must never silently die: a failed start reschedules a retry every
second, and the display thread's 30-s maintenance block restarts it if it
finds the tag idle, allowed and not advertising.

## What is persisted where

| Store | Survives | Contents |
| --- | --- | --- |
| Retained RAM (256 B above `sram0`, `persist.c`) | Warm reboot, OTA, crash. Not a power cycle. | Cumulative uptime, wall clock, boot and update counts, refresh counts, energy estimate. CRC-sealed; layout frozen at 80 bytes. |
| Settings / NVS (`settings_storage`, 8 KB legacy / 12 KB v2) | Everything short of a chip erase. | See the key table. |
| `factory_data` (v2 layout only, 4 KB, read-only) | Everything. | Serial, optional provisioned auth key. |
| The panel itself | Power loss, years. | The last image; picture mode boots without touching it. |

Settings keys, by module:

| Key | Module | Value |
| --- | --- | --- |
| `cfg/name` | `ble_service.c` | User part of the device name (NUL-terminated). |
| `sec/k`, `sec/en` | `secauth.c` | 16-byte runtime key; access-gate flag. |
| `pwr/p`, `pwr/b`, `pwr/m` | `power_profile.c` | Sleep profile; battery pack record; display mode (1 clock / 0 picture). Older, shorter blobs are migrated on load. |
| `mesh/g`, `mesh/rx` | `mesh.c` | Group id; observer scan on/off. |
| `mesh/seq` | `mesh.c` | Originator sequence checkpoint, every 64 PDUs; a boot resumes one step past it. |
| `mesh/rpl` | `mesh.c` | The replay-protection list (8 sources), written at most once per 10 s of mesh traffic. |
| `ps/d` | `persist.c` | Flash snapshot of the stats block, written on low battery and before a firmware update; marked consumed once carried forward. |

## Budgets

The nRF52832 has 512 KB flash and 64 KB RAM. Half the flash is the inactive
OTA bank; the signed image must stay under 229 376 B (legacy layout) or
MCUboot marks it "test" and never swaps it — `make build` refuses to publish
past that line. RAM is the binding constraint: the last build sits at
65 180 / 65 280 B. Stacks are static arrays, so that figure is the whole
picture; the MPU stack guard turns an overflow into a reboot rather than
silent corruption. `CONFIG_THREAD_ANALYZER` on a desk tag is the way to find
out what each stack really uses before trimming one.
