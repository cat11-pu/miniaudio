#!/usr/bin/env python3
"""
Mutation fuzzer for the miniaudio decode harness.

Strategies:
  - trunc:      cut the file at systematic offsets
  - hdrstorm:   random byte storms over the first 256 bytes
  - flip:       random bit flips anywhere
  - truncflip:  truncate + random bytes
  - structured: format-aware corruption of WAV fmt fields / FLAC and MP3 headers

Every case is decoded both from file and from memory. Any exit code other
than 0/2, or sanitizer output on stderr, counts as a crash. Unique crashes
are saved to crashes/.
"""
import os
import random
import subprocess
import sys
import multiprocessing

HARNESS = "./decode_asan"
SEEDS = ["seeds/tone.wav", "seeds/tone.mp3", "seeds/tone.flac", "seeds/sample.flac",
         "seeds/tone_u8_mono.wav", "seeds/tone_s24.wav", "seeds/tone_f32.wav",
         "seeds/tone_mulaw.wav", "seeds/tone_vbr_mono.mp3", "seeds/tone_8k.flac",
         "seeds/tone_s32.flac", "seeds/tone_smallframe.flac"]
CRASH_DIR = "crashes"

random.seed(1234)


def run_one(args):
    data, tag, wid = args
    path = os.path.join("/tmp", f"ma_fuzz_case_{os.getpid()}.bin")
    with open(path, "wb") as f:
        f.write(data)
    for mode in ([], ["mem"], ["seek"]):
        try:
            p = subprocess.run([HARNESS, path] + mode, capture_output=True, timeout=30)
        except subprocess.TimeoutExpired:
            return ("hang", tag + "+".join(mode), data, b"")
        bad = p.returncode not in (0, 2) or b"Sanitizer" in p.stderr or b"runtime error" in p.stderr
        if bad:
            return ("crash", tag + "+".join(mode), data, p.stderr[-2000:])
    return (None, tag, data, b"")


def mutate_structured(name, blob):
    """Format-aware corruption of header fields."""
    data = bytearray(blob)
    if name.endswith(".wav"):
        # fmt chunk fields: formatTag@20, channels@22, sampleRate@24,
        # avgBps@28, blockAlign@32, bitsPerSample@34; RIFF size@4; data size.
        fields = [20, 22, 24, 28, 32, 34, 4]
        idx = blob.find(b"data")
        if idx >= 0:
            fields.append(idx + 4)
        interesting = [0, 1, 2, 3, 7, 8, 15, 16, 24, 31, 32, 33, 64,
                       0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF,
                       0x10000, 0x7FFFFFFF, 0xFFFFFFFF]
        for _ in range(random.randint(1, 3)):
            off = random.choice(fields) + random.randrange(0, 2)
            val = random.choice(interesting)
            chunk = val.to_bytes(4, "little")[:max(1, min(4, len(data) - off))]
            data[off:off + len(chunk)] = chunk
        return bytes(data), "wavfields"
    if name.endswith(".flac"):
        for _ in range(random.randint(1, 4)):
            off = random.randrange(min(len(data), 256))
            data[off] = random.choice([0, 1, 0x7F, 0x80, 0xFF, 0xF8, 0x7E])
        return bytes(data), "flachdr"
    if name.endswith(".mp3"):
        for _ in range(random.randint(1, 6)):
            off = random.randrange(len(data))
            data[off] = random.choice([0xFF, 0xFB, 0xFA, 0x00, 0x90, 0xE0])
        return bytes(data), "mp3hdr"
    return bytes(data), "noop"


def main():
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 4000
    total = 0
    seen = set()

    os.makedirs(CRASH_DIR, exist_ok=True)

    def save_crash(idx, tag, data, err):
        fn = os.path.join(CRASH_DIR, f"crash_{idx:03d}_{tag.replace('/', '_').replace('#', '_')[:120]}")
        with open(fn, "wb") as f:
            f.write(data)
        with open(fn + ".log", "wb") as f:
            f.write(err)
        return fn

    def record(tag, data, err):
        sig = err.split(b"\n")[1:3] if err else [b"hang"]
        key = b"|".join(sig)
        if key in seen:
            return 0
        seen.add(key)
        fn = save_crash(len(seen), tag, data, err)
        print(f"[crash] {fn}", flush=True)
        print(err.decode(errors="replace")[:600], flush=True)
        return 1

    pool = multiprocessing.Pool(8)
    crash_count = 0

    def run_batch(cases):
        nonlocal total, crash_count
        for status, tag, data, err in pool.imap_unordered(run_one, cases, chunksize=16):
            total += 1
            if status:
                crash_count += record(tag, data, err)

    seeds = [(s, open(s, "rb").read()) for s in SEEDS]

    # 1. Systematic truncation.
    cases = []
    for name, blob in seeds:
        offsets = list(range(0, len(blob), 257)) + list(range(max(0, len(blob) - 64), len(blob)))
        for off in offsets:
            cases.append((blob[:off], f"{os.path.basename(name)}_trunc@{off}", off % 8))
    run_batch(cases)

    # 2. Random + structured mutations.
    cases = []
    for i in range(iterations):
        name, blob = seeds[i % len(seeds)]
        base = os.path.basename(name)
        data = bytearray(blob)
        strategy = i % 4
        if strategy == 0:
            for _ in range(random.randint(1, 32)):
                pos = random.randrange(min(len(data), 256))
                data[pos] = random.randrange(256)
            tag = f"{base}_hdrstorm#{i}"
        elif strategy == 1:
            for _ in range(random.randint(1, 8)):
                pos = random.randrange(len(data))
                data[pos] ^= 1 << random.randrange(8)
            tag = f"{base}_flip#{i}"
        elif strategy == 2:
            cut = random.randrange(1, len(data))
            data = data[:cut]
            for _ in range(random.randint(1, 4)):
                pos = random.randrange(len(data))
                data[pos] = random.randrange(256)
            tag = f"{base}_truncflip#{i}"
        else:
            data, stag = mutate_structured(base, bytes(blob))
            tag = f"{base}_{stag}#{i}"
        cases.append((bytes(data), tag, i % 8))
        if len(cases) >= 512:
            run_batch(cases)
            cases = []
    run_batch(cases)

    pool.close()
    pool.join()
    print(f"\ntotal_cases={total} unique_crashes={crash_count}")
    return 1 if crash_count else 0


if __name__ == "__main__":
    sys.exit(main())
