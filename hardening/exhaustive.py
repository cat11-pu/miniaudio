#!/usr/bin/env python3
"""
Exhaustive single-byte corruption: for every byte offset in the seed,
set it to 0x00 and 0xFF and decode (file + mem + seek modes).
"""
import os
import subprocess
import sys
import multiprocessing

HARNESS = "./decode_asan"


def run_one(args):
    data, tag = args
    path = os.path.join("/tmp", f"ma_exh_{os.getpid()}.bin")
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


def main():
    seed = sys.argv[1]
    blob = open(seed, "rb").read()
    os.makedirs("crashes", exist_ok=True)
    cases = []
    for off in range(len(blob)):
        for val in (0x00, 0xFF):
            data = bytearray(blob)
            data[off] = val
            cases.append((bytes(data), f"{os.path.basename(seed)}_set{val:02x}@{off}"))
    total = 0
    crashes = 0
    with multiprocessing.Pool(8) as pool:
        for status, tag, data, err in pool.imap_unordered(run_one, cases, chunksize=32):
            total += 1
            if status:
                crashes += 1
                fn = os.path.join("crashes", f"exh_{tag.replace('/', '_')}")
                with open(fn, "wb") as f:
                    f.write(data)
                with open(fn + ".log", "wb") as f:
                    f.write(err)
                print(f"[{status}] {fn}", flush=True)
                print(err.decode(errors="replace")[:600], flush=True)
    print(f"total_cases={total} crashes={crashes}")
    return 1 if crashes else 0


if __name__ == "__main__":
    sys.exit(main())
