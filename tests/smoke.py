#!/usr/bin/env python3
"""Smoke tests for the libksf zlib shim.

Each case runs a child Python interpreter twice: once with LD_PRELOAD pointing
at build/libksf.so and once against the stock system zlib. The child calls
zlib.compress() (which reaches deflate/deflateInit2_) and zlib.decompress()
(stock inflate), asserts the round-trip, and reports the compressed size.

Checks:
  * every case round-trips under the shim at levels 0, 1, 3, 6, 9
  * the shim is actually interposed (its output differs from stock)
  * at levels 6 and 9 the shim beats stock on compressible data
  * streaming works: Z_SYNC_FLUSH markers, raw and gzip wrappers, deflateCopy
  * preset dictionaries round-trip through deflateSetDictionary/inflate
  * build/abi_test passes its return-code and bound checks under LD_PRELOAD
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LIB = os.path.join(ROOT, "build", "libksf.so")
ABI = os.path.join(ROOT, "build", "abi_test")
STREAM_ABI = os.path.join(ROOT, "build", "stream_abi")

CHILD = r"""
import sys, zlib
path, level = sys.argv[1], int(sys.argv[2])
data = open(path, "rb").read()
packed = zlib.compress(data, level)
assert zlib.decompress(packed) == data, "round-trip failed"
sys.stdout.write("%d" % len(packed))
"""

STREAM_CHILD = r"""
import sys, zlib
path, level = sys.argv[1], int(sys.argv[2])
data = open(path, "rb").read()

# Sync flush after every chunk: exercises the empty stored-block marker.
obj = zlib.compressobj(level)
parts, pos = [], 0
while pos < len(data):
    parts.append(obj.compress(data[pos:pos + 4096]))
    parts.append(obj.flush(zlib.Z_SYNC_FLUSH))
    pos += 4096
parts.append(obj.flush(zlib.Z_FINISH))
stream = b"".join(parts)
assert zlib.decompress(stream) == data, "sync-flush round-trip failed"

# Raw deflate wrapper.
obj = zlib.compressobj(level, zlib.DEFLATED, -15)
raw = obj.compress(data) + obj.flush()
assert zlib.decompress(raw, -15) == data, "raw round-trip failed"

# gzip wrapper.
obj = zlib.compressobj(level, zlib.DEFLATED, 31)
gz = obj.compress(data) + obj.flush()
assert zlib.decompress(gz, 31) == data, "gzip round-trip failed"

# Preset dictionary: the zlib wrapper must carry FDICT/DICTID, the raw wrapper
# must use the dictionary without signalling it.
zdict = bytes((i * 13 + (i >> 5)) & 0x3F for i in range(20000))
body = (zdict * 4)[:60000] + data
obj = zlib.compressobj(level, zlib.DEFLATED, 15, 8,
                       zlib.Z_DEFAULT_STRATEGY, zdict)
packed = obj.compress(body) + obj.flush()
d = zlib.decompressobj(15, zdict)
assert d.decompress(packed) + d.flush() == body, "zlib zdict round-trip failed"
obj = zlib.compressobj(level, zlib.DEFLATED, -15, 8,
                       zlib.Z_DEFAULT_STRATEGY, zdict)
raw = obj.compress(body) + obj.flush()
d = zlib.decompressobj(-15, zdict)
assert d.decompress(raw) + d.flush() == body, "raw zdict round-trip failed"

# compressobj.copy() -> deflateCopy.
half = len(data) // 2
obj = zlib.compressobj(level)
head = obj.compress(data[:half])
clone = obj.copy()
tail_a = obj.compress(data[half:]) + obj.flush()
tail_b = clone.compress(data[half:]) + clone.flush()
assert tail_a == tail_b, "deflateCopy diverged"
assert zlib.decompress(head + tail_a) == data, "copy round-trip failed"

sys.stdout.write("stream ok")
"""

LARGE_CHILD = r"""
import sys, zlib
n = 5 * 1024 * 1024 + 12345
pattern = bytes((i * 7 + (i >> 3)) & 0xFF for i in range(65536))
data = (pattern * (n // len(pattern) + 1))[:n]

# One-shot Z_FINISH with more input than the 4 MiB segment limit: the whole
# buffer must be consumed in a single deflate() call.
packed = zlib.compress(data, 1)
assert zlib.decompress(packed) == data, "large one-shot round-trip failed"

# Incremental feeding without flushes: exercises automatic segmentation and
# the boundary markers between segments.
obj = zlib.compressobj(1)
parts = []
for i in range(0, len(data), 262144):
    parts.append(obj.compress(data[i:i + 262144]))
parts.append(obj.flush())
assert zlib.decompress(b"".join(parts)) == data, "large incremental failed"

# Raw wrapper on the same input.
obj = zlib.compressobj(1, zlib.DEFLATED, -15)
raw = obj.compress(data) + obj.flush()
assert zlib.decompress(raw, -15) == data, "large raw failed"

sys.stdout.write("large ok")
"""

CONCURRENT_CHILD = r"""
import concurrent.futures, sys, zlib

def make(seed, n):
    out = bytearray(n)
    state = (0x9e3779b9 ^ (seed * 2654435761)) & 0xffffffff
    for i in range(n):
        state = (state * 1664525 + 1013904223) & 0xffffffff
        if (i // 97 + seed) % 3 == 0:
            out[i] = state >> 24
        else:
            out[i] = (i * 7 + seed) & 0xff
    return bytes(out)

inputs = [make(i, 4096 + i * 12000) for i in range(6)]
serial = [zlib.compress(d, 3) for d in inputs]
assert all(zlib.decompress(c) == d for c, d in zip(serial, inputs))

def work(job):
    index, _round = job
    return index, zlib.compress(inputs[index], 3)

jobs = [(i, r) for r in range(3) for i in range(len(inputs))]
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    for index, packed in pool.map(work, jobs):
        assert packed == serial[index], \
            "concurrent output diverged for input %d" % index

sys.stdout.write("concurrent ok")
"""


def cases():
    random.seed(7)
    return {
        "empty": b"",
        "one": b"A",
        "zeros": b"\0" * 200000,
        "text": b"the quick brown fox jumps over the lazy dog\n" * 4000,
        "mixed": bytes((i * 7 + (i >> 3)) & 0xFF for i in range(120000)),
        "random": bytes(random.randrange(256) for _ in range(50000)),
    }


def run(env, path, level, child=CHILD):
    proc = subprocess.run(
        [sys.executable, "-c", child, path, str(level)],
        env=env, capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "child exited %d" % proc.returncode)
    return proc.stdout


def main():
    for path in (LIB, ABI, STREAM_ABI):
        if not os.path.exists(path):
            print("missing %s; run make first" % path)
            return 1

    # LD_PRELOAD splits its value on spaces and colons, so stage the library at
    # a space-free path. The project directory itself may contain spaces.
    staging = tempfile.mkdtemp(prefix="ksf-")
    staged_lib = os.path.join(staging, "libksf.so")
    with open(LIB, "rb") as src, open(staged_lib, "wb") as dst:
        dst.write(src.read())

    shim_env = dict(os.environ, LD_PRELOAD=staged_lib)
    stock_env = {k: v for k, v in os.environ.items() if k != "LD_PRELOAD"}

    ok = bad = 0
    sizes = {}
    with tempfile.TemporaryDirectory() as tmp:
        for name, data in cases().items():
            path = os.path.join(tmp, name + ".bin")
            with open(path, "wb") as handle:
                handle.write(data)
            for level in (0, 1, 3, 6, 9):
                try:
                    shim = int(run(shim_env, path, level))
                    stock = int(run(stock_env, path, level))
                except RuntimeError as exc:
                    print("FAIL %-6s level %d: %s" % (name, level, exc))
                    bad += 1
                    continue
                sizes[(name, level)] = (shim, stock)
                status = "ok" if shim <= stock else "bigger"
                print("%-6s level %d  shim %7d  stock %7d  %s"
                      % (name, level, shim, stock, status))
                ok += 1

        # Streaming paths: sync flush, raw, gzip, and deflateCopy. Kept to a
        # couple of cases so the suite stays fast.
        for name, level in (("text", 3), ("text", 6), ("empty", 6)):
            path = os.path.join(tmp, name + ".bin")
            for label, env in (("shim", shim_env), ("stock", stock_env)):
                try:
                    out = run(env, path, level, STREAM_CHILD)
                except RuntimeError as exc:
                    print("FAIL stream %-5s level %d (%s): %s"
                          % (name, level, label, exc))
                    bad += 1
                    continue
                if out.strip() != "stream ok":
                    print("FAIL stream %-5s level %d (%s): %s"
                          % (name, level, label, out.strip()))
                    bad += 1
                else:
                    print("stream %-5s level %d  %s" % (name, level, label))
                    ok += 1

        # Large input: one-shot Z_FINISH above the segment limit plus automatic
        # segmentation of an incrementally fed stream.
        for label, env in (("shim", shim_env), ("stock", stock_env)):
            try:
                out = run(env, "", 0, LARGE_CHILD)
            except RuntimeError as exc:
                print("FAIL large (%s): %s" % (label, exc))
                bad += 1
                continue
            if out.strip() != "large ok":
                print("FAIL large (%s): %s" % (label, out.strip()))
                bad += 1
            else:
                print("large input  %s" % label)
                ok += 1

        # Independent streams must not interfere when compressed concurrently.
        for label, env in (("shim", shim_env), ("stock", stock_env)):
            try:
                out = run(env, "", 0, CONCURRENT_CHILD)
            except RuntimeError as exc:
                print("FAIL concurrent (%s): %s" % (label, exc))
                bad += 1
                continue
            if out.strip() != "concurrent ok":
                print("FAIL concurrent (%s): %s" % (label, out.strip()))
                bad += 1
            else:
                print("concurrent  %s" % label)
                ok += 1

    # The shim must actually be interposed somewhere.
    differs = any(shim != stock for shim, stock in sizes.values())
    if not differs:
        print("FAIL shim never changed the output; LD_PRELOAD did not take effect")
        bad += 1

    # On compressible data the whole point is a smaller stream at high levels.
    for name in ("text", "mixed", "zeros"):
        for level in (6, 9):
            key = (name, level)
            if key in sizes and not sizes[key][0] < sizes[key][1]:
                print("FAIL %s level %d: shim %d not smaller than stock %d"
                      % (name, level, sizes[key][0], sizes[key][1]))
                bad += 1

    for label, env in (("shim", shim_env), ("stock", stock_env)):
        for name, binary in (("abi_test", ABI), ("stream_abi", STREAM_ABI)):
            proc = subprocess.run([binary], env=env, capture_output=True,
                                  text=True)
            if proc.returncode != 0:
                print("FAIL %s (%s): %s" % (name, label,
                                            proc.stdout.strip()))
                bad += 1
            else:
                print("%s (%s): %s" % (name, label, proc.stdout.strip()))

    print("ok=%d bad=%d" % (ok, bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
