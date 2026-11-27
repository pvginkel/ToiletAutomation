#!/usr/bin/env python3
"""Streams mono 16-bit PCM at 48 kHz to a ToiletAutomation device over UDP, as IntercomServer does.

The wire format is the device's (docs/high-level-design.md, section 5): each datagram is a 4-byte
big-endian counter, then 16-bit little-endian PCM. The pacing is IntercomServer's ambient player's
(docs/ambient-sound-design.md, section 3.5):

- 20 ms chunks, timed against a stopwatch started with the stream;
- the first 500 ms, the prebuffer, go out ahead of real time over about 100 ms, and the sender
  stays that far ahead;
- a stall of more than 60 ms resets the stopwatch, rather than sending what is late in a burst;
- each chunk goes out as datagrams of at most 1468 bytes of PCM.

A chunk splits into packets of equal length: at 48 kHz, 1920 bytes are two packets of 960. Today's
IntercomServer fills each packet to 1468 bytes first, which makes 1468 and 452, and --split greedy
does the same. From a KubeCoder pod, the device then gets most short packets before the long one
ahead of them. The device places a packet that comes after a later one in the place it estimated
for it, the stream's mean packet, and cuts the 1468-byte packets to that, so the stream runs short
and runs dry every half second. Packets of one length arrive in order, and a lost one's place is
exact.

What it streams is a mono 16-bit 48 kHz WAV, or with --test-signal a generated one: slow held
chords that crossfade into each other, for a listener to judge clicks and dropouts, at about
IntercomServer's loudness target of -20 LUFS.

--drop and --reorder exercise the device's gap fill: a dropped packet's counter is skipped, and a
reordered packet goes out right after the one behind it. The device's `audio` console command then
shows the gaps it filled and the packets it put back in order.

Python's standard library only.

    tools/send-audio.py --host 10.1.1.20 --test-signal 300
    tools/send-audio.py --host 10.1.1.20 --drop 0.01 --test-signal 300
    tools/send-audio.py --host 10.1.1.20 music.wav
"""

import argparse
import array
import math
import random
import socket
import sys
import time
import wave

SAMPLE_RATE = 48000
SAMPLE_BYTES = 2
CHUNK_MS = 20
PREBUFFER_MS = 500
PREBUFFER_SPREAD_MS = 100
STALL_RESET_MS = 60
MAX_PAYLOAD = 1468
DEFAULT_PORT = 11106
PROGRESS_EVERY_S = 30

# The test signal: four chords of four notes, each held 4 s and crossfaded over 1 s into the next,
# in a loop of 16 s. Each note rounds to a whole number of cycles in the loop, so the loop is
# seamless, and a note two chords share keeps its phase through the crossfade.
CHORD_SECONDS = 4.0
CROSSFADE_SECONDS = 1.0
CHORDS = [
    [261.63, 329.63, 392.00, 523.25],  # C major
    [220.00, 329.63, 440.00, 523.25],  # A minor
    [349.23, 440.00, 523.25, 698.46],  # F major
    [196.00, 293.66, 392.00, 493.88],  # G major
]
# Each note's harmonics, as amplitudes of the fundamental's.
HARMONICS = [1.0, 0.3, 0.1]
# The peak a note's waveform could reach with every note in phase: about -9 dBFS. Its RMS level is
# then about -21 dBFS.
NOTE_AMPLITUDE = 0.35 / (4 * sum(HARMONICS))
FADE_SECONDS = 0.5


def parse_args():
    parser = argparse.ArgumentParser(
        description="Stream mono 16-bit 48 kHz PCM to a ToiletAutomation device over UDP, as IntercomServer does."
    )
    parser.add_argument("--host", required=True, help="the device's address, such as 10.1.1.20")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"the device's audioUdpPort ({DEFAULT_PORT})")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("wav", nargs="?", help="a mono 16-bit 48 kHz WAV")
    source.add_argument(
        "--test-signal", type=float, metavar="SECONDS", help="stream this many seconds of generated held chords instead"
    )
    parser.add_argument("--drop", type=float, default=0.0, metavar="P", help="drop each packet with this probability")
    parser.add_argument(
        "--reorder",
        type=float,
        default=0.0,
        metavar="P",
        help="send each packet after the next one with this probability",
    )
    parser.add_argument("--seed", type=int, help="the seed for --drop and --reorder, to repeat a run")
    parser.add_argument("--counter", type=int, default=0, help="the first packet's counter (0)")
    parser.add_argument(
        "--split",
        choices=["equal", "greedy"],
        default="equal",
        help="split each chunk into packets of equal length (equal), or fill each to 1468 bytes first, as "
        "today's IntercomServer does (greedy)",
    )
    args = parser.parse_args()

    for name in ("drop", "reorder"):
        value = getattr(args, name)
        if not 0.0 <= value < 1.0:
            parser.error(f"--{name} is a probability from 0 to below 1")
    if args.test_signal is not None and args.test_signal <= 0:
        parser.error("--test-signal takes a length in seconds above 0")
    if not 0 <= args.counter <= 0xFFFFFFFF:
        parser.error("--counter is a 32-bit unsigned number")

    return args


def read_wav(path):
    with wave.open(path, "rb") as wav:
        if wav.getnchannels() != 1 or wav.getsampwidth() != SAMPLE_BYTES or wav.getframerate() != SAMPLE_RATE:
            sys.exit(
                f"{path}: {wav.getnchannels()} channels, {wav.getsampwidth() * 8}-bit, {wav.getframerate()} Hz; "
                f"the device plays mono 16-bit at {SAMPLE_RATE} Hz"
            )
        if wav.getcomptype() != "NONE":
            sys.exit(f"{path}: compressed ({wav.getcompname()}); the device plays PCM")

        # A WAV's samples are little-endian, as the wire's are.
        return wav.readframes(wav.getnframes())


def chord_weight(chord, t, loop_seconds):
    """How much of a chord sounds at time t in the loop: 1 while held, a raised cosine through the crossfades."""
    start = chord * CHORD_SECONDS
    # Time since the chord's crossfade in began, in the loop, which wraps.
    since = (t - start + CROSSFADE_SECONDS) % loop_seconds
    if since < CROSSFADE_SECONDS:
        return 0.5 - 0.5 * math.cos(math.pi * since / CROSSFADE_SECONDS)
    if since < CHORD_SECONDS:
        return 1.0
    if since < CHORD_SECONDS + CROSSFADE_SECONDS:
        return 0.5 + 0.5 * math.cos(math.pi * (since - CHORD_SECONDS) / CROSSFADE_SECONDS)
    return 0.0


def make_loop():
    """One loop of the chords, as floats from -1 to 1."""
    loop_seconds = CHORD_SECONDS * len(CHORDS)
    loop_samples = int(loop_seconds * SAMPLE_RATE)

    # Each note's frequency, rounded to a whole number of cycles in the loop.
    notes = {}
    for index, chord in enumerate(CHORDS):
        for frequency in chord:
            cycles = round(frequency * loop_seconds)
            notes.setdefault(cycles, []).append(index)

    # The envelope of each chord, at 1 ms steps: each sample takes its millisecond's.
    steps = int(loop_seconds * 1000)
    weights = [[chord_weight(index, step / 1000.0, loop_seconds) for step in range(steps)] for index in range(len(CHORDS))]

    loop = [0.0] * loop_samples
    for cycles, chords in notes.items():
        envelope = [sum(weights[index][step] for index in chords) for step in range(steps)]
        phase_step = 2.0 * math.pi * cycles / loop_samples
        partials = [(number + 1, amplitude * NOTE_AMPLITUDE) for number, amplitude in enumerate(HARMONICS)]

        for n in range(loop_samples):
            level = envelope[n * 1000 // SAMPLE_RATE]
            if level == 0.0:
                continue
            phase = phase_step * n
            value = 0.0
            for number, amplitude in partials:
                value += amplitude * math.sin(number * phase)
            loop[n] += level * value

    return loop


def make_test_signal(seconds):
    """The chords for this long, faded in and out, as 16-bit little-endian PCM."""
    loop = array.array("h", (max(-32768, min(32767, round(value * 32767))) for value in make_loop()))
    total = int(seconds * SAMPLE_RATE)
    fade = min(int(FADE_SECONDS * SAMPLE_RATE), total // 2)

    samples = loop * (total // len(loop) + 1)
    del samples[total:]
    for n in range(fade):
        samples[n] = round(samples[n] * n / fade)
        samples[total - 1 - n] = round(samples[total - 1 - n] * n / fade)

    if sys.byteorder == "big":
        samples.byteswap()

    return samples.tobytes()


def packets(pcm, split):
    """Each 20 ms chunk's packets' PCM, chunk by chunk."""
    chunk_bytes = SAMPLE_RATE * CHUNK_MS // 1000 * SAMPLE_BYTES
    for offset in range(0, len(pcm), chunk_bytes):
        chunk = pcm[offset : offset + chunk_bytes]
        payload = MAX_PAYLOAD
        if split == "equal":
            count = -(-len(chunk) // MAX_PAYLOAD)
            # Whole samples in each packet.
            payload = -(-len(chunk) // count // SAMPLE_BYTES) * SAMPLE_BYTES
        yield [chunk[i : i + payload] for i in range(0, len(chunk), payload)]


def stream(args, pcm):
    address = (args.host, args.port)
    # Not connected: a connected socket turns the ICMP a rebooting device sends back into an error,
    # which would end the stream.
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", 0))
    source_port = sock.getsockname()[1]

    rng = random.Random(args.seed)
    seconds = len(pcm) / SAMPLE_BYTES / SAMPLE_RATE
    prebuffer_chunks = PREBUFFER_MS // CHUNK_MS

    print(
        f"Streaming {seconds:.1f} s to {args.host}:{args.port} from port {source_port}, "
        f"counter from {args.counter}, {args.split} split, drop {args.drop}, reorder {args.reorder}",
        flush=True,
    )

    counter = args.counter
    sent = dropped = reordered = resets = 0
    held = None
    start = time.perf_counter()
    next_progress = PROGRESS_EVERY_S

    def send(datagram):
        nonlocal sent
        sock.sendto(datagram, address)
        sent += 1

    try:
        for index, chunk in enumerate(packets(pcm, args.split)):
            # Ahead of real time by the prebuffer, which goes out over its spread first.
            due = start + max(index * PREBUFFER_SPREAD_MS / prebuffer_chunks, index * CHUNK_MS - PREBUFFER_MS) / 1000
            now = time.perf_counter()
            if due > now:
                time.sleep(due - now)
            elif now - due > STALL_RESET_MS / 1000:
                # A stall: the stopwatch starts again from this chunk, and the lead shrinks by the stall.
                start += now - due
                resets += 1

            for payload in chunk:
                datagram = counter.to_bytes(4, "big") + payload
                counter = (counter + 1) & 0xFFFFFFFF

                if rng.random() < args.drop:
                    dropped += 1
                    continue
                if held is None and rng.random() < args.reorder:
                    held = datagram
                    continue

                send(datagram)
                if held is not None:
                    send(held)
                    held = None
                    reordered += 1

            if index * CHUNK_MS / 1000 >= next_progress:
                print(f"{next_progress} s sent: {sent} packets, {dropped} dropped, {reordered} reordered", flush=True)
                next_progress += PROGRESS_EVERY_S

        if held is not None:
            send(held)
    except KeyboardInterrupt:
        print("Interrupted", flush=True)

    print(
        f"Sent {sent} packets from port {source_port}: {dropped} dropped, {reordered} reordered, "
        f"{resets} stalls reset the stopwatch. The device plays out its buffer, about "
        f"{(PREBUFFER_MS + 200) / 1000:.1f} s, and its stream ends 0.5 s after that",
        flush=True,
    )


def main():
    args = parse_args()

    if args.test_signal is not None:
        print(f"Generating {args.test_signal:g} s of held chords", flush=True)
        pcm = make_test_signal(args.test_signal)
    else:
        pcm = read_wav(args.wav)

    if not pcm:
        sys.exit("Nothing to stream")

    stream(args, pcm)


if __name__ == "__main__":
    main()
