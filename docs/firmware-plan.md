# ToiletAutomation firmware: the implementation plan

Written 2026-11-27 against `9601ceb`, the skeleton that boots to `Application: Ready` on the bench
board (WC Speaker).

## Status

A new session resumes from this section. It is updated when a phase starts, when it ends, and
when an operator check comes back.

- **P1: done** on 2026-11-27.
  - Documents: 5e5084a and 6955623. Firmware: 9ef9ab0, f4dc370 and 909852a.
  - esp-libs feac610, outside the plan, masks the Wi-Fi password in esp-mdm's boot log.
  - The operator saw the LED follow the fail-safe and passed the HA checks. WC Speaker's
    `fanMaxOnMinutes` is back at 60.
- **P2: reopened** on 2026-11-27 for a hold-off at boot. Pushed at e77c601, built green as
  Firmware/ToiletAutomation #3.
  - Firmware: 259d32f and 443d064. `MotionFilter` with its host tests, `Pir` on GPIO1, HA's
    Motion binary_sensor, and the console's `motion` and `motion inject [ms]`.
  - The bench check passed: `motion inject 2000` turned motion on, then off 2 s later. One reset
    with the PIR uncovered gave no motion at boot.
  - The operator confirmed HA's Motion follows a hand.
  - Five bench resets with the PIR covered by a cup: no motion at boot. A bench reset leaves the
    PIR powered, so a power-on trigger is untested.
  - Thirty minutes with the PIR covered, on Wi-Fi, from 14:10 to 14:40 UTC: no edges, so no
    false triggers. The defaults stand. Recorded in `docs/hardware-bring-up.md` and HLD §6.
  - The trigger at boot came back. P3's flash, at 14:41 UTC with the PIR still covered, booted
    with a pulse: high at 1228 ms, 370 ms after Wi-Fi started, for 1753 ms. It was the first boot
    with the SGP40 sampling, and one boot in six with the PIR covered. It fell before MQTT
    connected, so HA never saw it.
  - Owed, as this phase's operator check rules: the filter ignores the PIR for the first 10 s
    after boot, with host tests and a bench check.
- **P3: built** on 2026-11-27; **the operator's two checks are owed.**
  - esp-libs: 3774d6f and f2c3f71. esp-sensor-support's `SGP40` on `i2c_master`, and Sensirion's
    gas index algorithm vendored unchanged with its licence and wrapped as `VOCIndex`. It builds
    without `-Wconversion` warnings as it is.
  - Firmware: 4cf3ea7 and 4b0757c. `VocGate` with its host tests, `AirSensor`'s 1 Hz task, HA's
    `voc_index` sensor (null, so unknown, in the blackout), and the console's `voc` and
    `voc inject <index>|off`.
  - The bench check passed: serial `0x0000057f63a1` and the self-test passing at boot, the first
    index 48 s after boot, and `voc inject 250` then `voc inject off` in the log. The first index
    is 1, and the algorithm's low-pass takes it to about 90 over the next two minutes.
  - Owed by the operator: HA's VOC index showing 250 under `voc inject 250`, and isopropyl
    alcohol held near the sensor driving the index well above 100 within a minute.
- **P4 to P7: not started.**

## The order

Settled with the operator on 2026-11-27: the Home Assistant device comes first and the audio
last. The operator's words: "What you think is best."

1. **P1**: the fan relay with its fail-safe, the HA plumbing that every later entity reuses, and
   the design documents' corrections (TA-1's documents among them).
2. **P2**: motion.
3. **P3**: the SGP40 and the VOC index (changes esp-libs).
4. **P4 to P6**: `esp-audio-support` and playback (changes esp-libs). P6 closes TA-1.
5. **P7**: the first OTA.

Why this order:

- **The cheap, settled parts go first.** P1 and P2 need nothing from esp-libs. They are mostly the
  pure logic that `test/` already expects in `main/motion/` and `main/relay/`. After them, HA has a
  working fan switch and a motion sensor.
- **Audio waits on nobody, and nobody waits on it yet.** Its only sender is IntercomServer's
  ambient player, and that project has built none of it: its latest commits, on 2026-11-27,
  onboard it to the dev pipeline. Audio is also the largest piece and takes three esp-libs phases,
  so it goes where a slip costs the least.
- **Motion's open Wi-Fi risk gets measured twice.** P2 counts false triggers on an idle network,
  and P6 counts them again while a stream keeps the radio busy.

This order reverses the design's order of work, which builds audio before motion and the fan
(`docs/high-level-design.md` §11: "3. **`esp-audio-support`.** … 4. **Motion and the HA
entities.**"). P1 points §11 at this plan. Its step 6, the release wiring, is already done: the
Jenkinsfile and the IoT Support model shipped with the skeleton. Only the first OTA is left, as P7.

## Settled

- **The audio format is 48 kHz, 16-bit, mono.** The operator asked for the best quality the
  system supports. The tracks are phone AAC at 48 kHz and MP3s, and IntercomServer sends 16-bit
  only, so anything above this costs Wi-Fi and buffer and plays nothing more. The checks behind
  it:
  - **The amplifier** takes 8–96 kHz and plays one channel. It reaches its 105 dB dynamic range
    with 24- or 32-bit data. Read from DFRobot's MAX98357 page (SKU DFR0954), because analog.com
    did not answer from the pod.
  - **The ESP32-S3's I2S** takes 8-, 16-, 24- and 32-bit data (ESP-IDF 5.5's `i2s_types.h`).
  - **The speaker** is one speaker on a mono amplifier.
  - **Wi-Fi.** 48 kHz/16-bit is about 770 kbit/s, or 65 packets/s. 96 kHz/24-bit would be about
    2.3 Mbit/s and three times the packets and the buffer.
  - **IntercomServer.**
    - Its settled design decodes to "the device's `audio_formats.out` at 16-bit"
      (`docs/ambient-sound-design.md` §3.5).
    - Its code today mixes 16-bit only: `AudioUtils.MixInBuffer` throws "Only 16 bites per samples
      is supported".
    - Its sources are recorded "mono, 48 kHz, 128 kbit/s" (§4.3).
- **esp-audio-support's host tests run from this repo's `kc project test`.** esp-libs has no
  gates or CI of its own, and Intercom's gate adds the same step when it migrates.

## How every phase runs

- **Gates.** `kc project test` (the firmware build, then `test/`) and `kc project lint` are green
  before each commit.
- **The bench.** The loop, from the `kubecoder:bench` skill:
  - build;
  - `POST /flash` with `/work/ToiletAutomation/build`;
  - wait from the flash's cursor for `Application: Ready`;
  - drive the console through `POST /console`.

  The bench never erases `nvs`, so WC Speaker stays provisioned. A line marked *Operator* is one
  only a person can check: hearing, seeing the relay's LED, or HA's device page. The bench board
  has no relay fitted, and its relay output lights an LED.
- **Patterns.**
  - The owner task and queue: `main/Application.cpp` already runs esp-mdm's queue on the app task.
    Every other task hands it closures through `get_queue()`.
  - HA entities: `MQTTConnection::publish_*_discovery` in esp-mqtt-support, declared from
    `on_publish_discovery`, with all state in one JSON through `send_state(cJSON*)`. SonosDisplay
    declares no HA entities, so copy `/work/scratch/Intercom/main/Application.cpp` instead.
  - ESP-side glue (GPIO, I2C, tasks) goes in `main/board/`, as SonosDisplay's
    `main/board/MotionSensor` does. `test/` builds every file in `main/motion/`, `main/relay/`,
    `main/air/` and `main/config/` as pure logic.
- **Device configuration.** Each phase reads its own fields. A field that is missing, of the wrong
  type or out of range takes its default, so the firmware runs before IoT Support's schema has the
  field. The fields, from HLD §8:

  | Field | Range | Default | Read from |
  |---|---|---|---|
  | `fanMaxOnMinutes` | integer, 1–240 | 60 | P1 |
  | `motionMinPulseMs` | integer, 0–1500 | 0 | P2 |
  | `motionPulseCount` | integer, 1–3 | 1 | P2 |
  | `motionPulseWindowSeconds` | integer, 1–60 | 10 | P2 |
  | `volumeDefault` | number, 0–1 | 0.5 | P5 |
  | `audioUdpPort` | integer, 1024–65535 | 11106, Intercom's port | P6 |

  `motionMinPulseMs` stays under 1.7 s, the shortest real pulse
  (`docs/hardware-bring-up.md`, "The PIR").
- **Pushes.** The operator gave standing permission to push on 2026-11-27. Push at each phase's
  end, once the gates are green.
  - Jenkins builds against esp-libs at `main`. So in a phase that changes esp-libs, esp-libs is
    pushed before ToiletAutomation. The other order breaks `Firmware/ToiletAutomation`.
  - Every ToiletAutomation push also uploads a release to IoT Support. That is harmless while WC
    Speaker, with Enable OTA off, is the only device.

## The phases

### P1. The fan relay, the HA device and the design's corrections

Builds:

- The design documents, corrected before the build starts. These edits are decided and depend on
  no code:
  - **TA-1's documents** (the firmware half closes in P6).
    - `docs/high-level-design.md` §5, "What the device advertises": the configuration carries
      `"capabilities": ["pa"]`, hard-coded, which puts the device on the doorbell and other
      broadcasts. "It sends no `in`" now means only that there is no microphone. It no longer
      keeps the device out of the doorbell.
    - `docs/ambient-sound-design.md` §3.2: calls, ChatGPT conversations and the pick-up button go
      to devices with `intercom`, and the doorbell goes to devices with `pa`. A device that
      publishes no capabilities keeps the microphone rule. `Doorbell:ExtraDevices` is gone.
    - §3.6: the Ambient sound switch is published for every device that advertises
      `audio_formats.out`, and `Ambient:TopicPrefixes` is gone. Both settings also leave §3.9.
  - **The relay.**
    - `docs/high-level-design.md` §2 and `docs/hardware-bring-up.md` ("Open hardware point: the
      relay during reset") say what the schematic has: a 10 kΩ pull-down, R2, added at `744a522`.
    - The bench board has no relay fitted. Its relay output lights an LED.
    - The HLD's status line names the schematic at `744a522`.
  - **The audio format.** HLD §5 and Q2 record 48 kHz, 16-bit, mono as settled, with this plan's
    reason.
  - **The order.** HLD §11 points at this plan.

- `main/relay/RelayGuard.{h,cpp}`, pure logic:
  - it starts off;
  - it takes on and off commands, each with the time;
  - `tick(now)` turns the relay off once it has been on for `fanMaxOnMinutes`, counted from the
    change from off to on;
  - an on command while it is on does not restart the count. HA turns it on again after the relay
    reports off (HLD §7).
- `main/config/DeviceSettings.{h,cpp}`, pure logic: the table's fields with their defaults and
  ranges. `Application::do_configuration_loaded` copies the cJSON values into it. Add
  `main/config` to `PURE_DIRS` in `test/CMakeLists.txt`.
- `main/board/Relay.{h,cpp}`: drives `RELAY_GPIO` as input and output, so the level reads back, as
  `hwtest`'s `relay_init` does. `drive_relay_off()` in `main.cpp` stays the first thing at boot.
- HA: the `fan` switch and the `restart` button. `Application` owns the guard on the app task and
  ticks it from `do_process`.
- Console: `relay [on|off]` goes through the guard, so the fail-safe also covers a forced relay. It
  prints the state and the on-time left.

Host tests:

- `test/RelayGuardTest.cpp`:
  - off at start;
  - on, then off;
  - off at exactly the maximum, and not a millisecond earlier;
  - an on command while on keeps the deadline;
  - on again after the fail-safe starts a new count.
- `test/DeviceSettingsTest.cpp`: fields that are missing, of the wrong type or out of range take
  the default.
- `SmokeTest.cpp` is deleted.

Operator stop: in IoT Support's model editor, add the table's six fields to the `toilet_automation`
model's configuration schema, and set WC Speaker's `fanMaxOnMinutes` to 1. The schema is database
data, not a file in any repo.

Bench check:

- At boot, `main: Relay driven off` comes before esp-mdm's first line, and `Application: Ready`
  follows.
- `relay on` logs the relay on, with the pin reading 1. About 60 s later the log shows it off, with
  the maximum on-time as the reason.
- *Operator:* the relay output's LED lights on `relay on` and goes out when the fail-safe trips.
- *Operator:* WC Speaker's device page in HA has Fan and Restart.
  - Fan follows `relay on`.
  - Switching Fan in HA shows in the log and on the LED.
  - Restart reboots the board.

Done when:

- the gates are green;
- the serial log shows the relay turning itself off after 1 minute;
- HA shows the switch going off, and the LED goes out;
- the design documents carry the corrections above.

### P2. Motion

Builds:

- `main/motion/MotionFilter.{h,cpp}`, pure logic:
  - it takes PIR edges with their times, and `tick(now)`;
  - it puts out motion on and off;
  - a pulse counts once it has been high for `motionMinPulseMs` and is at least the
    `motionPulseCount`-th such pulse within `motionPulseWindowSeconds`. Either rule alone is the
    other left at its default, as HLD §6 asks. Built as both, the stricter reading;
  - motion goes off when the PIR goes low;
  - it counts edges, accepted pulses and rejected pulses.
- `main/board/Pir.{h,cpp}`: `PIR_GPIO` as an input, with the pull-down and an any-edge interrupt,
  as `hwtest`'s `pir_init` does. The ISR queues the level and the time. The app task drains the
  queue and feeds the filter.
- HA: the `motion` binary_sensor, with device class motion.
- Console:
  - `motion` prints the level and the counters;
  - `motion inject [ms]` feeds the filter a pulse of that length.

Host tests: `test/MotionFilterTest.cpp`:

- a 1.7 s pulse passes at the defaults;
- a pulse shorter than the minimum is rejected;
- the two-pulse mode, with the second pulse inside and outside the window;
- a retrigger keeps motion on;
- motion goes off when the PIR falls.

Bench check:

- `motion inject 2000` logs motion on, then off 2 s later.
- *Operator:* a hand in front of the PIR shows in the log and in HA's Motion.
- *Operator:* five resets with the PIR covered by a cup. Count the motion at boot, the bring-up's
  unsettled "trigger at boot". If it comes back, the filter ignores the PIR for that long after
  boot, in this phase.
- *Operator:* thirty minutes with the PIR covered, on Wi-Fi. Every edge is a false trigger.
  - Record the count in the PIR section of `docs/hardware-bring-up.md`.
  - If there are false triggers, set defaults that reject them, with the minimum kept under 1.7 s.

Done when: the gates are green, HA's Motion follows a hand, and both counts with the PIR covered
are recorded.

### P3. Air: the SGP40 and the VOC index (changes esp-libs)

Builds:

- In `esp-libs/esp-sensor-support`:
  - An `SGP40` class on the `i2c_master` API, which owns its bus as `SEN0626` does. It reads the
    serial number, runs the self-test, and takes the raw measurement at the default compensation.
    The commands and the CRC come from `hwtest`'s `sgp40_*` functions.
  - Sensirion's gas index algorithm (BSD-3-Clause, `Sensirion/gas-index-algorithm`), vendored with
    its licence and wrapped as `VOCIndex`.
  - It compiles into every firmware that uses the component, so it has to build without
    `-Wconversion` warnings, through fixes or a per-file flag on the vendored file.
- `main/air/VocGate.{h,cpp}`, pure logic:
  - it publishes nothing while the algorithm is in its blackout;
  - after that, it publishes on a change of the integer index, at most once every 10 s.

  Add `main/air` to `PURE_DIRS`.
- `main/board/AirSensor.{h,cpp}`: a 1 Hz sampling task that hands each index to the app task.
  `main/idf_component.yml` gains `esp-sensor-support`.
- HA: the `voc_index` sensor, with state class measurement.
- Console:
  - `voc` prints the raw value, the index and the blackout;
  - `voc inject <index>|off` overrides the published index, so that HA's fan automation can be
    exercised.

Host tests: `test/VocGateTest.cpp`:

- nothing during the blackout;
- the first index after it;
- the rate limit;
- the override and its release.

Bench check:

- At boot the log shows serial `0x0000057f63a1` and the self-test passing. After the blackout it
  shows the first index.
- `voc inject 250` puts 250 in HA, and `voc inject off` brings back the measured index.
- *Operator:* isopropyl alcohol held near the sensor drives the index well above 100 within a
  minute.

Push point: esp-libs, then ToiletAutomation.

Done when: the gates are green, and HA shows the index and its reaction to the isopropyl alcohol.

### P4. `esp-audio-support`: the stream buffer (changes esp-libs: a new component)

Builds:

- `esp-libs/esp-audio-support/`, laid out like the other components: `CMakeLists.txt`,
  `idf_component.yml` on `esp-support`, and `README.md` with the config struct. Its
  `sdkconfig.defaults` sets `CONFIG_LWIP_UDP_RECVMBOX_SIZE=32` (Intercom's finding H5).
- The pure core, with no ESP-IDF include, from Intercom's `AudioMixer` and `AutoVolume` and with
  HLD §5's changes:
  - `StreamBuffer`:
    - places each packet by its 4-byte big-endian counter;
    - plays a gap as silence (finding H2);
    - starts a new stream on a jump bigger than the depth, in either direction;
    - starts the configured lead ahead of the read position;
    - ends the stream when it runs dry;
    - keeps Intercom's mixing of several sources, keyed by an opaque source id, so that Intercom
      can move onto it;
    - counts packets, gaps filled, late or duplicate drops, new streams and underruns.
  - The sample rate and the depth are parameters. The depth holds at least IntercomServer's
    500 ms prebuffer, and the default is 1 s.
  - Gain maps the volume (0–1) to dB, with 0 as mute. `AutoVolume` is an optional stage.
  - `ToneSource`: sine bursts as a local source, for the console and Identify.
- `esp-libs/esp-audio-support/test/`: a GoogleTest build on the pattern of this repo's
  `test/CMakeLists.txt`, with the check on pure includes. This repo's `.kubecoder/project.yaml`
  gains a test step that runs it, as settled.

Host tests:

- packets in order;
- a gap gives silence of its length;
- packets reordered within the depth;
- a duplicate is dropped;
- a forward jump and a backward jump each start a new stream (IntercomServer's counter restarts
  at 0);
- the stream ends when it runs dry;
- two sources mix with saturation;
- the gain's endpoints;
- the tone's frequency and length.

Bench check: none, because nothing runs on the device yet.

Done when: `kc project test` runs the component's tests, and they are green.

### P5. Playback: I2S out, the volume and Identify (changes esp-libs)

Builds:

- In the component, `I2SOutput`, from Intercom's `I2SPlaybackDevice`:
  - TX only, on the `i2s_std` driver, in its own task;
  - a preload, then a blocking `i2s_channel_write` sets the pace;
  - the mono stream goes out in the left slot;
  - Philips format, which `hwtest`'s `speaker_init` proved on this board. Intercom uses MSB, so the
    format is in the config struct;
  - a short grace fed with zeros before a dry buffer stops the channel (finding H3);
  - the played-samples tap: the samples, their time and a stream reset. It replaces Intercom's
    `feed_reference_samples` and `reset_feed_buffer`.
- In the component, `AudioPlayer`, which takes the config struct: the pins, the I2S port, the
  format, the depth, the UDP port, and whether the buffer is in PSRAM or internal RAM.
- In this repo, `main/audio/`, the ESP-side glue (not pure):
  - It builds the player from `BoardConfig.h` and the settings.
  - The `volume` number (0–1) is kept in NVS, through esp-support's `NVSProperty` or SonosDisplay's
    raw-NVS helpers. `volumeDefault` applies while NVS has no value.
  - The `identify` button plays two short beeps.
  - The console gets `tone [hz] [ms] [level]`.
- The parse and clamp of the HA number's payload is pure logic, in `main/config/`.

Host tests: `test/VolumeTest.cpp` covers the parse and clamp of the HA payload.

Bench check:

- `tone 440 1000` logs the tone and no underrun.
- *Operator:* hears the tone.
- *Operator:* HA's Volume at 0.2 and at 1.0 sounds different, and the setting survives
  `POST /reset`.
- *Operator:* Identify in HA beeps twice.

Done when: the gates are green, and the operator has heard the tone at two volumes and Identify.

### P6. The stream: UDP in and the advertisement (changes esp-libs; closes TA-1)

Builds:

- In the component:
  - `UDPReceiver`, from Intercom's `UDPServer`, at a higher priority than the I2S task (finding
    H5). It feeds the `StreamBuffer`. Playback starts on the first packet and stops when the
    buffer runs dry.
  - Wi-Fi modem power save goes off (`esp_wifi_set_ps(WIFI_PS_NONE)`, finding H1) when the
    receiver starts. HLD §5 puts this in the extraction, and esp-network-support has no setting
    for it.
- In this repo:
  - `on_create_configuration` adds these fields, in the shape of Intercom's `main/Application.cpp`:
    - `endpoint` (`ip:port`);
    - `audio_formats.out`: `mono`, a `sample_rate` of 48000, and a `bit_rate` of 16, which means
      bits per sample;
    - `"capabilities": ["pa"]`, hard-coded with no setting. This is TA-1's firmware half, and it
      puts the device on the doorbell once IntercomServer decides by capability (ICOMSRV-8).

    There is no `in`.
  - The top `CMakeLists.txt` reads the component's `sdkconfig.defaults`.
  - The console gets `audio`, which prints the counters and how full the buffer is.
- `tools/send-audio.py`, on the Python standard library only:
  - it streams a mono 16-bit WAV at the advertised rate, in the wire format;
  - 1468-byte payloads, paced at 20 ms after a 500 ms prebuffer (ambient-sound-design §3.5);
  - `--drop` and `--reorder` exercise the gap fill.

Host tests: none new, because P4's tests cover the logic.

Bench check:

- The pod reaches the board. Witnessed on 2026-11-27: `ping 10.1.1.20` from the pod, with 0 % loss
  at 67–111 ms. That proves the route for HLD Q6, and the stream below proves UDP. After this phase
  the round trip falls to a few ms, which shows that power save is off.
- The log shows the advertised endpoint, format and capabilities.
- `send-audio.py --host 10.1.1.20 --drop 0.01`:
  - the log shows a stream started;
  - `audio` shows gaps filled to match the drops, and no underruns;
  - the stream ends once the file and the buffer have played.
- `heap`, before and during a stream, shows the internal RAM headroom.
- *Operator:* the stream sounds clean.
- *Operator:* P2's count with the PIR covered, run again for 10 minutes during a stream.

Push point: esp-libs (P4 to P6), then ToiletAutomation.

Done when:

- the gates are green;
- a 5-minute stream with 1 % loss plays with no underrun;
- the operator has heard it;
- the PIR count during streaming is recorded;
- the configuration carries `capabilities`, which closes TA-1.

### P7. The first OTA (no code)

1. The operator turns Enable OTA on for WC Speaker in IoT Support.
2. `POST /reset`. The log shows esp-mdm installing the model's current release, and `version`
   shows its hash.
3. The operator turns Enable OTA off again, and a bench flash restores the dev build.

Done when: the log shows that install (HLD §11, step 6).

After this plan, outside the firmware:

- HA's occupancy, fan and ambient automations (HLD §6 and §7, ambient-sound-design §3.6);
- the end-to-end run with IntercomServer, once its ambient player is deployed
  (ambient-sound-design §7, step 7).

## Questions for the operator

None open. The operator answered the first four on 2026-11-27, and they are under Settled
above and in P1.
