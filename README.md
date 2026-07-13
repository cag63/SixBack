<p align="center">
  <img src="images/sixback-logo-crop-text.png" alt="SixBack — local SoundTouch cloud replacement" width="480">
</p>

# SixBack

> *Bring your six back.*

A tiny ESP32 stick that brings back the six Internet-radio preset buttons on
**Bose SoundTouch** speakers after Bose shut down their cloud
(2026-05-06).  It speaks just enough of the BMX cloud protocol that the
speaker firmware — which can no longer be updated — happily keeps working.

No subscription, no account, no Bose servers.  One USB stick on your LAN.

> SixBack was formerly developed and published as *BoseFix32*.  All
> functionality is preserved; the rename reflects the project's identity
> independent of any Bose trademark.

## Status (v0.8.31)

| Component                                                            | State                                                                                                              |
| -------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| **Wi-Fi drop diagnostics + silent-disconnect detection (v0.8.31)**  | working — the Wi-Fi watchdog now treats the link as *up* only when the station is associated **and** holds a valid IP. A silent drop where the driver keeps reporting associated at `0.0.0.0` (a lost DHCP lease / failed renew) was previously invisible to a bare `WL_CONNECTED` check, so no reconnect fired; it is now detected and drives the periodic reconnect. `/api/status` gained two health fields — `wifi_disconnects` (a persistent count of detected link losses, even ones that never triggered a reboot) and `last_wifi_down_s` (duration of the most recent outage) — so a single status read after a drop shows whether the device merely lost the link or actually rebooted, without a serial cable. The diagnostic snapshot also carries a new `device` block (chip / PSRAM / heap / Wi-Fi / health) so an uploaded snapshot includes the board identity and health inline. Groundwork for diagnosing a reported WebUI-triggered Wi-Fi drop on a dual-band board (FHEM #144729) |
| **No-PSRAM heap headroom + WebUI stability under load (v0.8.30)**   | fixed — on the no-PSRAM chips (C5 / C6 / C3) the Spotify subsystem eagerly allocated its TLS clients and a worker task at boot even though Spotify needs PSRAM to run at all, wasting ~18-22 KB of heap. Under a full WebUI page load (a fresh browser tab firing the page bundle + several `/api` calls + the per-speaker probe fan-out at once) the tight heap could collapse, crash, and drop the Wi-Fi link — the page never finished loading and presets did not appear. `spotify::init()` now skips the eager allocation when there is no PSRAM (S3 with PSRAM is unchanged), and the page-serving / refresh-status endpoints gained heap-floor guards as a safety net under heavy concurrency. Verified on a no-PSRAM C5: a single-tab load no longer crashes (3/8 → 0/8) and the S3 shows no regression (FHEM #144729, #121) |
| **Auto-Migrate toggle persistence + honest save (v0.8.29)**         | fixed — the **Auto-Migrate at Boot** switch could silently fail to persist: `PUT /api/auto-mode` always reported success even when the underlying NVS write failed, so the Web UI showed the new state while nothing was stored — after a reboot (or the UI's immediate re-read of the persisted value) the switch snapped back. The endpoint now uses the same **cleanup-retry NVS path** as the other settings stores (purges regenerable caches on a full partition, then retries) and **reports the real result** — a failed persist returns HTTP 500 so the UI surfaces it instead of a false success. The stereo-pair group store got the same cleanup-retry save, and `/api/status` gained a `groups_persist_ok` health flag (kept as diagnostics, the speaker-facing BMX responses are unchanged). Reproduced and verified end-to-end on a freshly-erased C5 — a non-default toggle value now survives a reboot (FHEM #144729) |
| **Refresh status — non-blocking (v0.8.27)**                         | working — the global **🔄 Refresh status** used to run the entire inventory probe (a per-speaker Telnet/BMX status check, an up-to-30 s stale-view retry, plus a Spotify-source pull per speaker) **synchronously** inside the single HTTP request the browser was waiting on with no timeout — so on a large speaker zone the whole Web UI froze at the first step. The probe now runs in a **background task** (the same pattern as **Discover**): the request returns immediately and the UI polls `/api/speakers` until it finishes, staying responsive throughout. The HTTP helper also gained a request timeout so no single call can hang forever. Reported on the FHEM forum |
| **Optional WebUI password protection (v0.8.27)**                    | working — an optional **HTTP Digest** login can be turned on for the device Web UI and its `/api` (off by default; configured via `GET/PUT/POST /api/ui-auth`). It gates only the Web UI / API surface — the BMX / cloud-replacement endpoints the speakers themselves talk to stay open, so device compatibility is unaffected. On a trusted LAN this is deterrence rather than a substitute for network isolation, since the traffic is plain HTTP (#31) |
| **Clock display on/off — ST20 / ST30 (v0.8.25)**                    | working — speakers that have a display (ST20 / ST30) get a 🕒 **Clock display** on/off switch on their card. SixBack reads and writes the speaker's device-direct `GET/POST :8090/clockDisplay` (nested `<clockConfig>` envelope, `text/xml`), gated on the speaker's `capabilities.xml` `<clockDisplay>` flag so it only appears on speakers with a display (the ST10 has none). The write is read-modify-write, so the other clock-config fields (timezone / format / offset) are preserved. Verified on real ST30 hardware — the display is **binary on/off** (it cannot dim, so there is no brightness control) |
| **STORED_MUSIC source self-heal (v0.8.25)**                         | working — a DLNA / STORED_MUSIC preset push could fail (`/select=500`, "speaker hardware out of sync") after a power outage rebooted the whole network: the speaker came back before its DLNA media servers were rediscovered, so its `STORED_MUSIC` source was never re-registered — while TuneIn (always-on) and DLNA **browse** kept working, which is exactly the reported symptom. The periodic status check now probes for a READY `STORED_MUSIC` source on owned, migrated speakers that have media servers, and on miss re-runs `refreshMediaServers` + `/setMargeAccount` to re-register it — the same self-healing the TuneIn source already had. Idempotent across cron ticks; verified on real hardware (#30) |
| **Hardware preset buttons — re-arm cold LIR slots (v0.8.23)**       | working — a physical preset-button press for a Local-Internet-Radio slot is now detected in real time over the Bose **`gabbo` notification WebSocket** (`ws://<speaker>:8080/`, LAN-reachable via `wsapiproxy`). When a cold slot fails to reach *playing* state — the raw-URL location yields no `playStatus`, or the speaker reports `INVALID_SOURCE` — SixBack re-arms it through the native ORION `/select` (the same path the UI **Play** button uses, v0.8.20), now **hardware-triggered**, so the physical button just works again. A persistent hand-rolled RFC6455 WebSocket client (zero deps) is kept per owned speaker *that has LIR presets*, with a 3-layer loop-guard against re-select storms (failure-only trigger · 20 s self-select suppress · per-slot attempt cap) and an active liveness ping + idle-reconnect. **S3 / S3-8MB only** (PSRAM); compiled out (no-op) on C3 / C6 / classic (#15) |
| **WebUI security headers (v0.8.23)**                                | working — the device Web UI now ships a **Content-Security-Policy** + `X-Content-Type-Options: nosniff` as defense-in-depth against XSS from untrusted data rendered in the UI (RadioBrowser / TuneIn / DLNA station names, custom stream URLs). The single-file SPA is inline-heavy, so `script-src` keeps `'unsafe-inline'`; the real hardening is `frame-ancestors 'none'` / `object-src 'none'` / `base-uri 'self'` / `form-action 'self'` + https-only `img` / `connect` |
| **Speaker hardware fingerprint (v0.8.23)**                          | working — the speaker's `moduleType` (sm2 / scm wireless-module generation) and `variant` (rhino / mojo / spotty) are parsed from `/info` and surfaced in `/api/speakers`, so issue triage can tell apart hardware revisions that `model` (e.g. *SoundTouch 20*) hides |
| Cloud replacement (`/bmx/registry`, `/streaming/…`, `/updates/…`)    | working — 22 / 30 ueberboese-spec endpoints served                                                                 |
| **ESP32-S3 8 MB variant — Seeed XIAO ESP32S3 & similar** (v0.8.18)  | working — dedicated `s3-8mb` build target with its own partition table (`partitions-8mb.csv`, same 3 MB A/B OTA app slots as the 16 MB layout, 1.9 MB LittleFS), its own web-flasher button pair and its own OTA artifact channel (`sixback-s3-8mb-*`), so over-the-air updates always match the flashed layout. Spotify stays enabled (8 MB PSRAM). The OTA puller also gained a **pre-flight size check** on all targets: an image larger than its target partition is refused before anything is unmounted or written ("wrong build variant for this board?") (#23) |
| **Rename speakers from the WebUI** (v0.8.18, fixed v0.8.22)          | working — ✏ next to the speaker name renames it app-free, so the new name persists across reboots. SixBack sends the rename device-direct (`POST :8090/name`, which the speaker silently ignores unless the content type is `text/xml`). A **migrated (cloud-bound) speaker does not apply the rename locally — it delegates it to its cloud**, firing `PUT /streaming/account/{a}/device/{deviceId}` back at SixBack. v0.8.22 adds the handler for that callback and records the new name, so renames now stick on migrated speakers too — before v0.8.22 SixBack ignored the callback and the rename silently did nothing (earlier mis-attributed to a non-existent "one rename per power-on" device limit). The UI re-checks the speaker's actual name a few seconds later instead of trusting the HTTP status, and flags it if the rename didn't take. The System tab also shows the flash size (16 MB / 8 MB) so you can tell the S3 variants apart (#25) |
| **Spotify — Library + slot trigger** (v0.7.7 → v0.7.11)              | working — connect once via OAuth in the 🎵 Spotify sidebar tab, save tracks / albums / playlists as reusable **Library tiles** (device-side NVS, `GET/POST/DELETE /api/spotify/library`), then drag a tile onto a preset slot. A physical button press fires the Spotify Web-API `/play` to the speaker as a Connect device, with per-slot **shuffle** + **repeat** (one track / full album-playlist) and a live trigger log with 🎵-badges |
| **Media sidebar — search & drag onto slots**                        | working — a 4-tab Media panel (📻 Radio · 🔗 Stream · 🎵 Spotify · 💿 DLNA): search TuneIn / RadioBrowser stations, keep custom stream URLs and Spotify Library tiles, or browse DLNA servers, then drag any result straight onto one of a speaker's 6 preset slots |
| **Marge keep-alive** (v0.7.7)                                        | working — 60s background ping of `/setMargeAccount` to every known speaker; prevents the scmudc event-stream from going silent after hours of idle |
| **Marge pair-bootstrap** (`/setMargeAccount` round-trip)             | working — `/streaming/account/{a}/device/` echoes deviceid with Bearer-credentials                                 |
| **scmudc telemetry** — per-device NowPlaying + event trace           | working — body-captured `/v1/scmudc/{deviceId}` JSON parsed into per-speaker store                                 |
| TuneIn preset resolver (`Tune.ashx` + `Describe.ashx`)               | working — stations show with correct name & artwork. **AAC-only stations now play (v0.8.9):** without an explicit `formats=` filter TuneIn hands back a `notcompatible.enUS.mp3` placeholder for AAC-only stations (the speaker played a ~12 s "station not compatible" message, then stopped). The resolver now requests `formats=mp3` first (keeps the most-compatible stream for dual-format stations) and falls back to `formats=aac`, so AAC-only stations resolve to their real stream — the SoundTouch decodes AAC-LC and HE-AAC v1/v2 fine; HLS (`.m3u8`) variants are skipped. `POST /api/tunein/cache/clear` flushes stale cached resolutions after updating. |
| Preset push to speaker — serialized FreeRTOS queue (v0.6.0)          | working — single persistent worker drains pushes one-by-one; depth 16, 503 when full; refuses with an actionable HTTP 409 ("migrate this speaker first") when the speaker isn't migrated yet, instead of a confusing "didn't save" (v0.8.7); waits for the speaker to actually reach *playing* state before the long-press (up to ~18 s) so a slow stream-start no longer drops the preset (v0.8.8) |
| **Source self-healing** (v0.8.8)                                     | working — a migrated speaker whose SixBack account never bound (empty `margeAccountUUID` → no TuneIn/Spotify/DLNA sources registered, so every push failed with `/select=500`) is detected on the periodic status check and **auto-re-bound** (synthetic per-device account id + `/setMargeAccount`), re-registering its sources with no user action; a `⚠ sources not synced` badge + a **Re-Sync Sources** button surface it in the WebUI too (#10) |
| **Captive portal** — WiFi setup AP                                   | working — fixed the `ERR_TOO_MANY_REDIRECTS` redirect loop that broke the setup page (the root route was a regex pattern handed to a non-regex router); the portal now loads cleanly (v0.8.8, #12) |
| **Compressed NVS stores** (v0.8.17)                                  | working — the JSON stores (presets, inventory, stream/Spotify libraries) are now heatshrink-compressed in NVS (vendored [atomicobject/heatshrink](https://github.com/atomicobject/heatshrink) v0.4.1, 1.6 KB encoder state, measured factor ~2-4 on real store data), raising the per-stick ceiling from ~7 fully-loaded speakers to 15-20+. Values under 512 B stay plaintext; legacy stores migrate in place on first save; every decode path is fail-safe (fuzz-tested host-side, 2700 cases under ASan/UBSan) and a corrupt frame can never hang the boot. **Note:** firmware older than v0.8.17 cannot read compressed stores — after a manual downgrade the preset store re-seeds from the speakers, but stream/Spotify library tiles would need a re-import |
| **Preset / inventory store — NVS blob storage** (v0.8.15)           | working — both per-stick stores (preset assignments, speaker inventory) were single NVS *strings*, which hit ESP-IDF's hard 4000-byte `nvs_set_str` limit at roughly five speakers with full presets: from then on **every** save failed regardless of free space (the "partition full" error was misleading — the partition was two-thirds empty), and a cleanup pass could destroy the last persisted state, so presets vanished on the next reboot or OTA update. Both stores are now NVS **blobs** (page-chunked, limit = partition size), the cleanup backs up and restores the previous value instead of sacrificing it, and a genuine out-of-space reports an accurate error. Existing data migrates in place on first save. Practical ceiling on the 24 KB NVS partition is ~7 fully-loaded speakers per stick — beyond that, writes fail loudly but never destroy data |
| **Preset-loss defense** (Defense-in-Depth)                           | working — `handleMigrate` pre-imports; `/presets` and `account/full` return 404 when store empty; TUNEIN source-block carries `username=TuneIn` so `sourceAccount` survives every sync |
| **Opaque-source passthrough** — DLNA / UPnP / Bluetooth presets      | working — original `<ContentItem>` captured at import and replayed 1:1; `STORED_MUSIC` and `STORED_MUSIC_MEDIA_RENDERER` declared in `accountSources`; serialized as Bosman-schema `<preset>` blocks with `<location>` + `<source>` reference (v0.6.537) |
| **DLNA preset workflow** end-to-end                                  | working — verified on SoundTouch 30 with 6/6 OPAQUE slots reboot-persistent (2026-05-21)                           |
| **DLNA browse** in the WebUI (v0.8.0)                                | working — sidebar tab with speaker + server pickers, breadcrumb, drag-track-onto-slot; UPnP ContentDirectory:Browse SOAP runs in a small Pi5/Apache-fronted proxy so the firmware stays thin; tested against MiniDLNA, Fritz!Mediaserver |
| **DLNA preset recording** via drag-to-slot (v0.8.0)                  | working — `POST /api/speaker/{id}/dlna/preset/{slot}` emulates long-press (`/select` STORED_MUSIC ContentItem → 8 s settle → `/key` press+release) then re-imports `/presets` so the new OPAQUE slot is captured into the store with its `rawContentItem`; peer-aware refuse (HTTP 409) when the speaker is owned by another SixBack |
| **Migrate / Reboot progress modal** (v0.8.0)                         | working — both speaker actions open the same step-by-step progress dialog used by Refresh; status transitions are tracked by polling `/api/speakers`, with explicit timeout + last-status surfacing if the speaker never returns |
| Speaker telnet bootstrap (`sys configuration …` via TCP 17000)       | working                                                                                                            |
| **Migrate verify post-boot** (v0.7.632)                              | working — second `getpdo` after `waitForSpeakerBack_`; mismatch → `MIGRATE_FAILED` instead of silent `MIGRATED`    |
| **Migration robustness** (v0.8.21)                                  | working — the Telnet `:17000` migration bootstrap retries up to **3× with backoff** before giving up, and on persistent failure surfaces a clear *weak-WiFi* message with the speaker's **RSSI** instead of a generic timeout; CPU clock is set to each chip's maximum (e.g. C6 → 160 MHz) for snappier TLS/HTTPS work (#28) |
| Auto-import existing presets via BMX `/presets`                      | working                                                                                                            |
| **Stereo-Pair / Multi-Room group API**                               | working — POST/PUT/DELETE on `/streaming/account/{a}/group/`, NVS-persistent                                       |
| **Stereo pair — ST10 left/right pairing in the WebUI** (v0.8.16)    | working — SoundTouch 10 cards get a stereo-pair row: pick the right-channel speaker and the two ST10 join into one left/right stereo image that presents as a single device (`POST /addGroup` to the master, device-direct — the speaker itself registers the pair with the SixBack cloud store, so it survives ESP reboots). Un-pair works from either card (`/removeGroup` is routed to the master). ST10-only per the protocol (`supportedURLs` is *not* a reliable gate — an ST30 advertises `/addGroup` too — so the UI gates by model). Stale pair entries left behind by the Bose app's own un-pair path are now pruned when a new pair is registered (#22) |
| **Device-direct multiroom** (ZoneManager, v0.8.7)                    | working — group speakers straight through the speaker's own `/setZone` / `/getZone` on port 8090 (master + slaves); stateless, live truth read from the master's `/getZone` — a separate layer from the cloud group-store above; WebUI group-picker / badge / ungroup |
| **Auto-Mode** — discover + migrate + preserve presets on first boot  | working — gated by NVS flag, default on                                                                            |
| **Auto-Mode cron** — periodic re-check every 30 min when enabled     | working — light discovery + auto-claim/release + migrate newcomers; since v0.8.13 a speaker is only *released* to a **verified** foreign owner (a live SixBack peer, or an explicit revert to the Bose cloud) — a speaker pointing at a dead URL stays owned and is **re-claimed** automatically (covers stale bases after an IP change and retired second sticks; the re-claim path skips the model/firmware whitelist because the speaker has already been migrated successfully before) |
| **Peer-aware Auto-Mode** (v0.7.5)                                    | working — HTTP-probes other SixBack sticks in the LAN; skips speakers already claimed by a peer; UI shows `claimed by peer @ <ip>` |
| **Source-Normalizer** — TuneIn / Local / RadioBrowser → playable     | working — RadioBrowser UUID resolved via radio-browser.info                                                        |
| **LIR preset playback fix** (v0.8.20)                               | working — clicking *Play* on a Local-Internet-Radio (LIR) preset slot now starts the stream via `play-source` / the native ORION adapter instead of emulating a hardware `/key` press, which from a cold/idle speaker could leave the slot silent (#15) |
| **IP-Failsafe** — auto-remigrate on ESP-IP change, with pre-probe    | working — every migrated speaker stores the SixBack base URL as a fixed IP, so a DHCP change would strand them; SixBack detects its own IP change **at boot and at runtime** (WiFi reconnect event, v0.8.13) and re-points every speaker it owns, skipping those already on the new base. If a speaker is offline during the run (router swap — speakers boot slower than the ESP), the run retries every 60 s for up to 20 min instead of giving up (v0.8.13). A DHCP reservation for the SixBack MAC avoids the situation entirely and is still the recommended setup |
| **SETTLING status** (v0.6.541)                                       | working — backend reports `settling` instead of `offline` when only Telnet:17000 is down but BMX:8090 still answers |
| **Speaker status reliability** (v0.8.22)                            | working — fixed a status-tile flicker where the reachability fall-back probe hit the ESP's own port instead of the speaker's BMX port (8090) and flagged a live speaker as *offline*; the probe now targets the right port, uses a longer connect/read timeout, and a **2-strike debounce** requires two consecutive failed probes before a card flips to offline |
| Preset UI — drag&drop, dual-row (HW vs Store), per-slot revert       | working — modal progress, per-speaker export/import, refresh discards unsaved (v0.7.3)                             |
| **Custom stream library — device-side** (v0.8.5)                     | working — Stream-tab tiles persist in device NVS instead of per-browser localStorage; `GET/POST/DELETE /api/streams` + bulk import, one-time localStorage→device migration, Export/Import; survives USB-erase and browser change |
| **Add unreachable / LAN stream URLs** (v0.8.19)                      | working — the 🔗 Stream tab offers an *Add anyway* path: if the validator can't reach a URL (typical for LAN-only or self-hosted streams), the tile can still be saved, with a warning that the stream was not verified (#15) |
| **Speaker reordering** (v0.8.6)                                      | working — drag the ⠿ grip on a speaker card header to reorder the list; order is stored device-side (`POST /api/speakers/order`, persisted in NVS in the speaker-vector order), so it's identical in every browser and survives reboot; newly discovered speakers append at the end |
| Diagnostic snapshot (v0.6.0)                                         | working — `GET /api/speaker/{id}/diagnostic-snapshot` + one-shot pre-migrate snapshot persisted to `/snapshots/{deviceId}.json`; WebUI download or "Send to maintainer" upload to `sixback.io/snapshots/bosefix/snapshot` |
| OTA — app & LittleFS                                                 | working — `UPDATE_SIZE_UNKNOWN` + stream-to-EOF + 90% sanity-abort (v0.7.0 fix for HTTPS Content-Length truncation); a **size-scaled stall backstop** aborts a transfer that stops making progress, scaled to the image size instead of a fixed timeout (v0.8.22) |
| **OTA install — self-validating + clear status** (v0.8.3)            | working — the *Install* action re-checks the manifest itself instead of gating on a stale prior check, so a legitimate update is never blocked by a misleading "no update available"; distinct messages for *server unreachable* (retry) vs *already up-to-date* (use Force re-install); the WebUI panel always reflects the real state, so an error can no longer sit next to a stale "available" |
| **Manual "Flash web UI" — full-size S3 image** (v0.8.4)              | working — the WebUI upload guard rejected the ~9.9 MB S3 LittleFS image against a leftover 1 MB cap; raised to 11 MB so a manual FS upload matches the S3 spiffs partition. Verified end-to-end on S3 test hardware (~9.9 MB upload written, rebooted, FS intact). Also in v0.8.4: larger at-a-glance speaker status dots, and a GitHub project link in the WebUI + landing-page footer |
| **Tag-based release versioning** (v0.7.6)                            | working — `RELEASE_TAG` env bakes the same version string into all four target firmwares; eliminates multi-target build-drift |
| **Build size-gate** (v0.7.5)                                         | working — `build_release.sh` aborts if any firmware or LittleFS image exceeds its partition slot                   |
| **A/B-OTA partition layout**                                         | working — C3 / C6 / classic use symmetric `partitions-4mb.csv`: two 1.90 MB app slots (app0/app1) + 256 KB spiffs, so OTA flips between slots (no USB needed for updates). S3 uses two 3 MB app slots + 10 MB spiffs (`partitions.csv`). The size-gate refuses any image that won't fit its slot |
| WiFi provisioning — Improv-Serial (idle-window) + Captive AP         | working — both armed in parallel on cold boot                                                                      |
| **ESP32-C6 WPA2 reliability**                                        | working — `WiFi.setSleep(WIFI_PS_NONE)` + `setAutoReconnect(true)` applied **before** `WiFi.begin()`; closes 4-Way-Handshake-Timeout on WPA2-Mixed APs |
| System health — Task-WDT, WiFi / heap watchdog, crash counter, self-ping | working                                                                                                        |
| **Discovery stack-safety** (v0.8.5)                                  | working — the background discovery worker no longer overruns its task stack on setups with many speakers: SSDP responder collection and per-speaker probing now run in separate stack frames and the worker stack was enlarged. Fixes a stack-canary crash that rebooted the device mid-scan and left discovery finding 0 speakers (manual add still worked). Verified across S3 / C6 / C3 |
| **ESP32-C5 dual-band target** (v0.8.28)                              | working — `c5` build for the ESP32-C5 (RISC-V, dual-band Wi-Fi 6). Verified end-to-end on real C5 silicon (rev v1.0): boots from the C5's `0x2000` bootloader offset, connects on **5 GHz** (channel 40), Web UI + provisioning + OTA-check all work. 4 MB / no-PSRAM devkit → C6-equivalent config; `/api/status` now reports `band` + `channel`. Factory-image merge uses esptool ≥ 5 (4.x has no esp32c5 target) |
| Builds for **ESP32-S3 ★ / ESP32-C5 / ESP32-C3 / ESP32-C6 / ESP32-classic** | working — S3 is the recommended target; ESP32-classic re-enabled (`scripts/fs_exclude_esp32.py` trims the Spotify-only `silence.mp3` from its LittleFS image so the Web UI fits the 256 KB spiffs slot of `partitions-4mb.csv`) |
| ESP-Web-Tools landing page (auto-detects chip)                       | working — <https://sixback.io/>                                                                                    |

## Install (recommended)

Open the **web flasher** in Chrome or Edge desktop and click *Connect*:

> 🔗 **<https://sixback.io/>**

The page reads [`webflasher/manifest.json`](webflasher/manifest.json),
detects the chip family of the connected board, and writes the matching
factory image — bootloader + partition table + firmware + Web UI — in a
single shot.  Right after the flash, esp-web-tools also offers to hand
over WiFi credentials via Improv-Serial.

If Web Serial is unavailable, every target also ships an
`*-firmware.bin` (for OTA over WiFi) and `*-littlefs.bin` (for FS-OTA).

### ⚠ Auto-migration runs by default

A freshly-flashed device boots with **`auto_migrate_on_boot = true`** in NVS.
Once it is on your WiFi, it will:

1. Discover all SoundTouch speakers on the LAN (SSDP + ARP-probe).
2. For every eligible speaker (model whitelist `SoundTouch 10/20/30`,
   firmware whitelist `27.0.6.x` and `27.0.3.x`):
   - Read its current presets via the BMX API.
   - Normalize each preset (TuneIn passthrough; RADIO_BROWSER converted
     to a direct stream URL; DLNA / Bluetooth captured as opaque
     `<ContentItem>` and replayed 1:1).
   - Rewrite the speaker's cloud URLs via Telnet `:17000`.
   - Reboot the speaker; presets survive without long-press because the
     normalized list is embedded in the speaker's `account/full` sync.

If you'd rather drive each migration by hand, **turn the switch off at
the very top of `http://sixback.local/`** *before* the device finds your
speakers — or pre-disable it via `PUT /api/auto-mode` (Body:
`{"enabled":false}`).  The default is "on" because the typical install
path is *flash → provision → presets work*, and the foot-gun guards
(eligibility whitelists, `max_per_boot=4`) are tight enough that nothing
unrelated on your LAN gets touched.

After the initial boot pass, SixBack keeps the auto-mode pipeline alive
as a **periodic cron** (default every 30 minutes, configurable via
`cron_interval_s`).  Each tick does a light discovery (SSDP + known-IP
probe, no full `/24` sweep), runs Auto-Claim/Release on the inventory,
and migrates any newcomer that matches the eligibility whitelist.  A
speaker is only *released* when its new owner is verified — a live
SixBack peer answering on that URL, or an explicit revert to the Bose
cloud.  A speaker that points at a dead URL (a stale SixBack base after
an IP change, or a second stick that was retired) stays owned and is
automatically re-claimed on the next tick.  The countdown to the next
tick is visible at the top of the Web UI.

If multiple SixBack sticks coexist on the same LAN, the peer-aware
auto-mode (v0.7.5+) keeps them from fighting over the same speakers:
each stick HTTP-probes any foreign cloud URL it sees, and if the response
looks like another SixBack instance the speaker is left to its current
owner.  The UI labels such speakers as *claimed by peer @ &lt;ip&gt;*.

<p align="center">
  <img src="images/WebUIRadioSelector.png" alt="SixBack Web UI — radio/media selector with speaker preset slots" width="720">
</p>

The top of `http://sixback.local/` is where the **Auto-Migrate at Boot**
switch lives.  Below it every discovered speaker gets a card with its
current state (migrated / settling / original / foreign-cloud / offline),
its 6 preset slots, and per-speaker actions (migrate, revert, reboot,
edit presets, group sync).

## WiFi provisioning — two paths in parallel

On every cold boot the device opens **two** parallel provisioning
windows.  Whichever finishes first wins; the other is torn down.
Same pattern as the sister project [ip4knx / TUL KNX-Gateway](https://github.com/tostmann/ip4knx).

| Path           | When                                         | Window                                        |
| -------------- | -------------------------------------------- | --------------------------------------------- |
| Improv-Serial  | always                                       | 30 s idle (with creds) / 120 s idle (without) |
| Captive AP     | no NVS creds **or** STA-connect timeout      | 5 min idle                                    |

The **Improv** path is what esp-web-tools uses right after flashing.
The **Captive Portal** opens an **open** AP called `SixBack-XXYYZZ`
(no password) with a DNS hijack so any phone connecting to it gets the
WiFi-setup form automatically; after the user submits, the success
page auto-redirects to the device's freshly assigned LAN IP via
`<meta http-equiv="refresh">`.

## Supported hardware

| Chip          | Board reference                  | Flash  | Notes                                                            |
| ------------- | -------------------------------- | ------ | ---------------------------------------------------------------- |
| **ESP32-S3 ★**| `esp32-s3-devkitc-1` **with PSRAM** (any "R8" variant, e.g. N16R8 / N8R8) | ≥ 8 MB | **recommended** — **PSRAM is required** (TLS/HTTPS path for Spotify + OTA). The exact SKU is not important; clones are fine. 16 MB is the tested config and uses the default web-flasher button. **8 MB+PSRAM boards (e.g. Seeed XIAO ESP32S3) use the dedicated "S3 8 MB" button** on the web flasher (`s3-8mb` build: own partition table + own OTA channel) — do *not* use the standard S3 button on them, the 16 MB image does not fit the flash |
| ESP32         | `esp32dev` (DevKitC-1)           | 4 MB  | classic — **shipped again** (v0.8.x); `scripts/fs_exclude_esp32.py` trims the Spotify-only `silence.mp3` from its LittleFS image so the gzipped Web UI fits the 256 KB spiffs slot |
| ESP32-C3      | `esp32-c3-devkitm-1`             | 4 MB  | flashes over the chip's built-in USB-Serial-JTAG                 |
| ESP32-C6      | `esp32-c6-devkitc-1`             | 4 MB  | WiFi 6 — works, but cold-start discovery occasionally drops SSDP-multicast packets and rare HTTP-server hangs need a reset |
| ESP32-C5      | `esp32-c5-devkitc1-n4`          | 4 MB  | **dual-band Wi-Fi 6 (2.4 + 5 GHz)** — native USB-Serial-JTAG; verified connecting on 5 GHz (channel 40; `band`/`channel` shown in `/api/status`). 4 MB / no-PSRAM devkit, A/B-OTA like C3/C6. **Note:** the C5 second-stage bootloader lives at flash `0x2000` (not `0x0`), and merging its factory image needs esptool ≥ 5. 8 MB+PSRAM C5 boards (e.g. Seeed XIAO ESP32-C5) would warrant a separate build target |

**S3 is the recommended target for distribution.** During the 4-phase
end-to-end test (S3 ↔ C6 ping-pong with full erase/flash/provision each
round) the S3 hit 3/3 speakers discovered + migrated in every single
auto-mode run, while the C6 needed a second boot in one cold-start case
and produced one HTTP-server hang that recovered only after a hardware
reset.  The extra ~5 € for an S3-DevKitC-1 (with PSRAM) buys noticeable
robustness and plenty of free flash for future features.  Any S3 board
**with PSRAM** works — the specific flash size is not critical (the app is
~1.6 MB and the web UI ~160 KB), but a board *without* PSRAM will struggle
on the TLS/HTTPS path and is not supported.

C3, C6 and ESP32-classic are fully functional and stay built/published on
every release.  ESP32-classic is published again: `scripts/fs_exclude_esp32.py`
strips the Spotify-only `silence.mp3` stub from its LittleFS image so the
gzipped Web UI fits the 256 KB spiffs slot of `partitions-4mb.csv`.

All targets share the same source tree and the same Web UI; the
PlatformIO `extends = common` mechanism keeps the per-target diff small
([`platformio.ini`](platformio.ini)).

## What it does on the speaker

After clicking *Migrate* in the Web UI, SixBack talks to the Bose
Diagnostic Shell on **TCP&nbsp;:17000** of the speaker and rewrites the
cloud URLs the firmware caches in NVS:

```
sys configuration bmxRegistryUrl http://<sixback-ip>:8000/bmx/registry/v1/services
sys configuration statsServerUrl http://<sixback-ip>:8000
sys configuration margeServerUrl http://<sixback-ip>:8000
sys configuration swUpdateUrl    http://<sixback-ip>:8000/updates/soundtouch
envswitch boseurls set http://<sixback-ip>:8000 http://<sixback-ip>:8000/updates/soundtouch
sys reboot
```

No SSH, no firmware mod, no Bose login.  The change is fully reversible
via *Revert to original Bose* — the speaker returns to its factory URL
set even though the original cloud is offline.

## Build locally

Requires PlatformIO and a Linux/macOS host.

```bash
# build everything (all targets) + LittleFS images
pio run -e esp32 -e s3 -e s3-8mb -e c3 -e c6
pio run -e esp32 -e s3 -e s3-8mb -e c3 -e c6 -t buildfs

# produce tagged factory images + manifest for the web flasher
./scripts/build_release.sh v0.8.22    # tag arg bakes the version into all firmwares

# flash a single target via USB
pio run -e s3 -t upload
pio run -e s3 -t uploadfs
```

Versioning + build snapshots are automatic
(see [`scripts/version_bump.py`](scripts/version_bump.py)): every local
build snapshots the working tree before bumping `build_number.txt`, so
you can always roll back to the exact state a given binary was built
from.  Those snapshot commits stay **local** — only tagged releases are
pushed to the public repo.

## Repository layout

```
src/                  Firmware (Arduino + ESP-IDF mix)
web-src/              Web UI source (index.html, gzipped at build time
                      into data/ for LittleFS)
webflasher/           esp-web-tools landing page + manifest (binaries
                      are .gitignored — rebuild via build_release.sh)
images/               README assets — title PNG + Web-UI screenshot
scripts/              version_bump pre-build hook + build_release.sh
partitions.csv        16 MB partition table  (ESP32-S3 16-MB modules)
partitions-8mb.csv    8 MB partition table   (ESP32-S3 8-MB modules, e.g. Seeed XIAO)
partitions-4mb.csv    4 MB partition table   (ESP32 / C3 / C6)
platformio.ini        Multi-env config, see `[common]` + `[env:*]`
```

## Support

SixBack is free and open source. If it kept your speakers out of the
landfill and you'd like to say thanks, there's a tip jar via
[PayPal](https://paypal.me/busware) — entirely optional, and it helps keep
the lab stocked with test hardware. A ⭐ on the repo is just as welcome.

## Acknowledgements

- **[atomicobject/heatshrink](https://github.com/atomicobject/heatshrink)** (v0.4.1, ISC) —
  embedded LZSS compressor vendored under `src/heatshrink/`; SixBack uses it
  to compress the NVS-persisted JSON stores (presets, inventory, libraries)
  with a 1.6 KB encoder state, raising the per-stick speaker ceiling.
- **[julius-d/ueberboese-api](https://github.com/julius-d/ueberboese-api)** —
  OpenAPI specification of the legacy Bose SoundTouch streaming cloud,
  reconstructed from observed traffic. It is SixBack's verifiable
  ground-truth for endpoint shapes, header semantics, and event-body
  formats (scmudc envelope, NowPlaying structure, kebab-case event
  types, group/preset XML).  Thanks to **julius-d** for publishing it.

- **[tostmann/ip4knx](https://github.com/tostmann/ip4knx)** — sister
  project. The dual-path WiFi provisioning (Improv + Captive in
  parallel) and the system-health / self-ping watchdog pattern are
  carried over from there.

## Disclaimer

SixBack is an independent open-source project.  It is **not** affiliated
with, endorsed by, or sponsored by Bose Corporation.  All references to
Bose products and protocols are nominative, for interoperability with
hardware their owners have already paid for.  Use at your own risk.

## Licence

[PolyForm Noncommercial 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0).
See [LICENSE](LICENSE) for the full text and
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) for upstream
component licences.
