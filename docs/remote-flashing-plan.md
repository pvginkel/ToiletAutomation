# The bench: build plan

The bench is built in a repo of its own, proven on the real board from this environment, deployed
as a KubeCoder catalog service, and then its repo leaves this environment. The firmware work
starts only after that. The spikes come first and live in this repo.

Written 2026-11-22 for the session that builds it. What the bench is, and why, is in
[remote-flashing-design.md](remote-flashing-design.md). This document says what to do, in what
order, and when each step is done.

## 1. What the operator decides or does

Three choices, each with the recommendation the rest of this plan assumes:

- **The repo's name, and who creates it.** Recommended: `pvginkel/FirmwareBench`, created by the
  operator. The service itself is called `bench` in the catalog.
- **The stack.** Recommended: TypeScript throughout, a Node server and a Vite-built page, on the
  `frontend` toolchain. The page has to be TypeScript because esptool-js is, and one language
  lets the page, the server and the tests share the message types. The price is a Node image of
  about 256Mi where `pilot-sync`, which is Go, takes 128Mi.
- **Where these two documents live.** Recommended: move both into the bench's repo with its
  first commit, and leave one line in this repo's high-level design that points there.

Three things only the operator can do:

- **Ten minutes at the desk for the spikes** (step 1), and about fifteen for acceptance (step 5).
- **Promote KubeCoderDeploy to prd** (step 4). That job is run by hand.
- **Say whether KubeCoder checks a repo's `ports:` entry against its own containers.** If it
  accepts any port number, nothing is open on the HTTPS route (design §3.4).

## 2. The order

| Step | What | Done when |
|---|---|---|
| 1 | Spikes S1 to S3, in this repo | the three answers are written into design §9 |
| 2 | The bench: messages, server, page, tests | its suite is green with a scripted holder standing in for the browser |
| 3 | The bench as a dev process in this environment | an agent flashes the real board and reads its log |
| 4 | Image, catalog entry, deploy | `kc catalog` lists `bench` |
| 5 | This environment on the catalog service | the acceptance list passes and the repo is gone from this environment |

A failed spike changes step 2, not the order:

- **S1 fails** (the port grant does not survive the reset): the holder becomes a small desktop
  helper instead of the page. The server and the agent API are unchanged.
- **S3 fails** (no WebSocket through the edge): the transport becomes SSE for jobs plus POSTs for
  results and log data (design §7).

## 3. Step 1: the spikes

They live under `spikes/bench/` in this repo and are deleted in step 5.

- **This environment's `.kubecoder/config.yaml` gains** the `esp-idf` toolchain, to build a
  firmware to flash, and one port:

  ```yaml
  ports:
    - name: bench
      port: 3520
      type: http
      sslOffload: true
  ```

  The page is then at `https://bench.<env-id>.home`. Keep this name and number for the whole
  build. The browser's port grant belongs to the origin, so it carries over from the spike to
  the dev process to the deployed service.
- **The page** is one static HTML file that loads esptool-js 0.7.1, with the serial work in a
  dedicated worker. Something minimal serves it on port 3520 and echoes a WebSocket.
- **The firmware** is ESP-IDF's own `hello_world` example, built out of tree for `esp32s3`, plus
  a variant that calls `abort()` a few seconds after boot.
- **What the operator does:** open the page, pick the port, press the buttons the page offers,
  and read back what it shows. The questions are in design §9.

Record the answers in design §9: yes or no for each, the seconds re-enumeration takes, and the
idle timeout of a WebSocket through the edge.

## 4. Step 2: the bench

The repo is onboarded with `/kubecoder:onboard`, so that `kc project setup`, `build`, `test` and
`lint` work from a fresh clone. It is added to this environment through `repos:` in
`.kubecoder/config.yaml`, with the `frontend` toolchain.

### 4.1 The parts

- **The messages between holder and server.** One versioned set of types, used by the server,
  the page and the tests. A sketch:
  - Holder to server: hello (with the protocol version), port state (open, lost, closed, with
    chip and MAC once known), log bytes, job progress, job result.
  - Server to holder: a job (flash, reset or write a line), ping, and a refusal when another
    holder is already connected.
- **The server.** Two listeners:
  - The page port, `3520` on all interfaces: the static page, the WebSocket, and the images of
    the current job. Image URLs carry the job's random id.
  - The agent API, `3521` on `127.0.0.1` only.
- **The page.** It asks for the port once and shows status. A dedicated worker does the rest:
  - It reopens the granted port on load and on a `connect` event, without a click.
  - It runs the monitor between jobs and esptool-js during a flash.
  - It reconnects to the server by itself, with back-off, when the WebSocket drops.
- **The log writer.** `/work/.bench/serial.log`, one timestamped line per device line, colour
  codes stripped, rotated by size. Flash start and end, reset, port lost and port reopened are
  written as marker lines.

Neither port number is used by a catalog service today (`kc catalog`, 2026-11-22). `pilot-sync`
has 3510.

### 4.2 The agent API

A sketch for the builder to finish. Every failure is a non-2xx status with a JSON body that
carries an error code, so `curl -f` fails on it. The body is read as JSON whatever content type
curl sends.

| Call | What it does | What it answers |
|---|---|---|
| `POST /flash` with `build_dir` | flashes the build and blocks until the device has been reset | chip, MAC, seconds taken, and the log cursor at the start of the flash |
| `GET /status` | nothing | holder connected, port open, chip, MAC, current job |
| `GET /log` with `since`, `wait`, `timeout` | blocks until a line after the cursor matches | the lines, the match, the new cursor |
| `POST /reset` | hard-resets the device and waits for the port to return | the log cursor at the reset |
| `POST /console` with a line | writes the line to the device | nothing |

The error codes: no holder connected, port not open, busy, bad build directory, wrong chip, flash
failed (with esptool's own message), timed out.

### 4.3 A flash, step by step

1. The server checks the build directory: it is under `/work` and holds `flasher_args.json` and
   every image that file names. It reads the chip and the flash settings from it.
2. It refuses at once when no holder is connected, the port is not open, or a job is running.
3. The holder stops the monitor, connects with esptool-js, and compares the chip. A mismatch
   ends the job before anything is written.
4. It writes each image at its own offset, compressed, and checks each MD5.
5. It hard-resets the device, waits for the port to come back, reopens it and resumes the
   monitor.
6. It reports the result. The server answers the blocked call and writes the markers.

Two rules hold throughout: only the images in `flasher_args.json` are written, and nothing is
ever erased as a whole. That is what keeps the provisioned `nvs` partition safe.

IoT Support's `frontend/src/hooks/use-flash-device.ts` is the working reference for steps 3 and
4, on esptool-js 0.5.7. Its reset workaround has to be checked against 0.7.1 (design §3.3).

### 4.4 Tests

- **Server, against a scripted holder.** A test client speaks the holder's side of the messages.
  Cases:
  - a flash that succeeds, one that fails, and one that times out;
  - the holder dropping in the middle of a job;
  - no holder, a second holder, and a second flash while one runs;
  - a build directory outside `/work`, one with an image missing, and a wrong chip;
  - the log cursor, wait-for-pattern, its timeout, and rotation;
  - that a flash job names exactly the images of `flasher_args.json`.
- **Page, against a fake serial port.** The reconnect logic: port lost and returned, the same
  port object or a new one, the server connection lost and returned.
- **Not covered by a suite:** esptool-js against a real chip. That is steps 3 and 5.

## 5. Step 3: on the real board, as a dev process

The server runs in the `frontend` tool container through `cexec`, on the same two ports. It is
unsupervised and dies on a restart, which is fine here.

Done when an agent in this environment, with the operator's tab open:

- flashes `hello_world` with one call and gets the result;
- waits for a line of its output through `/log`, and finds the same line in the log file;
- resets the device and sees it boot again.

## 6. Step 4: image, catalog entry, deploy

- **The image.** A Dockerfile and a Jenkinsfile in the bench's repo. The Jenkinsfile follows the
  `kubecoder:jenkins-pipelines` skill and pushes with `helmCharts.kaniko2`, as PilotApp's does
  for `pilot-sync`. Catalog images are named `registry:5000/<name>:<tag>`.
- **Pinned or floating tag.** Both exist in the catalog: `pilot-sync` floats on a tracking tag,
  and the toolchains pin. Take whichever the pipelines guide prescribes for a new image.
- **The catalog entry.** Under `controllerConfig.services` in KubeCoderDeploy's
  `chart/values.yaml`, modelled on `pilot-sync`:
  - `description` and `instructions`;
  - `container` with the image and a memory limit;
  - `mountWork: true`;
  - no `dataPath`, because the log is under `/work`;
  - no `securityContext`, so the log file belongs to the pod's user and the agent can read it.
- **The `instructions`.** They are what an agent sees in `kc env describe`, so they are the
  bench's manual. They carry:
  - the `ports:` entry a repo has to add, verbatim, and that the service is selected by its bare
    name, without `expose:`;
  - the page's URL, and that the operator opens it once;
  - the API calls with a curl line each, and the error codes;
  - the log file's path;
  - how to decode a backtrace in the `esp-idf` toolchain.
- **Rollout.** Argo CD syncs dev from KubeCoderDeploy's `main` and prd from its `prd` branch.
  The promotion to `prd` is the operator's. This is read from the repo's README, not tried.

## 7. Step 5: switch over, accept, clean up

This environment's `.kubecoder/config.yaml` selects the service and keeps the port entry:

```yaml
services:
  - bench
```

Acceptance, on the real board, through the catalog service:

1. The page opens and connects after one click. A reload of the page reconnects with none.
2. `/status` shows the chip and the MAC.
3. A flash of `hello_world` succeeds, and the log shows its boot.
4. The same flash succeeds after the tab has been hidden for more than five minutes.
5. `/reset` reboots the device, and the port comes back without a click.
6. A line sent with `/console` reaches a firmware that has a console (ESP-IDF's console example).
7. The firmware that aborts leaves its backtrace in the log, and `addr2line` decodes it.
8. A build for another chip is refused and nothing is written.
9. With the device unplugged, a flash is refused as "port not open". Plugged back in, it works
   without a click.
10. With the tab closed, a flash is refused as "no holder". A second tab is turned away.
11. After `kc env restart`, the page reconnects by itself and the log continues.

Then clean up:

- Delete `spikes/bench/` from this repo.
- Drop the bench's entry from `repos:` and the `frontend` toolchain from this environment.
  Removing the entry leaves the checkout on disk, so delete `/work/<repo>` by hand, after
  checking that everything in it is pushed.
- Move the documents, if that was the choice in §1, and update the link in the high-level
  design.

What stays in this environment is the `bench` service, the port entry and the `esp-idf`
toolchain. That is where the firmware work starts.
