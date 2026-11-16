# ksflate

A fork of the reference-exact KZIP/PNGOUT reconstructions aimed at two things:

1. a **zlib drop-in** (`libksf.so`) usable with `LD_PRELOAD`, and
2. **speed** — the engine is now free of process-wide mutable state, so
   independent compressions run in parallel.

This directory currently contains the zlib shim and the de-static engine work.
The original reconstructions in `../kzip-decomp/` and `../pngout-decomp/` are
untouched and remain the byte-exact oracle.

## Build

```sh
make            # build/libksf.so, build/abi_test, build/stream_abi
make test       # ABI + round-trip + streaming + consumer tests
make clean
```

The engine sources under `src/` are copied from `../kzip-decomp/src/` and are
built as C++20. Only the deflate side is provided; `inflate*`, `crc32`,
`adler32`, `uncompress*`, `gz*` and friends resolve from the real libz.

## Usage

`LD_PRELOAD` splits its value on spaces and colons, so the library path must
not contain spaces. Stage it somewhere simple first:

```sh
mkdir -p /tmp/ksf
cp build/libksf.so /tmp/ksf/
LD_PRELOAD=/tmp/ksf/libksf.so python3 -c 'import zlib; print(zlib.decompress(zlib.compress(b"hi"*1000)))'
```

Set `KSF_VERBOSE=1` to trace each segment on stderr.

## Implemented entry points

One-shot:

```
compress  compress2  compress_z  compress2_z  compressBound  compressBound_z
```

Streaming:

```
deflate  deflateInit_  deflateInit2_  deflateEnd  deflateReset
deflateResetKeep  deflateCopy  deflateBound  deflateBound_z  deflateUsed
deflatePending  deflateParams  deflateTune  deflateSetDictionary
deflateGetDictionary  deflatePrime  deflateSetHeader
```

Only these symbols are exported (see `zlib/ksf.map`). They are deliberately
unversioned so they interpose in front of the versioned `libz`.

## Level mapping

The engine's natural dial is the optimization-pass budget plus the parser
strategy. Measured on a 6.2 MB filtered PNG stream (deflate only):

| zlib level | strategy | passes | output | time |
|---|---|---|---|---|
| 0 | stored | – | n + ~0.02 % | fast |
| 1, 2 | 2 | 1 | 1,181,208 | 0.25 s |
| 3 | 1 | 1 | 652,517 | 3.99 s |
| 4, 5 | 1 | 2 | 634,755 | 5.96 s |
| 6 | 0 | 2 | 632,631 | 8.08 s |
| 7 | 0 | 4 | 628,122 | 11.4 s |
| 8 | 0 | 12 | 627,093 | 37.5 s |
| 9 | 0 | 1024 | 627,093 | 37.5 s (converges) |

Stock zlib -9 produced 717,351 bytes in 11.8 s on the same stream.

### End-to-end comparison

1.5 MB slice of the same stream, `zlib.compress()` under `LD_PRELOAD`:

| level | stock zlib | shim | ratio | speed |
|---|---|---|---|---|
| 0 | 1,500,126 B | 1,500,121 B | same | same |
| 1 | 300,808 B | 285,266 B | −5 % | 7.7× slower |
| 3 | 285,415 B | 157,205 B | −45 % | 24× slower |
| 6 | 208,761 B | 153,003 B | −27 % | 15× slower |
| 9 | 173,588 B | 151,342 B | −13 % | 2× slower |

The headline: **at level 9 the shim is smaller and about twice as slow**. Lower
levels trade much more speed for size. The level dial is coarse; tuning it is
an open question for the next step.

## Streaming model

The engine is a whole-buffer, multi-pass optimizer, so it cannot emit output
incrementally at the same quality. `deflate()` buffers input and compresses it
when one of these happens:

* `Z_FINISH` — final segment (`BFINAL=1`) plus wrapper trailer.
* `Z_SYNC_FLUSH` / `Z_FULL_FLUSH` / `Z_PARTIAL_FLUSH` / `Z_BLOCK` — non-final
  segment plus an empty stored block.
* 4 MiB of buffered input without a flush — automatic non-final segment plus
  an empty stored block.

Every segment is primed with the preceding 32 KiB of uncompressed data, so
LZ77 matches cross segment boundaries exactly as they do in stock zlib. The
decompressor already holds that window, so nothing extra is signalled. A
`Z_FULL_FLUSH` clears the history, preserving zlib's promise that decompression
can restart after the flush point. The empty stored block after each non-final
segment also restores byte alignment, because the engine pads each segment to a
byte boundary and those padding bits would otherwise be consumed as the next
block header. The marker length depends on the number of significant bits in
the final byte (4 bytes for 1–5 bits, else 5 bytes), which is why
`EncoderResult` exposes `end_bits`.

Memory is bounded by the 4 MiB segment limit plus at most 1 MiB of undelivered
output. A `Z_FINISH` call is never split, because callers such as CPython's
`zlib.compress()` pass the whole buffer with `Z_FINISH` and do not loop on
`avail_in`.

Wrappers: `windowBits` 8–15 → zlib, −8–−15 → raw, 24–31 → gzip, 40–47 → zlib.

## Dictionary / history (fork-local)

`EncoderOptions::history` accepts up to 32 KiB of bytes that precede the input
in the same deflate stream. The parser parses that history once per pass and
discards its tokens, leaving its hash chains, run cache and match rows
available to the real input. The history occupies the first `history_size`
positions of the same 64 KiB ring as the input, so distances and ring
wraparound behave exactly as if the bytes were one continuous stream.

This is what lets a parallel compressor keep cross-chunk ratio while
compressing chunks independently. `tests/history.cpp` verifies a primed
segment against zlib's `inflateSetDictionary`, and concatenates a
non-final/final pair through the sync marker.

The shim uses the same mechanism in two places:

* `deflateSetDictionary()` seeds the history before the first segment. For the
  zlib wrapper the header sets `FDICT` and carries the DICTID (Adler-32 of the
  whole dictionary, even when only its last 32 KiB is used), so `inflate()`
  returns `Z_NEED_DICT`. Raw streams use the dictionary without signalling it.
  gzip has no DICTID field, so it rejects dictionaries with `Z_STREAM_ERROR`,
  matching zlib.
* Each subsequent segment is primed automatically with the previous 32 KiB, so
  chunked and flushed streams keep cross-boundary matches. `Z_FULL_FLUSH`
  drops the history.

`deflateGetDictionary()` reports the current window (dictionary plus consumed
input, capped at 32 KiB). `deflateReset()` drops the dictionary;
`deflateResetKeep()` keeps it for raw streams and keeps the preset dictionary
for zlib streams, while dropping previous-stream input that a fresh header
could not describe. `tests/stream_abi.c` covers all of these paths.

The pigz fork (`../pigz-ks`) calls the engine directly for level 10 and
vendors the engine sources under `pigz-ks/engine/`.

## Concurrency

`Strategy12Parser` no longer keeps scratch in process-wide statics; every
parser owns its window, rings, hash chains and arenas. The shim therefore has
no lock: independent `z_stream` objects can be compressed in parallel. Each
individual stream must still be driven by one thread at a time, as zlib
requires.

Measured through the shim from four Python threads on four distinct 1.5 MB
inputs (12-core machine), outputs byte-identical to the serial runs:

| level | 1 thread | 4 threads | speedup |
|---|---|---|---|
| 1 | 0.35 s | 0.10 s | 3.68× |
| 3 | 4.38 s | 1.11 s | 3.94× |
| 6 | 7.82 s | 1.99 s | 3.93× |

## Known limitations

* **Not byte-identical to zlib.** The output is a valid deflate stream but a
  different one. Decompression is unaffected.
* **Small windows.** `windowBits` below 15 is accepted, but the stream may use
  the full 32 KiB window and the zlib header always advertises `CINFO=7`.
  Callers that request a small window (rare) are not supported.
* **Prime / custom header.** `deflatePrime` and `deflateSetHeader` return
  success but are ignored.
* **`Z_FIXED`.** Accepted but emits dynamic Huffman headers.
* **Tiny inputs.** A one-byte input costs 12 bytes versus stock zlib's 9 because
  the engine has no fixed-Huffman emitter. The empty input matches at 8 bytes.
* **Static linking / `dlsym`.** Consumers that bundle zlib statically, or
  resolve it with `dlopen`/`dlsym`, bypass the preload.

## Testing

`make test` runs `tests/smoke.py`, which drives real Python interpreters under
`LD_PRELOAD`:

* `zlib.compress()` round-trip at levels 0/1/3/6/9 on empty, tiny, zeros, text,
  mixed and random inputs;
* the shim actually interposes and beats stock on compressible data;
* streaming: `Z_SYNC_FLUSH` markers, raw and gzip wrappers, `deflateCopy`;
* a 5 MiB input exercising the one-shot `Z_FINISH` and auto-segmentation paths;
* independent streams compressed from eight Python threads, byte-identical to
  the serial results;
* `tests/abi.c` return codes and `compressBound`;
* `tests/stream_abi.c`: `deflateReset`, `deflateParams`, `deflateBound`,
  `deflatePending`, `deflateSetDictionary`/`deflateGetDictionary` (zlib
  DICTID, raw, oversized dictionaries, gzip rejection, reset semantics),
  custom allocators, interleaved streams, init error paths;
* `tests/concurrent.cpp`: the engine driven from six threads, every result
  checked against the serial output (also clean under ThreadSanitizer).

Additional manual checks (not committed): random-chunk fuzzing across all flush
modes and levels, and `zipfile` / `gzip` / `tarfile` round-trips.

## Verifying the engine is still exact

The fork's engine output was compared against the untouched reconstruction on
the 191-image corpus at `../pngout-re/work/imgs/`; all 191 deflate streams are
byte-identical.
