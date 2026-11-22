# ToiletAutomation: high-level design

A mains-powered device in the toilet. It plays ambient sound while someone is in there, and it
switches the extractor fan. Home Assistant decides when either happens. IoT Support (the MDM)
manages the device through `esp-libs`' esp-mdm: provisioning, device configuration, OTA, log
shipping and coredumps. Home Assistant sees it over MQTT, the same way it sees the other devices.
ESP-IDF 5.5.3, C++20.

Status: second draft, 2026-11-22. The device now reports and Home Assistant decides. The sound
comes from IntercomServer, as [ambient-sound-design.md](ambient-sound-design.md) designs. The
hardware is the schematic at `51c1611`, before the relay was added. How the device is flashed and
tested from a KubeCoder environment is in
[remote-flashing-design.md](https://github.com/pvginkel/FirmwareBench/blob/main/docs/remote-flashing-design.md).

## 1. The decisions

1. **Built the way SonosDisplay is built.** The app is an esp-mdm `ApplicationBase` subclass on
   `esp-libs` path dependencies, with one owner task and a queue for the other tasks. The CMake
   project name is the IoT Support model code: `toilet_automation`.
2. **Audio is a UDP sink, as in Intercom, and IntercomServer feeds it.** The device opens a UDP
   port and plays whatever PCM arrives there. It never decides what to play or when. It
   advertises its endpoint and format the way an intercom does (§5).
3. **The audio code moves into esp-libs, not into this repo.** A new component,
   `esp-audio-support`, is extracted from Intercom's playback path and generalised (§5). This
   project is its first consumer. Intercom moves onto it later, as a separate change.
4. **The device reports, and Home Assistant decides.** The device is a set of HA MQTT entities:
   sensors (motion, the VOC index) and actuators (the fan relay, the volume). Occupancy, the
   fan's logic and when the sound plays are HA automations. The device and IntercomServer never
   talk about presence.
5. **The device keeps only fail-safes.** The relay is off at boot, and it turns itself off after
   a maximum on-time, so a dropped network or a stopped HA cannot leave the fan running. This
   reverses the first draft, which ran the fan loop on the device: the toilet is no longer
   ventilated while HA or the network is down.
6. **The logic is kept testable on the host.** The motion filter, the relay's fail-safe and the
   jitter-buffer logic stay free of ESP-IDF includes, so `kc project test` runs them natively,
   the way SonosDisplay's `sim/` runs its UI.
7. **Releases go through Jenkins.** A push to `main` builds and uploads to IoT Support, which
   pushes it over the air (§8). Dev builds never go through the MDM. They are flashed through the
   remote bench.

## 2. Hardware

| Part | What it is | Interface |
|---|---|---|
| Waveshare ESP32-S3-Tiny | ESP32-S3 SiP board. The variant (S3FH4R2: 4 MB flash, 2 MB PSRAM, or N8R8) is still to be confirmed. USB-C is native USB (USB-Serial-JTAG, no UART bridge), expected but to be verified on the board | USB, BOOT and RESET buttons |
| DFRobot MAX98357 | I2S mono class-D amplifier | `SPEAKER_SCK`, `SPEAKER_WS`, `SPEAKER_DATA` |
| HC-SR602 | Mini PIR motion sensor with a digital output | `PIR_OUT` |
| DFRobot SEN0394 | Sensirion SGP40 VOC sensor, I2C at 0x59 | `AIRQ_SCL`, `AIRQ_SDA` |
| HLK-5M05 | 5 V AC-DC supply, with a 1000 µF bulk capacitor on 5 V | mains via the screw terminal |
| **Relay (to add)** | Switches the fan | one GPIO through a driver |

The pin numbers are read off the schematic into `main/board/BoardConfig.h` when it is written. Two
hardware points to settle before the relay goes in:

- **The relay switches mains.** That needs the fan's rating, a driver transistor and flyback diode
  (or a relay module that has them), creepage between the mains and the 3V3 side, and a fuse.
- **The relay comes up off.** The boot and reset state of the GPIO chosen for it must keep the
  relay off. Some S3 GPIOs are straps or glitch at reset.

## 3. System context

```
                        MQTT (state, commands, HA discovery)
   Home Assistant  <------------------------------------------+
   (occupancy, fan and sound automations)                     |
                                                              |
   IoT Support (MDM) <-- HTTPS: config, OTA, coredumps -->  [ ToiletAutomation ]
                     <-- MQTT: logsink, update notices --> |   ESP32-S3         |
                                                           |                    |
   IntercomServer ---- UDP: 4-byte seq + PCM ------------> |  I2S -> MAX98357 -> speaker
   (reads the device's configuration over MQTT)            |  PIR (GPIO)
                                                           |  SGP40 (I2C)
                                                           |  relay -> fan
```

IntercomServer reads the device's endpoint and audio format from the device's retained MQTT
configuration. It streams to the device while HA has the device's "Ambient sound" switch on,
which IntercomServer publishes. All of that is in
[ambient-sound-design.md](ambient-sound-design.md).

## 4. Firmware structure

Laid out like SonosDisplay:

```
main/
  Application.{h,cpp}     esp-mdm ApplicationBase subclass; owns the app task and its queue,
                          the device configuration, and the MQTT/HA entities
  board/BoardConfig.h     every pin and timing
  motion/                 MotionFilter: PIR edges in, filtered motion out (pure logic)
  air/                    VOC index pipeline: SGP40 reads -> Sensirion VOC index algorithm
  relay/                  RelayGuard: commands and time in, relay state out, with the
                          maximum on-time (pure logic)
  audio/                  thin glue onto esp-audio-support: port, format, volume, and the
                          endpoint and format in the MQTT configuration
test/                     host tests of motion/, relay/ and config parsing (native toolchain)
```

The rule that keeps `test/` honest is the same one SonosDisplay's `sim/` relies on. `motion/`,
`relay/` and the pure half of `air/` include only the C++ standard library. Time comes in as a
parameter, never from `esp_timer`. The test build fails on an ESP-IDF include.

## 5. Audio

**Starting point: Intercom's playback path.** The relevant files are `UDPServer`, `AudioMixer`,
`I2SPlaybackDevice` and `AutoVolume`.

- **Wire format.** A 4-byte big-endian sequence counter, then raw PCM, up to 1472 bytes per
  datagram.
- **Jitter buffer.** Each source starts 200 ms ahead of the read position.
- **I2S.** New `i2s_std` driver, TX only, 16-bit mono at 16 kHz.
- **Pacing.** A blocking `i2s_channel_write` sets the pace.
- **End of stream.** Playback stops when the buffer runs dry.

**What changes in the extraction to `esp-audio-support`:**

- **Sample rate is a parameter.** Intercom `#error`s on anything but 16 kHz. Music wants 44.1 or
  48 kHz. The MAX98357 is mono, so the stream is mono, which comes to about 770 kbit/s at
  48 kHz/16-bit, or about 65 packets/s. The format is a decision (Q2).
- **Lost packets become silence.** Intercom appends packets back to back and ignores gaps, so
  every lost packet permanently shortens the buffer (finding H2 in Intercom's
  `docs/audio-pipeline-review/`). The component uses the sequence number to fill gaps with
  silence.
- **A large jump starts a new stream.** A jump in the counter bigger than the buffer's depth,
  in either direction, starts a new stream instead of being filled. IntercomServer's counter
  restarts at zero when the server restarts, and filling that jump would play minutes of
  silence.
- **A deeper buffer.** Music is not latency-sensitive. 500 ms to 1 s is cheap: 48 kHz mono is
  96 KB per second. It can live in PSRAM if internal RAM gets tight. The buffer holds at least
  IntercomServer's prebuffer (500 ms), which the server keeps ahead of real time.
- **WiFi modem power save off.** This is Intercom's H1 finding.
- **Config struct, not `board_config.h` macros.** It carries pins, I2S port, format, buffer depth
  and UDP port.
- **A played-samples tap.** It is a callback, so Intercom can feed its AEC reference from it when
  it migrates. That coupling is the only thing tying Intercom's playback to its microphone path.
- **AutoVolume stays optional.** Volume is a 0–1 setting mapped to a dB range, persisted in NVS
  and exposed as an HA `number`. IntercomServer evens out the loudness between tracks, and the
  volume sets the listening level.

**What the device advertises.** The retained MQTT configuration carries `endpoint` (`ip:port`)
and `audio_formats.out` (`channel_layout`, `sample_rate`, `bit_rate`), in Intercom's shape,
through esp-mqtt-support's `on_create_configuration`, under
`CONFIG_MQTT_TOPIC_PREFIX="toilet_automation"`.

- **It sends no `in`, since there is no microphone.** That is also what keeps the device out of
  IntercomServer's calls and doorbell.
- **One format is enough.** IntercomServer converts to whatever the device advertises.

**Kept out of esp-libs:** Intercom's recording/AFE path, controls, LEDs and `Device`/MQTT code.
Intercom's `support.{h,cpp}` duplicates `esp-support` macros, so the extracted code uses
`esp-support` instead.

**Intercom keeps its own copy until it migrates.** The migration changes Intercom's behaviour
(gap fill), so it is tested on the intercoms in a change of its own. `esp-libs` is shared and
consumed at `main` by every firmware's build. A new component is purely additive, so adding it
cannot break SonosDisplay or Intercom.

## 6. Motion and presence

- **What the sensor gives.** The HC-SR602 drives `PIR_OUT` high on motion for a short, fixed,
  retriggerable window. The datasheet value still needs checking.
- **What the firmware makes of it.** `MotionFilter` turns the pulses it accepts into `motion`, an
  HA `binary_sensor` (motion) that is on while the PIR reports motion.
- **Occupancy is HA's.** A template binary sensor holds on for a few minutes after the last
  motion, and the sound and fan automations build on it:

  ```yaml
  template:
    - binary_sensor:
        - name: Toilet occupancy
          device_class: occupancy
          state: "{{ is_state('binary_sensor.toilet_motion', 'on') }}"
          delay_off:
            minutes: 3
  ```

- **Risk: someone sitting still.** A person sitting still produces little motion, so occupancy
  can drop mid-visit and the sound stops. The hold time is the lever, and it is HA's `delay_off`.
  A door contact would settle it properly, but it is not on the schematic.
- **Risk: false triggers from WiFi.** Small PIRs are widely reported to false-trigger from the
  ESP's own WiFi bursts when mounted close to the antenna. The mitigations are distance on the
  PCB, and requiring a pulse of minimum length or two pulses within a window. That is the motion
  filter's job, on the device, because it needs the pulse timing. Its configuration allows
  either.

## 7. Fan

- **VOC index.** The SGP40 gives raw ticks. Sensirion's VOC Index algorithm (BSD-3-Clause,
  sampled at 1 Hz) turns them into an index from 0 to 500, where 100 is the learned baseline.
  The index is relative and adaptive, which suits detecting "the air just got worse".
- **What that means in practice.** It has a start-up blackout and a learning period. The device
  publishes no index until the blackout has passed, so an HA automation never acts on it. There
  is no humidity sensor, so the algorithm runs with its default compensation.
- **The relay is an HA `switch`.** An HA automation turns it on above an on-threshold and off
  below an off-threshold after a hold. It can add an after-run once occupancy ends. A sketch,
  with the thresholds to be tuned:

  ```yaml
  alias: Toilet fan
  triggers:
    - trigger: numeric_state
      entity_id: sensor.toilet_voc_index
      above: 200
      for: { minutes: 1 }
      id: "on"
    - trigger: numeric_state
      entity_id: sensor.toilet_voc_index
      below: 150
      for: { minutes: 5 }
      id: "off"
  actions:
    - action: "switch.turn_{{ trigger.id }}"
      target:
        entity_id: switch.toilet_fan
  ```

- **The device's fail-safe.** The relay turns itself off after a maximum on-time (device
  configuration, 60 minutes by default) and reports off. HA turns it on again if it still wants
  it. A stuck-high index, a hung automation, or a network that drops with the relay on cannot
  run the fan forever.
- **Safe states.** The relay is off at boot. Its state is not persisted.
- **Home Assistant** sees the fan as a `switch` and the VOC index as a `sensor`.
- **Where the drivers go.** The SGP40 driver and the VOC algorithm go into `esp-sensor-support`,
  beside `SEN0626`. The PIR is a GPIO interrupt and stays in this app.

## 8. Integration and release

- **IoT Support.** Model `toilet_automation`, created before the first upload. Its configuration
  schema carries:
  - `deviceName`, `deviceEntityId` and `enableOTA`;
  - the motion filter's settings;
  - the relay's maximum on-time;
  - the volume default and the UDP port.
- **Home Assistant.** Discovery through `esp-mqtt-support` covers motion, the VOC index, the fan,
  the volume, identify and restart, plus esp-mdm's own diagnostics. IntercomServer adds the
  Ambient sound switch and the Ambient track sensor to the same HA device.
- **Flash layout.** 4 MB, assuming the FH4R2: `nvs`, `otadata`, `phy_init`, two OTA slots of
  about 1.9 MB, and a `coredump` partition. esp-mdm turns on coredump-to-flash and uploads the
  dump on the next boot. That needs the `coredump` partition, which Intercom's table lacks. An
  image-size check in the build keeps the app under the slot size, as SonosDisplay's does.
- **Release.** Jenkins `Firmware/ToiletAutomation` checks out the repo and `esp-libs`, builds,
  and uploads to IoT Support. That sets the model's current version and notifies the device. It is
  the same `espFirmware.build`/`upload` pair as SonosDisplay and Intercom.
- **Initial flash and provisioning.** Done once from IoT Support's Flash device and Provision
  device buttons. Provisioning writes the `nvs` partition with credentials and addresses. Nothing
  in the dev loop may erase it.

## 9. Testing

| Layer | What it proves | How |
|---|---|---|
| Host tests | the motion filter, the relay's fail-safe, the jitter-buffer logic, config parsing | `kc project test` on the `native` toolchain; `esp-audio-support` carries its own host tests for the mixer |
| Build and lint | it compiles, fits the slot, is formatted | `kc project build` (`esp-idf`), `kc project lint` |
| On-device, agent-driven | it boots, connects, reaches ready, and the logic does what it should on the real chip | the remote bench: flash, read the serial log, reset, send console commands ([remote-flashing-design.md](https://github.com/pvginkel/FirmwareBench/blob/main/docs/remote-flashing-design.md)); after boot also the esp-mdm log stream in Elasticsearch |
| On-device, operator | sound, PIR coverage, the fan actually switching | the operator, after the agent's checks pass |

The agent-driven layer needs two things built into the firmware:

- **A console with test hooks.** Make USB-Serial-JTAG the primary console, so it accepts input as
  well as printing. The `esp_console` commands to add are: inject motion, inject a VOC index,
  force the relay, play a test tone, and print audio counters (packets, gaps filled, underruns).
  The agent then exercises the whole path on the device without touching it.
- **A test audio sender.** `tools/send-audio.py` streams a WAV file in the UDP format. Whether the
  pod can reach the device on the LAN is open (Q6). Without that, the agent verifies the audio
  path with the test tone and counters, and the operator verifies the sound. Once IntercomServer
  plays ambient sound, it is the end-to-end sender from production.

A green gate proves the build and the logic, never the behaviour on the device.

**Bench caveat.** esp-mdm checks for OTA on every boot. It installs the model's current version
whenever that differs from the running one. A dev build flashed through the bench is therefore
replaced by the MDM version on its next boot, unless the bench device's configuration has
`enableOTA: false`. The remote-flashing design treats this as a decision (its D7).

## 10. Open questions

- **Q1. What sends the music?** Settled: IntercomServer, switched by HA
  ([ambient-sound-design.md](ambient-sound-design.md)).
- **Q2. Audio format.** 48 kHz, 44.1 kHz or 32 kHz, mono 16-bit. Lean: 48 kHz mono. The device
  advertises one format, and IntercomServer converts to it.
- **Q3. Fan logic on the device or in Home Assistant?** Settled: in Home Assistant, with the
  device's fail-safe (decision 5).
- **Q4. Board variant.** FH4R2 or N8R8. This sets the flash layout and PSRAM budget.
- **Q5. Relay and fan.** The fan's rating and the relay part.
- **Q6. Can a KubeCoder pod reach the device's LAN?** It matters for the test audio sender and for
  any OTA-from-the-pod variant of the bench. The production IntercomServer already reaches the
  LAN.

## 11. Order of work

1. **The remote bench.** Done on 2026-11-22
   ([remote-flashing-plan.md](https://github.com/pvginkel/FirmwareBench/blob/main/docs/remote-flashing-plan.md)).
2. **The skeleton.** The `.kubecoder` config (`esp-idf`, `native`, `esp-libs` shared),
   `project.yaml`, CMake, partitions, the esp-mdm `Application`, the console, the Jenkinsfile and
   the IoT Support model. It boots to ready on the bench, with logs read by the agent.
3. **`esp-audio-support`.** The extraction, playback, the advertised endpoint and format, and the
   test tone and sender.
4. **Motion and the HA entities.**
5. **The SGP40, the VOC index, and the relay switch with its fail-safe.** The relay waits until
   it is on the schematic.
6. **Release wiring.** Then the first OTA through IoT Support.
7. **The HA automations.** Occupancy, the fan and the ambient sound, once IntercomServer plays
   ([ambient-sound-design.md](ambient-sound-design.md)).
