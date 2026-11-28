# ToiletAutomation: hardware bring-up

Every part on the experimental board works on the pins the schematic gives it. The bring-up
changed one thing in the hardware: the amplifier's SD pin is now tied to 3V3, so the amp plays the
left I2S channel only. That change is in the schematic as of `f2b5aea`. The bring-up also raised
one hardware point: nothing held the relay driver off while the ESP32 was in reset. The schematic
has a 10 kΩ pull-down for it, R2, as of `744a522`.

Witnessed on 2026-11-27, on the operator's experimental board on the bench, wired to the schematic
at `dba271f` with SD added. The test app is `hwtest/`. The agent read the results through the
bench's serial log and console. The operator confirmed by ear and eye what only a person can
check: the speaker, the relay's output and the PIR reacting to a hand.

## Pins

Read off the schematic's netlist. The Waveshare ESP32-S3-Tiny's pin names are GPIO numbers.

| Net | GPIO | Part and pin |
|---|---|---|
| `PIR_OUT` | 1 | HC-SR602 OUT |
| `AIRQ_SCL` | 2 | SEN0394 (SGP40) SCL |
| `AIRQ_SDA` | 3 | SEN0394 (SGP40) SDA |
| `RELAY` | 4 | 1 kΩ to the S8050's base, which R2 (10 kΩ) pulls to GND. The S8050 switches the G5Q-1A's coil, with a 1N4148 across it |
| `SPEAKER_DATA` | 16 | MAX98357 DIN |
| `SPEAKER_SCK` | 17 | MAX98357 BCLK |
| `SPEAKER_WS` | 18 | MAX98357 LRC |

Power: the HC-SR602 and the SEN0394 run on 3V3. The MAX98357, the relay coil and the module run
on 5V.

## What each part did

### The module

The module is an **ESP32-S3FH4R2**, chip revision v0.2. It has 4 MB of flash and 2 MB of quad
PSRAM, both in the package. That settles the high-level design's open question Q4.

- **How it was read.** The eFuses report a 4 MB flash capacity and a 2 MB PSRAM capacity. The
  flash chip reports 4096 KB.
- **PSRAM.** ESP-IDF finds and tests the PSRAM in quad mode at 40 MHz. It adds 2 MB to the heap,
  beside 324 KB of internal RAM.
- **USB.** The USB-C is the S3's native USB-Serial-JTAG (USB id 303a:1001). As the primary
  console it takes input, so the bench can drive console commands.

### The air sensor (SGP40)

It answers at 0x59, the only device on the bus, and it reacts to VOCs.

- **The bus.** With the ESP32's weak pull-downs on, both lines still read high. So the SEN0394
  board's own pull-ups are there.
- **Identity and self-test.** The serial number is 0x0000057f63a1. The self-test answers 0xD400,
  which means pass.
- **Warm-up.** The raw signal was about 24,700 just after power-up. It settled at about 30,300
  after a few minutes of measuring once a second.
- **The IPA test.** The operator held isopropyl alcohol near the sensor.
  - Before: steady at about 30,350.
  - The value started falling at once. It reached 23,246 (23 % lower) about 37 seconds later.
  - It had climbed back to about 25,000 after another 30 seconds and was still rising.

This tested the raw signal only. The 0–500 VOC index that Home Assistant gets comes from
Sensirion's algorithm in the firmware. The warm-up above is why the firmware holds the index back
at first.

### The PIR (HC-SR602)

Its output reaches GPIO1 and moves with motion. The operator saw the log follow their hand.

- **Pulse lengths.** Over about ten triggers, the output stayed high between 1.7 s and 4.8 s. The
  shortest pulses were about 1.7 s. Longer ones look like retriggering on continued motion.
- **A trigger at boot.** One trigger came 1.5 s after a power-up under `hwtest`. Under the
  firmware, one boot in six with the PIR under a cup gave a pulse: high 1.2 s after boot, about
  0.4 s after Wi-Fi started, for 1.75 s. The other five gave none, and neither did one with the
  PIR uncovered. All six were resets of the ESP32, which leave the PIR powered, so they say
  nothing about the sensor's own power-up. Unplugging the board and plugging it back in would
  test that.
- **Wi-Fi did not trigger it.** The firmware ran on Wi-Fi, connected to MQTT, with the PIR under
  a cup. In 30 minutes, the motion filter counted no edges at all. The filter's defaults stay as
  they are.

### The speaker (MAX98357)

The I2S output plays on the speaker. It was tested with 16-bit stereo at 48 kHz in Philips
format, using sine tones.

- **The speaker is a Visaton FR 7**, the one in the intercoms. On 2026-11-28 the operator played
  the same ambient tracks on it and on a Visaton BF 32, a 32 mm full-range rated 150 Hz to
  20 kHz, and kept the FR 7 as the better all-round speaker.
- **Channel test with SD floating.** A tone on both channels was loud. A tone on the left only or
  the right only was quieter. The amp was mixing the two channels, (L+R)/2.
- **Channel test with SD tied to 3V3.** Both channels and left only were loud. Right only was
  silent. The amp now plays the left channel only.
- **Gain.** GAIN is left unconnected, which the MAX98357 datasheet gives as 9 dB. That's read
  from the datasheet, not measured. A tone at 20 % of full scale was clearly audible.
- **The loudest it takes.** Under the firmware, with Identify's 880 Hz beeps at full scale, the
  operator heard crackling above -12 dB and none at -12 dB or below.
- **Crackle under Wi-Fi traffic (2026-11-28, P6).** With a stream running, the speaker crackled,
  and so did Identify's beeps mixed into it. With no stream, the same beeps played clean.
  - Not the audio data. The board logged no gaps and no underruns. A 440 Hz tone crackled over
    a stream of pure silence, and worse during a flood of the same packet rate to another
    port, which never reaches the audio path. Silence never crackled: only a signal did.
  - The supply was part of it. The board ran on the PC's USB 5 V. On a Voltcraft LPS 1305 at
    5 V, on the board's 5 V pin and the amp's, with the USB's 5 V cut, the crackle fell to
    about a fifth. A bulk capacitor at the amp changed nothing after that.
  - The wiring was the rest. The I2S wires looped 30–40 cm around the breadboard between modules
    10 cm apart, with no ground beside them. Short wires, a ground wire beside them and the
    bundle tie-wrapped took the crackle away, flood included.
  - It is not gone at every level. After the rewire, Identify still crackles above a volume of
    0.8 (-17.6 dB). The operator expects soldered tracks to cure that. The -12 dB cap stays
    until a soldered board is tested.
  - The console has `i2s drive [0-3]` and `wifi txpower [dBm]` for the next board. Neither is
    persisted. The I2S pins boot at drive 2 for DIN and 1 for BCLK and LRC, and the TX power at
    20 dBm. Intercom's V1 board capped it at 11 dBm against the same kind of interference.

### The relay

GPIO4 switches the relay's output on and off, and the pin reads back the level it was set to. The
operator saw the output follow the commands. In their words: "it's a led at the moment". The
bench board has no relay fitted, and its relay output lights an LED.

## Resolved hardware point: the relay during reset

**R2 holds the S8050's base low while the ESP32 is in reset or booting.** During that time
nothing drives GPIO4. Without a pull-down the relay could pick up for a moment, and the design
asks that the relay comes up off. The first thing `hwtest` does is drive GPIO4 low, so after boot
it is held off.

- **The fix.** R2, a 10 kΩ resistor from the S8050's base to GND, added to the schematic at
  `744a522`.
- **How to check it on a board.** Watch the relay's output during a reset.

The bring-up never saw the relay turn on during a reset. But no one was watching for it, so that
proves nothing.

## What the firmware takes from this

- **Audio goes in the left I2S slot.** In ESP-IDF that's I2S mono mode with the left slot. It
  also works to duplicate the samples into both slots.
- **The flash layout is for 4 MB, with quad PSRAM.** That is what the design assumed for the
  FH4R2: two OTA slots of about 1.9 MB and a coredump partition.
- **The motion filter's minimum pulse length must stay under about 1.7 s.** That's the shortest
  real pulse seen. Any requirement for a longer pulse would throw away real motion.
- **The motion filter ignores the PIR for the first 10 s after boot.** The PIR gives a false
  pulse after a power-up or reset, as the trigger at boot in the PIR section describes. A pulse
  that rises inside those 10 s is never accepted, even if it is still high when they end.
- **The volume tops out at -12 dB.** Louder crackles, so a volume of 1 is -12 dB.
- **The I2S lines are short, with ground beside them.** Long loose wires crackled under Wi-Fi
  traffic. The final board keeps BCLK, LRC and DIN short over ground, and the amp on a clean 5 V.
- **The VOC index waits out the warm-up.** The raw signal drifts for minutes after power-up.
- **The console works as the design wants.** USB-Serial-JTAG as the primary console, driven by
  the bench, is what the design's test hooks rely on.

## What this changes in the high-level design

`docs/high-level-design.md` carries these findings: the module, the USB and the relay in §2, the
flash layout in §8, and Q4 settled.

## Running `hwtest`

```sh
cd /work/ToiletAutomation/hwtest && cexec esp-idf idf.py build
curl -s --fail-with-body http://127.0.0.1:3521/flash \
  -d '{"build_dir": "/work/ToiletAutomation/hwtest/build"}'
```

At boot the app does these checks, in order, and ends with a `HWTEST SUMMARY` line:

1. It drives the relay off.
2. It reports the module.
3. It checks the I2C pull-ups and scans the bus.
4. It reads the SGP40's serial number and runs its self-test.
5. It starts logging PIR edges.
6. It plays two short beeps.

Then it runs a console with the prompt `hwtest> `:

| Command | What it does |
|---|---|
| `info` | the chip, flash, PSRAM and pin map |
| `i2cscan` | scans the air sensor's bus |
| `sgp40 [count\|selftest]` | raw readings at 1 Hz, or the self-test |
| `pir` | the current level and the number of triggers |
| `relay [on\|off\|pulse [ms]]` | switches the relay |
| `tone [hz] [ms] [volume%] [both\|left\|right]` | plays a sine on the speaker |
