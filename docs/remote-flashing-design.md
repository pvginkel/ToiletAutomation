# Remote flashing and device logs: research and options

The problem: an agent in a KubeCoder environment builds firmware, and the device it runs on is
plugged into the operator's desktop. The goal is that the agent flashes a build and reads the
device's logs without the operator doing anything per build. The operator opens one page once,
connects the port, and leaves it connected.

Status: research and options, 2026-11-22. The design is bottomed out in a separate session, and
§10 lists the decisions it takes. Facts carry their source, and anything not verified is marked
**unverified**. §9 lists what a spike on the real board has to settle first.

Working name for the thing: **the bench**.

## 1. Where this is leaning

The leaning is a **KubeCoder catalog service**, a long-running sidecar of its own. It is not a
toolchain. It serves two things:

- **The page.** It is served on an `sslOffload` port. The operator opens it in Chrome or Edge,
  picks the COM port once, and leaves the tab open.
  - The page holds the port and runs **esptool-js** itself for every flash. The reset sequence is
    the one thing that cannot cross a network reliably, so it stays local.
  - Between flashes the page streams every byte the device prints to the server.
- **The agent API.** It is on `127.0.0.1` only, so it is unreachable through the edge.
  - `POST /flash` takes a build directory. With `mountWork` the service reads `flasher_args.json`
    from `/work` itself, so nothing is uploaded. The call blocks until the device is flashed and
    answers with the result.
  - `GET /log` (cursor, follow, wait-for-pattern), `POST /reset` and `POST /console`.
  - The log is also written to a file the agent can grep.
- **Decoding happens agent-side.** Backtrace and coredump decoding runs in the `esp-idf`
  toolchain against the build's ELF, so the service stays lean.
- **Network logs are already available.** Once the firmware is on WiFi, esp-mdm ships logs to
  Elasticsearch, which the agent can already query.

So: curl, yes, but against localhost, and pointing at a build directory rather than pushing bytes.
The whole agent-side flow is two calls:

```
curl -s localhost:<api>/flash -d '{"build_dir":"/work/ToiletAutomation/build"}'
curl -s 'localhost:<api>/log?since=<cursor>&wait=Ready&timeout=30'
```

The alternatives that were weighed:

- **A dumb browser pipe plus RFC2217.** This keeps `idf.py flash monitor` unchanged but sends
  resets across the network (§4 A2).
- **A desktop helper instead of a browser.** It is more robust but has to be installed (§4 A3).
- **OTA from the pod.** It takes the browser out of routine flashes, but needs firmware and LAN
  support (§4 A4). It is a later complement to the bench, not a replacement.

## 2. What the bench has to do

| # | Requirement |
|---|---|
| R1 | The agent flashes a build with one blocking call and gets a clear result: done, failed (with esptool's error), or no device attached. |
| R2 | The agent reads the serial log from boot, across resets and flashes, timestamped, and can wait for a pattern with a timeout. |
| R3 | The agent can reset the device and write a line to its console (for the firmware's test hooks). |
| R4 | Operator effort is one bookmark and, at most, one click per desktop session. Re-enumeration after a reset must not need a click. |
| R5 | Nothing goes through IoT Support. A dev build is not a release and must not reach the fleet. |
| R6 | It works for every firmware repo (SonosDisplay, Intercom, this one), whether the board has a UART bridge or native USB. |
| R7 | It never erases the provisioned `nvs` partition. It refuses a build for the wrong chip. |
| R8 | Backtraces in the log can be decoded to file and line. |

## 3. Facts the design rests on

### 3.1 The board, and why reset is the hard part

- **Native USB, probably.** The Waveshare ESP32-S3-Tiny has USB-C, BOOT and RESET, and no UART
  bridge is listed (Waveshare wiki, CircuitPython board page). Native USB-Serial-JTAG is very
  likely but **unverified**; plug it in and check for VID 0x303A, PID 0x1001. Its USB serial
  number is the chip's MAC (esp-iot-solution USB docs).
- **Every reset drops the port.** On USB-Serial-JTAG each chip reset restarts the USB peripheral,
  so the host sees the device disconnect and come back (esptool PR #980, Espressif forum).
  Re-enumeration can take seconds on some PCs. A UART-bridge board such as SonosDisplay's CH343
  does not do this, because the bridge stays up.
- **Two different reset sequences.** esptool and esptool-js use the USB-JTAG-Serial sequence for
  VID 0x303A/PID 0x1001 and the classic DTR/RTS sequence otherwise. Both have 50–100 ms waits
  (esptool-js `reset.ts`).
- **Early boot output can be lost on native USB.** USB-Serial-JTAG output without a host attached
  buffers, then pauses (ESP-IDF USB-Serial-JTAG console guide). After a reset, ROM and bootloader
  lines printed before the port is reopened may be lost or delayed. A UART-bridge board gets the
  whole boot log.
- **Console input needs the primary console.** The default ESP-IDF console is UART0, with
  USB-Serial-JTAG as an output-only secondary on the S3 (**unverified** default). A firmware that
  wants console input over USB sets `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` as the primary console.
- **The firmware can kill USB.** Light or deep sleep, or reusing GPIO19/20, makes the port
  vanish. Recovery is then the BOOT/RESET buttons, which needs the operator.

### 3.2 Web Serial

Sources: Chrome developer docs, MDN, the Chrome energy-saver and timer-throttling posts, and
Mozilla Hacks.

- **Secure context only.** The KubeCoder `sslOffload` front door provides it.
- **Port permissions.** `requestPort()` needs a user gesture. `getPorts()` returns ports this
  origin was granted earlier, and `open()` needs no gesture. A page can therefore reopen a granted
  port on load, and on a `connect` event, without a click.
  - **Unverified, and the key risk:** whether Chrome keeps the grant across the S3's
    re-enumeration. It is plausibly keyed on the USB serial number, which the S3 has. This is
    spike S1.
- **Disconnect.** It errors the open streams and nulls `readable`. When the device comes back,
  Chrome fires `connect`. Whether the page gets the same `SerialPort` object back or a new one
  is **unverified** (S1). esptool-js's `Transport.updateDevice(newPort)` exists for the new-object
  case.
- **Baud rate.** It is set at `open()`. Changing it means a close and reopen, which esptool-js
  does itself. USB-Serial-JTAG ignores baud altogether.
- **Exclusive access.** While the bench tab holds the port, IoT Support's flash page, a serial
  terminal and a local `idf.py` cannot.
- **Background tabs.**
  - Chrome's energy-saver freezing exempts pages that are using Web Serial.
  - Timer throttling still applies to hidden tabs: up to 1 s granularity, and once a minute after
    5 minutes hidden under chained timers. The `setTimeout` waits in a reset sequence could
    stretch.
  - Mitigations are a visible window, or running the serial work in a dedicated worker
    (`WorkerNavigator.serial` exists). Both are part of spike S2.
- **Browsers.** Chrome and Edge 89+. Firefox desktop since 151 (May 2026). Not Safari.

### 3.3 esptool-js, and what IoT Support already does

- **esptool-js** (Apache-2.0) is at 0.7.1 (2026-11-02). After a port is open it is driven
  entirely by async calls, with no UI of its own:
  - `new Transport(port)`
  - `new ESPLoader({transport, baudrate, romBaudrate, terminal})`
  - `main(mode)` to connect, sync, detect the chip, load the stub and switch baud
  - `writeFlash({fileArray:[{data,address}], compress, calculateMD5Hash, reportProgress})`
  - `after('hard_reset')`
- **Its reset choice is automatic.** It picks `UsbJtagSerialReset` on 0x303A/0x1001.
- **It does not reconnect.** `Transport` has `setDeviceLostCallback()` and `updateDevice(newPort)`
  for a device re-selected after a reset, but the page has to supply the reconnect.
- **IoT Support's flash dialog** (`frontend/src/hooks/use-flash-device.ts`) is the working
  reference.
  - It uses esptool-js **0.5.7**, at 115200 ROM and 460800 flash baud.
  - It flashes every image in the build's `flasher_args.json` at its own offset: bootloader,
    partition table, `ota_data_initial`, app. There is no erase-all and no merged binary.
  - It checks the chip against the manifest before writing.
  - It MD5-verifies each image.
  - It replaces esptool-js's `HardReset` with its own 100 ms RTS pulse, because 0.5.7's never
    asserted RTS.
  - It was built against SonosDisplay's UART bridge. Native-USB re-enumeration is not handled,
    and real-hardware reset behaviour was still owed at that slice's close (IoTSupportSpecs
    slice 001, V08).
- **Reuse.** The image-set logic, the chip check and the MD5 check carry over as they are. The
  reset workaround needs re-checking against 0.7.1.

### 3.4 KubeCoder: what a "sidecar" can be

From the `config.yaml` and `controller.yaml` references.

- **A toolchain cannot run a server.** A toolchain (or inline `tools:` entry) is a `kc cexec`
  target. KubeCoder replaces its command with the exec daemon, so it cannot run its own program.
  A server started in one through `cexec` is unsupervised and is lost on restart.
- **A service can.** A service runs its own program as a sidecar, supervised by the kubelet.
  Services come only from the deployment catalog in KubeCoderDeploy's chart values, not from a
  repo.
  - A service can set `mountWork: true` (read and write `/work`), `dataPath` (persistent storage)
    and `ports`.
  - Pilot's `pilot-sync` is the precedent: an estate image, `mountWork`, a localhost HTTP API,
    and `instructions` telling the agent how to call it.
- **HTTPS comes from the repo's `ports:`.** A `ports:` entry with `sslOffload: true` is served at
  `https://<name>.<env-id>.<suffix>`, with TLS at the edge. A service's own `expose:` has no
  `sslOffload`.
  - Containers in a pod share one network namespace, so a repo `ports:` entry should be able to
    front a port the service listens on. This is **unverified** (spike S4). If it does not work,
    the alternative is a catalog-level offload for service ports, which is a KubeCoder change.
- **The edge.** It is the `nginx-configurator` template in DockerImages.
  - Non-public sites allow only private address ranges, and there is no authentication.
  - The estate's proxy snippet forwards the WebSocket `Upgrade` and `Connection` headers.
  - HomelabAppsDeploy's nginx config sets a 120 s read timeout and a 500 MB body limit. Confirm
    that this is the config the KubeCoder front door runs (spike S3). Either way, any long-lived
    browser connection needs a heartbeat.

The answer to "sidecar or toolchain": **a service**. The agent needs no toolchain to use it,
because curl against localhost from the dev container or the `esp-idf` sidecar is enough. Usage
guidance goes into the catalog entry's `instructions`, which every selecting environment shows
through `kc env describe`.

### 3.5 Interactions with IoT Support and esp-mdm

- **A dev build reverts on its next boot.** On every boot esp-mdm fetches the model's current
  firmware from IoT Support. It installs it when the version string differs, and a dev build's
  git hash always differs (`esp-mdm/src/ApplicationBase.cpp`, `OTAManager.cpp`). The gate is
  `enableOTA` in the per-device configuration. See D7.
- **Publishing is releasing.** A pipeline upload makes the version the model's current one and
  notifies every device of the model. IoT Support has no dev channel, no pinning and no
  pre-release. Pushing dev builds through the MDM is ruled out by R5.
- **Logs already reach Elasticsearch.** esp-mdm hooks `esp_log` and publishes over MQTT to
  `iotsupport/logsink`. IoT Support writes that to Elasticsearch (`logstash-http-*`, by the
  device's `entity_id`). Every KubeCoder environment has read credentials, so post-boot logs are
  available to the agent today. They start only once WiFi and MQTT are up.
- **Coredumps.** They go from flash to IoT Support on the next boot and are parsed there against
  the stored ELF. For a dev build IoT Support has no matching ELF, so bench coredumps are decoded
  agent-side.
- **Provisioning.** It is the `nvs` partition, written once by IoT Support's Provision device. The
  bench writes only the `flasher_args.json` images, never `nvs`, and never erases all.

## 4. Option set A: where the serial protocol runs

This is the core choice.

### A1. The browser flashes, and the server hands it jobs (leaning)

The page runs esptool-js. The server queues a flash job. The page fetches the images, flashes,
hard-resets, reopens the port and resumes the monitor, then reports the result. The agent's
blocking call returns that result.

- **Pros.** Reset timing stays local. It sidesteps RFC2217's native-USB limitation (A2). It is the
  same library and flow IoT Support already runs. The server never speaks the serial protocol.
- **Cons.**
  - Only what esptool-js does is available: write and verify, read flash, erase. There is no
    `idf.py monitor`, no efuse tools and no full esptool.
  - Two readers cannot share the port, so the monitor pauses during a flash and the log marks
    the gap.
  - The page has to own reconnect after re-enumeration.

### A2. The browser is a byte pipe, and the server speaks RFC2217

The page only shuttles bytes and DTR/RTS between Web Serial and a WebSocket. The service exposes
`rfc2217://localhost:<port>`, so the agent runs plain `esptool` and `idf.py flash monitor`.

- **Pros.** Standard tooling, including `idf.py monitor` decoding, every esptool command, and NVS
  writes.
- **Cons.**
  - Each DTR/RTS toggle is a network round trip.
  - esptool's docs say USB-Serial-JTAG and USB-OTG are not supported over `rfc2217://`, because
    esptool picks the reset sequence from the local VID/PID. It would have to be forced with
    `--before usb-reset` (or `no-reset` and the buttons).
  - The port re-enumerates mid-session while esptool waits on the far side, and the pipe has to
    hide that.
  - `esp_rfc2217_server` solves the timing problem by running the reset locally on the server.
    Here the "server" would have to be the page, which means reimplementing that logic in it.
- **Prior art.** studioalight/esp-serial-relay (MIT, about March 2026, no adoption to speak of)
  does exactly this: a browser tab holds the port, a Python daemon speaks RFC2217 on 127.0.0.1,
  and it uses `--before usb_reset` for 0x303A. It is worth reading before building either A1 or
  A2.

### A3. A desktop helper instead of a browser

Run `esp_rfc2217_server` (from `pip install esptool`), or a small purpose-built agent, on the
desktop against the COM port.

- **Reaching it.** Either the pod reaches the desktop on the LAN, or the helper dials out to the
  service over a WebSocket.
- **Pros.**
  - The reset runs on the desktop next to the port.
  - pyserial reconnects without anyone's permission.
  - No tab has to stay open, and it can run as a Windows service.
- **Cons.**
  - Software to install and keep running on the desktop.
  - The native-USB RFC2217 question from A2 applies here as well (**unverified** for
    `esp_rfc2217_server`).
  - It is the opposite of "I open a page".

### A4. Flash over the network after the first flash (a complement)

The service also serves the build's app image by URL. A dev build takes a command (MQTT, HTTP or
console) to fetch it with `esp_https_ota` and reboot. Logs come from Elasticsearch, or from a UDP
log sink in the service.

- **Pros.** No USB in the routine loop, and the device can be anywhere (mounted in the toilet).
- **Cons.**
  - It needs firmware support, which could be an esp-mdm dev hook.
  - The device must reach the pod's HTTPS address and trust the homelab CA.
  - There is nothing before WiFi is up, and nothing after a bad build: recovery is USB again.
  - Rollback must be enabled, or a bad build means a trip to the device.
  - Pod-to-LAN and LAN-to-pod reachability is unknown (HLD Q6).

### Comparison

| | A1 browser flashes | A2 browser pipe + RFC2217 | A3 desktop helper | A4 network OTA |
|---|---|---|---|---|
| Reset timing | local | across the network | local | none needed |
| Native USB (S3) | supported by esptool-js | forced, untested | untested | n/a |
| Agent tooling | bench API | `idf.py`, `esptool` | `idf.py`, `esptool` | bench API |
| Operator effort | tab open | tab open | install + run | none after the first flash |
| Pre-WiFi and crash logs | yes | yes | yes | no |
| Build effort | page + server | page + server + RFC2217 | small, mostly off-the-shelf | firmware + server |

## 5. Option set B: how it is packaged in KubeCoder

1. **Catalog service (leaning).** It is defined in KubeCoderDeploy's chart values like
   `pilot-sync`, from an image built by Jenkins.
   - Repos select it with `services: [<bench>]` plus one `ports:` entry with `sslOffload: true`.
   - `mountWork` gives it the build directories. `dataPath` keeps the log across restarts.
   - It is supervised, the same in every firmware environment, and documented through
     `instructions`.
2. **Inline tool in this repo.** Fastest to prototype, with no KubeCoder change. The server is
   started through `cexec` and dies on restart. Prototype only.
3. **A process in the `esp-idf` sidecar.** It needs no new image and has `addr2line` at hand. It
   has the same supervision problem as option 2, and it bolts a server onto a toolchain.
4. **One central bench for all environments.** This could be a standalone app or an IoT Support
   feature next to Flash device, reusing its esptool-js code and Keycloak login.
   - **Pros.** One tab and one port for every project.
   - **Cons.**
     - It needs authentication and per-environment routing ("which env owns the device now?").
     - Builds must be uploaded rather than read from `/work`.
     - It couples dev tooling to the production MDM.
     - It loses per-environment isolation.

Where the code lives under option 1: a repo of its own for the server and page, its image in the
estate's pipeline, and the catalog entry in KubeCoderDeploy.

## 6. Option set C: the agent's interface

- **How the build is handed over.**
  - A path (`build_dir`) read through `mountWork`. Nothing moves, and the service reads
    `flasher_args.json`, `project_description.json` (chip and project name) and the images.
  - Or a multipart upload of the same ZIP layout IoT Support's upload script produces, which a
    central bench (B4) would need.
  - Both can be supported.
- **The calls.** These are a sketch, not a contract.
  - `POST /flash`. It blocks, takes a timeout, and refuses on a chip mismatch or when no port is
    open.
  - `GET /status`: page connected, port open, chip and MAC.
  - `GET /log?since=&follow=&wait=<regex>&timeout=`.
  - `POST /reset`, `POST /console`.
- **Device identity.** `/status` reports the MAC. A guard can refuse to flash a device other than
  the environment's expected one, so a project cannot flash another project's board that happens
  to be plugged in.
- **The wrapper.** Plain curl, a small `bench` CLI (baked into the `esp-idf` image or kept in
  `esp-libs/tools`), or a repo script. `project.yaml` has no verb for "flash", so it would not be
  a `kc project` verb.
- **Decoding.** The agent runs `xtensa-esp32s3-elf-addr2line` and `esp-coredump` in the `esp-idf`
  toolchain against `build/<project>.elf`. Both need only the log text and the ELF (ESP-IDF
  monitor and core-dump guides). The service could add decoding later, but that would pull the
  toolchain into its image.

## 7. Logs

- **Serial, through the page.**
  - Every byte read is streamed to the server, timestamped and stored in a ring buffer plus a
    rotating file.
  - Session markers go in the log: flash started and finished, reset, port lost, port reopened.
  - This is the only source for boot, crash and pre-WiFi output. The native-USB early-boot gap
    from §3.1 applies.
- **Network, through esp-mdm.** Already in Elasticsearch, from MQTT-up onwards. Good for long
  soaks, and for the device once it is mounted and off USB. A helper that queries by the device's
  `entity_id` is cheap to add.
- **Transport from page to server.** Either a WebSocket, which the edge forwards, with a ping
  inside the read timeout, or SSE for commands plus batched POSTs for log data. SSE plus POST
  survives any proxy. A WebSocket is simpler to program for a bidirectional console.

## 8. Security

- **Anyone on the LAN.** The page is reachable only from private ranges and has no login.
  Anyone on the LAN can open it. Without the operator's USB port that copy is harmless to the
  device, but it could read the log or pose as the bench's page to the server. The server
  should therefore accept one page session at a time. Nobody on the LAN can start a flash,
  because only the localhost API does that.
- **The agent API.** It binds to `127.0.0.1`, so it is unreachable through the edge. Only the
  pod's own containers can flash.
- **Further options.** No auth (homelab), a per-environment token in the page URL, or Keycloak in
  front.

## 9. Spikes before the design session

These can be run from a throwaway page plus the operator for ten minutes. No server is needed.

- **S1. Reset and re-enumeration on the real S3-Tiny.** With esptool-js 0.7.1 over Web Serial:
  - Does `main()` enter download mode with the USB-JTAG sequence?
  - Does the port drop on that reset, and on the hard reset after flashing?
  - Does `getPorts()` hand it back with the grant intact, and does `open()` work without a click?
  - How long does it take on the operator's Windows machine?

  This decides whether R4 is achievable.
- **S2. A hidden tab.** Flash after the tab has been in the background for more than 5 minutes.
  Repeat with the serial work in a dedicated worker.
- **S3. The edge.** A WebSocket through an `sslOffload` port: does it upgrade, and what is the idle
  timeout?
- **S4. KubeCoder.** Can a repo `ports:` entry with `sslOffload` front a port served by a service
  sidecar, or only one in the dev or tool containers?
- **S5. Optional.** Read esp-serial-relay. Does its forced `usb_reset` over RFC2217 actually work
  on an S3? That decides whether A2 stays on the table.

## 10. Decisions for the design session

- **D1. Where the serial protocol runs.** A1, A2 or A3 (§4). Leaning A1.
- **D2. Packaging.** A catalog service, or a prototype as an inline tool first. Per environment or
  central (§5). Leaning: catalog service, per environment.
- **D3. Build handover.** By path through `mountWork`, by upload, or both (§6).
- **D4. Agent interface.** curl and an API documented in `instructions`, or a CLI, and where the
  CLI lives (§6).
- **D5. Page-to-server transport.** WebSocket or SSE plus POST (§7).
- **D6. Log retention.** Size and location (`dataPath` or a file under the repo's build
  directory), and whether the service also pulls the device's network logs.
- **D7. Keeping a dev build on the device.** Options:
  - The bench device's `enableOTA: false` in IoT Support. This needs no code, but the operator
    must remember to flip it back.
  - An esp-mdm Kconfig that skips OTA in dev builds. That needs a dev/release build split.
  - A per-device pinned version in IoT Support. That is a feature there.
- **D8. Scope of v1.** Flash, log and reset only, or console input too. Whether A4 (network OTA)
  is planned now or later. Whether NVS provisioning ever moves into the bench (today it is IoT
  Support's).
- **D9. Security.** None, a URL token, or Keycloak (§8).
- **D10. Multiple devices.** One port per environment, or several (Intercom has two hardware
  versions).

## Sources

- **esptool-js.** README and source (`esploader.ts`, `reset.ts`, `webserial.ts`):
  https://github.com/espressif/esptool-js. npm registry: https://registry.npmjs.org/esptool-js.
- **esptool remote serial ports, RFC2217 and `esp_rfc2217_server`.**
  https://docs.espressif.com/projects/esptool/en/latest/esp32s3/remote-serial-ports.html
- **esptool PR #980, USB re-enumeration on reset.** https://github.com/espressif/esptool/pull/980
- **Web Serial.** https://developer.chrome.com/docs/capabilities/serial and
  https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API
- **Chrome freezing and throttling.** https://developer.chrome.com/blog/freezing-on-energy-saver
  and https://developer.chrome.com/blog/timer-throttling-in-chrome-88
- **Firefox Web Serial.** https://hacks.mozilla.org/2026/05/web-serial-support-in-firefox/
- **ESP32-S3 USB-Serial-JTAG descriptors.**
  https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/usb_device_const_COM.html
- **ESP-IDF 5.5 guides.** IDF Monitor, Core Dump, USB-Serial-JTAG console, `esp_https_ota`,
  logging: https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/
- **esp-serial-relay.** https://github.com/studioalight/esp-serial-relay
- **ESP Web Tools.** https://github.com/esphome/esp-web-tools
- **Board.** https://docs.waveshare.com/ESP32-S3-Tiny and
  https://circuitpython.org/board/waveshare_esp32_s3_tiny/
- **SGP40.** https://wiki.dfrobot.com/sen0394/
- **Estate sources.**
  - IoT Support: `frontend/src/hooks/use-flash-device.ts`, `docs/firmware-flashing.md`,
    `backend/app/api/pipeline.py`, `backend/app/services/logsink_service.py`.
  - esp-libs: `esp-mdm/src/ApplicationBase.cpp`, `OTAManager.cpp`, `LogManager.cpp`.
  - KubeCoderDeploy: `chart/values.yaml` (the `pilot-sync` and `esp-idf` entries).
  - DockerImages: `nginx-configurator/app/template.j2`.
  - KubeCoder manual: the `config.yaml` and `controller.yaml` references.
