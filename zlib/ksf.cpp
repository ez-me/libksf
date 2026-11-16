// libksf: a zlib-ABI compression shim backed by the reconstructed
// KZIP/PNGOUT deflate engine.
//
// Two layers are provided:
//
//   one-shot:   compress(), compress2(), compressBound() and the _z variants
//   streaming:  deflateInit_(), deflateInit2_(), deflate(), deflateEnd(),
//               deflateReset*(), deflateCopy(), deflateParams(),
//               deflateBound(), deflatePending(), deflateSetDictionary(),
//               deflateGetDictionary(), deflatePrime(), deflateSetHeader(),
//               deflateTune()
//
// Only the deflate side is provided. Every other zlib entry point
// (inflate*, crc32, adler32, gz*, uncompress*) resolves from the real libz, so
// the shim can be dropped in with LD_PRELOAD without replacing the library:
//
//     make
//     LD_PRELOAD=/abs/path/to/build/libksf.so python3 -c 'import zlib; ...'
//
// Note that LD_PRELOAD splits its value on spaces and colons, so the path must
// not contain spaces.
//
// Streaming model. The engine is a whole-buffer, multi-pass optimizer, so it
// cannot emit output incrementally at the same quality. deflate() therefore
// buffers input and compresses it when one of these happens:
//
//   * Z_FINISH              -> final segment (BFINAL set) plus wrapper trailer
//   * Z_SYNC_FLUSH et al.   -> non-final segment plus an empty stored marker
//   * 4 MiB of buffered input without a flush -> non-final segment
//
// Each segment is primed with the preceding 32 KiB of uncompressed data, so
// LZ77 matches cross segment boundaries exactly as they do in stock zlib. The
// decompressor already holds that window, so no extra signalling is needed.
// Z_FULL_FLUSH drops the history, matching zlib's promise that decompression
// can restart after the flush point. Memory is bounded by the 4 MiB segment
// limit, the 32 KiB history and at most 1 MiB of undelivered output.
//
// deflateSetDictionary() seeds the history before the first segment and, for
// the zlib wrapper, emits the DICTID that makes inflate() return Z_NEED_DICT.
// As in zlib, gzip does not support preset dictionaries and returns
// Z_STREAM_ERROR; raw streams use the dictionary without signalling it.
//
// The wrapper (raw / zlib / gzip) is chosen from windowBits exactly like zlib:
// 8..15 zlib, -8..-15 raw, 24..31 gzip, 40..47 zlib with a larger window.
//
// Level mapping. The engine's natural dial is the optimization pass budget
// (EncoderOptions::max_passes) plus the parser strategy. Measured on a 6.2 MB
// filtered image stream:
//
//   level  strategy  max_passes   output       time
//     0     stored        -       n + ~0.02%   fast
//     1       2           1       1,181,208    0.25 s
//     2       2           1       1,181,208    0.25 s
//     3       1           1         652,517    3.99 s
//     4       1           2         634,755    5.96 s
//     5       1           2         634,755    5.96 s
//     6       0           2         632,631    8.08 s
//     7       0           4         628,122   11.4  s
//     8       0          12         627,093   37.5  s
//     9       0        1024         627,093   37.5  s  (converges)
//
// Stock zlib -9 produced 717,351 bytes in 11.8 s on the same stream, so level
// 6 already wins on size and levels 8/9 win by about 12%.
//
// Threading. Every parser owns its scratch (Strategy12Parser has no static
// state), so independent deflate streams may be compressed concurrently. Each
// z_stream is still single-threaded, as zlib requires: do not call deflate() on
// the same stream from two threads at once. Sequential calls are also
// independent: 191 corpus inputs encoded back-to-back in one process produced
// byte-identical streams to fresh-process runs.

#include <zlib.h>

#include "block_splitter.hpp"
#include "strategy0_encoder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <vector>

#ifndef Z_TREES
#define Z_TREES 6
#endif

#if defined(__GNUC__)
#define KSF_PUBLIC extern "C" __attribute__((visibility("default")))
#else
#define KSF_PUBLIC extern "C"
#endif

namespace {

constexpr int kDefaultLevel = 6;

// Streaming segment size and output backpressure limit. A larger segment gives
// better ratio; a smaller one bounds latency and memory.
constexpr std::size_t kSegmentLimit = 4u << 20;
constexpr std::size_t kOutputLimit = 1u << 20;
// Maximum back-reference distance, and therefore the largest useful history.
constexpr std::size_t kWindowSize = 32768;

// ---------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------

bool verbose_enabled() {
   static const bool enabled = std::getenv("KSF_VERBOSE") != nullptr;
   return enabled;
}

std::uint32_t adler32_update(std::uint32_t adler, const std::uint8_t* data,
                             std::size_t size) {
   constexpr std::uint32_t kMod = 65521;
   std::uint32_t a = adler & 0xffffu;
   std::uint32_t b = (adler >> 16) & 0xffffu;
   while (size > 0) {
      const std::size_t chunk = size > 5552 ? 5552 : size;
      for (std::size_t i = 0; i < chunk; ++i) {
         a += data[i];
         b += a;
      }
      a %= kMod;
      b %= kMod;
      data += chunk;
      size -= chunk;
   }
   return (b << 16) | a;
}

const std::array<std::uint32_t, 256>& crc_table() {
   static const std::array<std::uint32_t, 256> table = [] {
      std::array<std::uint32_t, 256> values{};
      for (std::uint32_t i = 0; i < 256; ++i) {
         std::uint32_t c = i;
         for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1u) != 0 ? 0xedb88320u ^ (c >> 1) : c >> 1;
         }
         values[i] = c;
      }
      return values;
   }();
   return table;
}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data,
                           std::size_t size) {
   const std::array<std::uint32_t, 256>& table = crc_table();
   crc = ~crc;
   for (std::size_t i = 0; i < size; ++i) {
      crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
   }
   return ~crc;
}

// ---------------------------------------------------------------------------
// Wrapper emission
// ---------------------------------------------------------------------------

// Two-byte zlib header. FLEVEL mirrors the level bands used by stock zlib so
// the stream advertises a sensible "fastest/default/best" hint. FDICT marks a
// DICTID immediately after the header.
void append_zlib_header(std::vector<std::uint8_t>& out, int level, bool dict) {
   const int flevel = level <= 1 ? 0 : level <= 5 ? 1 : level == 6 ? 2 : 3;
   const std::uint8_t cmf = 0x78; // CM = 8 (deflate), CINFO = 7 (32 KiB window)
   std::uint8_t flg =
      static_cast<std::uint8_t>((flevel << 6) | (dict ? 0x20 : 0));
   const std::uint32_t header = (static_cast<std::uint32_t>(cmf) << 8) | flg;
   flg = static_cast<std::uint8_t>(flg + (31 - header % 31) % 31);
   out.push_back(cmf);
   out.push_back(flg);
}

void append_gzip_header(std::vector<std::uint8_t>& out) {
   static const std::uint8_t header[10] = {0x1f, 0x8b, 0x08, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x00, 0xff};
   out.insert(out.end(), header, header + sizeof header);
}

// Emit one or more stored deflate blocks. An empty input still needs a single
// block with LEN = 0. `final_block` controls BFINAL on the last block so a
// segment can continue an enclosing stream.
void append_stored(std::vector<std::uint8_t>& out, const std::uint8_t* data,
                   std::size_t size, bool final_block) {
   do {
      const std::size_t chunk = size > 0xffff ? 0xffff : size;
      const bool last = chunk == size;
      const std::uint16_t nlen = static_cast<std::uint16_t>(~chunk);
      out.push_back((final_block && last) ? 0x01 : 0x00); // BFINAL, BTYPE = 00
      out.push_back(static_cast<std::uint8_t>(chunk & 0xff));
      out.push_back(static_cast<std::uint8_t>((chunk >> 8) & 0xff));
      out.push_back(static_cast<std::uint8_t>(nlen & 0xff));
      out.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xff));
      if (chunk != 0) {
         out.insert(out.end(), data, data + chunk);
         data += chunk;
      }
      size -= chunk;
   } while (size > 0);
}

void append_u32_be(std::vector<std::uint8_t>& out, std::uint32_t value) {
   out.push_back(static_cast<std::uint8_t>(value >> 24));
   out.push_back(static_cast<std::uint8_t>(value >> 16));
   out.push_back(static_cast<std::uint8_t>(value >> 8));
   out.push_back(static_cast<std::uint8_t>(value));
}

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t value) {
   out.push_back(static_cast<std::uint8_t>(value));
   out.push_back(static_cast<std::uint8_t>(value >> 8));
   out.push_back(static_cast<std::uint8_t>(value >> 16));
   out.push_back(static_cast<std::uint8_t>(value >> 24));
}

// ---------------------------------------------------------------------------
// Level / strategy mapping
// ---------------------------------------------------------------------------

struct LevelPlan {
   int strategy;
   std::size_t max_passes;
};

LevelPlan plan_for_level(int level) {
   switch (level) {
   case 1:
   case 2:
      return LevelPlan{2, 1};
   case 3:
      return LevelPlan{1, 1};
   case 4:
   case 5:
      return LevelPlan{1, 2};
   case 6:
      return LevelPlan{0, 2};
   case 7:
      return LevelPlan{0, 4};
   case 8:
      return LevelPlan{0, 12};
   default:
      return LevelPlan{0, 1024};
   }
}

// zlib strategies map onto the engine's parser:
//   Z_HUFFMAN_ONLY -> strategy 3 (literal only)
//   Z_RLE          -> strategy 1 (closest cheap parser)
//   everything else -> the level's strategy (Z_FIXED still emits dynamic
//                     headers, which is valid but not the fixed-tree promise)
LevelPlan plan_for(int level, int zstrategy) {
   if (zstrategy == Z_HUFFMAN_ONLY) {
      return LevelPlan{3, 1};
   }
   if (zstrategy == Z_RLE) {
      return LevelPlan{1, 1};
   }
   return plan_for_level(level);
}

// Compress one independent segment. The engine's size-based stored fallback
// leaves EncoderResult::deflate holding the compressed pass for inspection, so
// the stored representation is emitted here instead. `end_bits` is the number
// of significant bits in the final byte before alignment padding (0 when the
// segment ends on a byte boundary).
struct Segment {
   std::vector<std::uint8_t> bytes;
   std::uint8_t end_bits{};
};

Segment encode_segment(std::span<const std::uint8_t> input, int level,
                       int zstrategy, std::span<const std::uint8_t> history,
                       bool final_block) {
   if (input.empty() && final_block) {
      // A fixed-Huffman block holding only EOB: 10 bits, two bytes. Smaller
      // than the five-byte empty stored block stock zlib also accepts.
      Segment segment;
      segment.bytes = {0x03, 0x00};
      segment.end_bits = 2;
      return segment;
   }
   if (level == 0) {
      Segment segment;
      segment.bytes.reserve(input.size() + input.size() / 0xffff * 5 + 8);
      append_stored(segment.bytes, input.data(), input.size(), final_block);
      return segment;
   }

   const LevelPlan plan = plan_for(level, zstrategy);
   kzip::EncoderOptions options;
   options.strategy = plan.strategy;
   options.max_passes = plan.max_passes;
   options.literal_only = zstrategy == Z_HUFFMAN_ONLY;
   options.final_block = final_block;
   options.history = history;

   const std::vector<std::size_t> ends =
      kzip::choose_block_ends(input, kzip::SplitOptions{});
   kzip::EncoderResult result = kzip::encode_strategy0(input, ends, options);

   if (verbose_enabled()) {
      std::fprintf(stderr,
                   "ksf: segment in=%zu out=%zu passes=%zu strategy=%d "
                   "max_passes=%zu end_bits=%u%s\n",
                   input.size(), result.deflate.size(), result.pass_count,
                   plan.strategy, plan.max_passes, result.end_bits,
                   result.stored ? " stored" : "");
   }

   if (!result.stored) {
      Segment segment;
      segment.bytes = std::move(result.deflate);
      segment.end_bits = result.end_bits;
      // The engine's own stored fallback is tied to its convergence loop and
      // does not always trigger (e.g. strategy 1 on incompressible input).
      // Fall back here whenever stored blocks are strictly smaller.
      const std::size_t blocks =
         input.empty() ? 1 : (input.size() + 0xffff) / 0x10000;
      const std::size_t stored_size = input.size() + blocks * 5;
      if (segment.bytes.size() > stored_size) {
         Segment stored;
         stored.bytes.reserve(stored_size);
         append_stored(stored.bytes, input.data(), input.size(), final_block);
         return stored;
      }
      return segment;
   }
   Segment segment;
   segment.bytes.reserve(input.size() + input.size() / 0xffff * 5 + 8);
   append_stored(segment.bytes, input.data(), input.size(), final_block);
   return segment;
}

// Append the empty stored block that marks a sync/full/partial flush.
//
// The engine byte-aligns each segment, so its padding zeros are the first bits
// of this block's 3-bit header (BFINAL = 0, BTYPE = 00). Depending on how many
// significant bits the segment used, those zeros already cover the header and
// the skip-to-byte-boundary, or one extra zero byte is needed:
//
//   end_bits 1..5  -> 00 00 FF FF      (header + skip already in the padding)
//   end_bits 0,6,7 -> 00 00 00 FF FF   (one more byte completes the header)
void append_sync_marker(std::vector<std::uint8_t>& out, std::uint8_t end_bits) {
   if (end_bits >= 1 && end_bits <= 5) {
      static const std::uint8_t marker[4] = {0x00, 0x00, 0xff, 0xff};
      out.insert(out.end(), marker, marker + 4);
   } else {
      static const std::uint8_t marker[5] = {0x00, 0x00, 0x00, 0xff, 0xff};
      out.insert(out.end(), marker, marker + 5);
   }
}

// ---------------------------------------------------------------------------
// Streaming state
// ---------------------------------------------------------------------------

struct DeflateState {
   int level{kDefaultLevel};
   int zstrategy{Z_DEFAULT_STRATEGY};
   bool raw{false};
   bool gzip{false};

   bool header_done{false};
   bool finished{false};
   bool need_flush{false};

   std::vector<std::uint8_t> pending;
   std::vector<std::uint8_t> output;
   std::size_t output_pos{0};

   // Up to 32 KiB of uncompressed bytes preceding `pending` in the same
   // stream. Passed to the engine so matches can cross segment boundaries.
   std::vector<std::uint8_t> history;
   // The dictionary supplied to deflateSetDictionary(), if any. `preset` is
   // its last 32 KiB and `dict_adler` covers the whole argument, exactly as
   // zlib's DICTID does. Both survive deflateResetKeep().
   std::vector<std::uint8_t> preset;
   std::uint32_t dict_adler{1};
   bool dict_set{false};

   std::uint32_t check{1}; // Adler-32 for zlib, CRC-32 for gzip
   // deflateUsed(): 0 before any flush, then 8 because every emitted segment
   // is byte-aligned.
   int bits_used{0};
};

void update_check(DeflateState& state, const std::uint8_t* data,
                  std::size_t size) {
   if (state.raw) {
      return;
   }
   if (state.gzip) {
      state.check = crc32_update(state.check, data, size);
   } else {
      state.check = adler32_update(state.check, data, size);
   }
}

// Remember the most recent 32 KiB of uncompressed input for the next segment.
void update_history(DeflateState& state, const std::vector<std::uint8_t>& data) {
   if (data.size() >= kWindowSize) {
      state.history.assign(data.end() - kWindowSize, data.end());
      return;
   }
   const std::size_t total = state.history.size() + data.size();
   if (total > kWindowSize) {
      const std::size_t excess = total - kWindowSize;
      state.history.erase(state.history.begin(),
                          state.history.begin() + excess);
   }
   state.history.insert(state.history.end(), data.begin(), data.end());
}

void append_wrapper_header(DeflateState& state) {
   if (state.raw) {
      return;
   }
   if (state.gzip) {
      append_gzip_header(state.output);
      return;
   }
   append_zlib_header(state.output, state.level, state.dict_set);
   if (state.dict_set) {
      // DICTID: Adler-32 of the full dictionary argument. inflate() reports
      // Z_NEED_DICT and the caller supplies the dictionary again.
      append_u32_be(state.output, state.dict_adler);
   }
}

void append_wrapper_trailer(DeflateState& state, uLong total_in) {
   if (state.raw) {
      return;
   }
   if (state.gzip) {
      append_u32_le(state.output, state.check);
      append_u32_le(state.output, static_cast<std::uint32_t>(total_in));
   } else {
      append_u32_be(state.output, state.check);
   }
}

void* default_alloc(voidpf /*opaque*/, uInt items, uInt size) {
   return std::malloc(static_cast<std::size_t>(items) * size);
}

void default_free(voidpf /*opaque*/, voidpf address) {
   std::free(address);
}

int deflate_impl(z_streamp strm, int flush) {
   if (strm == nullptr || strm->state == nullptr) {
      return Z_STREAM_ERROR;
   }
   DeflateState& state = *reinterpret_cast<DeflateState*>(strm->state);
   if (flush < 0 || flush > Z_TREES) {
      return Z_STREAM_ERROR;
   }
   if (strm->next_out == nullptr ||
       (strm->next_in == nullptr && strm->avail_in != 0)) {
      return Z_STREAM_ERROR;
   }
   if (strm->avail_out == 0) {
      return Z_BUF_ERROR;
   }

   const bool finishing = flush == Z_FINISH;
   const bool flushing = flush == Z_SYNC_FLUSH || flush == Z_FULL_FLUSH ||
                         flush == Z_PARTIAL_FLUSH || flush == Z_BLOCK ||
                         flush == Z_TREES;

   // 1. Consume input, with backpressure when too much output is undelivered.
   //    At most kSegmentLimit bytes are buffered, so a single huge avail_in is
   //    taken in segment-sized pieces over successive calls.
   bool consumed = false;
   const std::size_t undelivered = state.output.size() - state.output_pos;
   if (!state.finished && strm->avail_in != 0 && undelivered < kOutputLimit) {
      std::size_t size = strm->avail_in;
      // Bound buffering for plain streaming, but never cap a Z_FINISH call:
      // callers such as CPython's zlib.compress() pass the whole buffer with
      // Z_FINISH and do not loop on avail_in. The engine needs the whole input
      // for a final segment anyway.
      if (!finishing && state.pending.size() + size > kSegmentLimit) {
         size = kSegmentLimit - state.pending.size();
      }
      if (size != 0) {
         const std::uint8_t* data = strm->next_in;
         state.pending.insert(state.pending.end(), data, data + size);
         update_check(state, data, size);
         strm->adler = state.check;
         strm->total_in += size;
         strm->next_in += size;
         strm->avail_in -= static_cast<uInt>(size);
         state.need_flush = true;
         consumed = true;
      }
   }

   // 2. Produce a segment if the buffered input is being flushed, finished, or
   //    has reached the segment limit. Every non-final segment is followed by
   //    an empty stored block: it implements the flush for Z_SYNC_FLUSH and,
   //    for automatic segments, it restores byte alignment so the next
   //    segment's block header is not swallowed by the alignment padding.
   bool produced = false;
   if (!state.finished) {
      // Only finalize once every byte of input has been consumed. With
      // Z_FINISH and a large avail_in, earlier calls emit non-final segments
      // and the caller loops until avail_in reaches zero.
      const bool want_final = finishing && strm->avail_in == 0;
      const bool want_flush = flushing && state.need_flush;
      const bool want_auto = state.pending.size() >= kSegmentLimit;
      if (want_final || want_flush || want_auto) {
         if (!state.header_done) {
            append_wrapper_header(state);
            state.header_done = true;
            // Once the header is out, adler/crc covers input only, not the
            // dictionary.
            strm->adler = state.check;
         }
         if (!state.pending.empty()) {
            Segment segment = encode_segment(
               std::span<const std::uint8_t>(state.pending.data(),
                                             state.pending.size()),
               state.level, state.zstrategy,
               std::span<const std::uint8_t>(state.history), want_final);
            state.output.insert(state.output.end(), segment.bytes.begin(),
                                segment.bytes.end());
            update_history(state, state.pending);
            state.pending.clear();
            if (!want_final) {
               append_sync_marker(state.output, segment.end_bits);
            }
         } else if (want_final) {
            // Fixed-Huffman block holding only EOB, two bytes.
            state.output.push_back(0x03);
            state.output.push_back(0x00);
         } else {
            // Flush with no new input: the stream is already byte-aligned
            // because the previous segment ended with a marker.
            append_sync_marker(state.output, 0);
         }
         if (flush == Z_FULL_FLUSH) {
            // zlib clears its hash chains here so decompression can restart
            // after this point without any preceding history.
            state.history.clear();
         }
         if (want_final) {
            append_wrapper_trailer(state, strm->total_in);
            state.finished = true;
         }
         state.need_flush = false;
         state.bits_used = 8;
         produced = true;
      }
   }

   // 3. Drain buffered output into the caller's buffer.
   const std::size_t available = state.output.size() - state.output_pos;
   const std::size_t copied =
      std::min<std::size_t>(available, strm->avail_out);
   if (copied != 0) {
      std::memcpy(strm->next_out, state.output.data() + state.output_pos,
                  copied);
      state.output_pos += copied;
      strm->next_out += copied;
      strm->avail_out -= static_cast<uInt>(copied);
      strm->total_out += copied;
   }
   if (state.output_pos == state.output.size()) {
      state.output.clear();
      state.output_pos = 0;
   }

   // 4. Status.
   if (state.finished && state.output.empty()) {
      strm->data_type = Z_BINARY;
      return Z_STREAM_END;
   }
   if (copied == 0 && !consumed && !produced) {
      return Z_BUF_ERROR;
   }
   return Z_OK;
}

// ---------------------------------------------------------------------------
// One-shot API
// ---------------------------------------------------------------------------

int do_compress2(std::uint8_t* dest, uLongf* dest_len,
                 const std::uint8_t* source, std::size_t source_len,
                 int level) {
   if (dest == nullptr || dest_len == nullptr) {
      return Z_STREAM_ERROR;
   }
   if (source == nullptr && source_len != 0) {
      return Z_STREAM_ERROR;
   }
   if (level == Z_DEFAULT_COMPRESSION) {
      level = kDefaultLevel;
   }
   if (level < 0 || level > 9) {
      return Z_STREAM_ERROR;
   }

   std::vector<std::uint8_t> out;
   out.reserve(source_len + (source_len >> 12) + 64);
   append_zlib_header(out, level, false);

   if (level == 0) {
      append_stored(out, source, source_len, true);
   } else {
      const std::span<const std::uint8_t> input =
         source_len != 0 ? std::span<const std::uint8_t>(source, source_len)
                         : std::span<const std::uint8_t>();
      Segment segment =
         encode_segment(input, level, Z_DEFAULT_STRATEGY, {}, true);
      out.insert(out.end(), segment.bytes.begin(), segment.bytes.end());
   }

   append_u32_be(out, adler32_update(1, source, source_len));

   if (out.size() > *dest_len) {
      return Z_BUF_ERROR;
   }
   std::memcpy(dest, out.data(), out.size());
   *dest_len = static_cast<uLongf>(out.size());
   return Z_OK;
}

} // namespace

// ---------------------------------------------------------------------------
// Exported zlib entry points
// ---------------------------------------------------------------------------

KSF_PUBLIC int compress(Bytef* dest, uLongf* destLen, const Bytef* source,
                        uLong sourceLen) {
   return do_compress2(dest, destLen, source,
                       static_cast<std::size_t>(sourceLen),
                       Z_DEFAULT_COMPRESSION);
}

KSF_PUBLIC int compress2(Bytef* dest, uLongf* destLen, const Bytef* source,
                         uLong sourceLen, int level) {
   return do_compress2(dest, destLen, source,
                       static_cast<std::size_t>(sourceLen), level);
}

KSF_PUBLIC int compress_z(Bytef* dest, z_size_t* destLen, const Bytef* source,
                          z_size_t sourceLen) {
   if (destLen == nullptr) {
      return Z_STREAM_ERROR;
   }
   uLongf size = static_cast<uLongf>(*destLen);
   const int status =
      do_compress2(dest, &size, source, static_cast<std::size_t>(sourceLen),
                   Z_DEFAULT_COMPRESSION);
   if (status == Z_OK) {
      *destLen = static_cast<z_size_t>(size);
   }
   return status;
}

KSF_PUBLIC int compress2_z(Bytef* dest, z_size_t* destLen, const Bytef* source,
                           z_size_t sourceLen, int level) {
   if (destLen == nullptr) {
      return Z_STREAM_ERROR;
   }
   uLongf size = static_cast<uLongf>(*destLen);
   const int status =
      do_compress2(dest, &size, source, static_cast<std::size_t>(sourceLen),
                   level);
   if (status == Z_OK) {
      *destLen = static_cast<z_size_t>(size);
   }
   return status;
}

KSF_PUBLIC uLong compressBound(uLong sourceLen) {
   // Stock zlib's bound, which is comfortably larger than our worst case
   // (stored blocks add 5 bytes per 65535 input bytes).
   return sourceLen + (sourceLen >> 12) + (sourceLen >> 14) +
          (sourceLen >> 25) + 13;
}

KSF_PUBLIC z_size_t compressBound_z(z_size_t sourceLen) {
   return static_cast<z_size_t>(compressBound(static_cast<uLong>(sourceLen)));
}

KSF_PUBLIC int deflateInit_(z_streamp strm, int level, const char* version,
                            int stream_size) {
   return deflateInit2_(strm, level, Z_DEFLATED, MAX_WBITS, 8,
                        Z_DEFAULT_STRATEGY, version, stream_size);
}

KSF_PUBLIC int deflateInit2_(z_streamp strm, int level, int method,
                             int windowBits, int memLevel, int strategy,
                             const char* version, int stream_size) {
   if (version == nullptr || version[0] != ZLIB_VERSION[0] ||
       stream_size != static_cast<int>(sizeof(z_stream))) {
      return Z_VERSION_ERROR;
   }
   if (strm == nullptr) {
      return Z_STREAM_ERROR;
   }
   if (method != Z_DEFLATED || memLevel < 1 || memLevel > MAX_MEM_LEVEL ||
       strategy < 0 || strategy > Z_FIXED) {
      return Z_STREAM_ERROR;
   }
   if (level == Z_DEFAULT_COMPRESSION) {
      level = kDefaultLevel;
   }
   if (level < 0 || level > 9) {
      return Z_STREAM_ERROR;
   }

   bool raw = false;
   bool gzip = false;
   int wbits = windowBits;
   if (windowBits < 0) {
      raw = true;
      wbits = -windowBits;
   } else if (windowBits > 15) {
      if (windowBits >= 24 && windowBits <= 31) {
         gzip = true;
         wbits = windowBits - 16;
      } else if (windowBits >= 40 && windowBits <= 47) {
         wbits = windowBits - 32;
      } else {
         return Z_STREAM_ERROR;
      }
   }
   if (wbits < 8 || wbits > MAX_WBITS) {
      return Z_STREAM_ERROR;
   }

   strm->msg = nullptr;
   if (strm->zalloc == nullptr) {
      strm->zalloc = default_alloc;
      strm->zfree = default_free;
      strm->opaque = nullptr;
   }

   voidpf memory = strm->zalloc(strm->opaque, 1, sizeof(DeflateState));
   if (memory == nullptr) {
      return Z_MEM_ERROR;
   }
   DeflateState* state = new (memory) DeflateState();
   state->level = level;
   state->zstrategy = strategy;
   state->raw = raw;
   state->gzip = gzip;
   state->check = gzip ? 0u : 1u;

   strm->state = reinterpret_cast<internal_state*>(state);
   strm->total_in = 0;
   strm->total_out = 0;
   strm->data_type = Z_UNKNOWN;
   strm->adler = state->check;
   return Z_OK;
}

KSF_PUBLIC int deflate(z_streamp strm, int flush) {
   return deflate_impl(strm, flush);
}

KSF_PUBLIC int deflateEnd(z_streamp strm) {
   if (strm == nullptr || strm->state == nullptr) {
      return Z_STREAM_ERROR;
   }
   DeflateState* state = reinterpret_cast<DeflateState*>(strm->state);
   state->~DeflateState();
   strm->zfree(strm->opaque, state);
   strm->state = nullptr;
   return Z_OK;
}

static int deflate_reset_impl(z_streamp strm, bool keep) {
   if (strm == nullptr || strm->state == nullptr) {
      return Z_STREAM_ERROR;
   }
   DeflateState& state = *reinterpret_cast<DeflateState*>(strm->state);
   state.header_done = false;
   state.finished = false;
   state.need_flush = false;
   state.pending.clear();
   state.output.clear();
   state.output_pos = 0;
   state.check = state.gzip ? 0u : 1u;
   state.bits_used = 0;
   strm->total_in = 0;
   strm->total_out = 0;
   strm->adler = state.check;
   strm->data_type = Z_UNKNOWN;

   if (!keep) {
      state.history.clear();
      state.preset.clear();
      state.dict_set = false;
      state.dict_adler = 1;
   } else if (!state.raw) {
      // deflateResetKeep() promises to preserve the window, but a fresh zlib
      // header can only describe a preset dictionary: bytes from the previous
      // stream would make the new stream undecodable. Keep exactly that.
      state.history = state.dict_set ? state.preset
                                     : std::vector<std::uint8_t>();
   }
   return Z_OK;
}

KSF_PUBLIC int deflateReset(z_streamp strm) {
   return deflate_reset_impl(strm, false);
}

KSF_PUBLIC int deflateResetKeep(z_streamp strm) {
   return deflate_reset_impl(strm, true);
}

KSF_PUBLIC int deflateCopy(z_streamp dest, z_streamp source) {
   if (dest == nullptr || source == nullptr || source->state == nullptr) {
      return Z_STREAM_ERROR;
   }
   const DeflateState& original =
      *reinterpret_cast<const DeflateState*>(source->state);
   *dest = *source;
   if (dest->zalloc == nullptr) {
      dest->zalloc = default_alloc;
      dest->opaque = nullptr;
   }
   if (dest->zfree == nullptr) {
      dest->zfree = default_free;
   }
   voidpf memory = dest->zalloc(dest->opaque, 1, sizeof(DeflateState));
   if (memory == nullptr) {
      dest->state = nullptr;
      return Z_MEM_ERROR;
   }
   DeflateState* copy = new (memory) DeflateState(original);
   dest->state = reinterpret_cast<internal_state*>(copy);
   return Z_OK;
}

KSF_PUBLIC uLong deflateBound(z_streamp /*strm*/, uLong sourceLen) {
   return compressBound(sourceLen) + 32;
}

KSF_PUBLIC z_size_t deflateBound_z(z_streamp /*strm*/, z_size_t sourceLen) {
   return static_cast<z_size_t>(
      compressBound(static_cast<uLong>(sourceLen)) + 32);
}

KSF_PUBLIC int deflateUsed(z_streamp strm, int* bits) {
   if (strm == nullptr || strm->state == nullptr) {
      return Z_STREAM_ERROR;
   }
   if (bits != nullptr) {
      *bits = reinterpret_cast<const DeflateState*>(strm->state)->bits_used;
   }
   return Z_OK;
}

KSF_PUBLIC int deflatePending(z_streamp strm, unsigned* pending, int* bits) {
   if (strm == nullptr || strm->state == nullptr) {
      return Z_STREAM_ERROR;
   }
   const DeflateState& state = *reinterpret_cast<const DeflateState*>(strm->state);
   if (pending != nullptr) {
      *pending = static_cast<unsigned>(state.output.size() - state.output_pos);
   }
   if (bits != nullptr) {
      *bits = 0;
   }
   return Z_OK;
}

KSF_PUBLIC int deflateParams(z_streamp strm, int level, int strategy) {
   if (strm == nullptr || strm->state == nullptr) {
      return Z_STREAM_ERROR;
   }
   if (level == Z_DEFAULT_COMPRESSION) {
      level = kDefaultLevel;
   }
   if (level < 0 || level > 9 || strategy < 0 || strategy > Z_FIXED) {
      return Z_STREAM_ERROR;
   }
   DeflateState& state = *reinterpret_cast<DeflateState*>(strm->state);
   state.level = level;
   state.zstrategy = strategy;
   return Z_OK;
}

KSF_PUBLIC int deflateTune(z_streamp strm, int good_length, int max_lazy,
                           int nice_length, int max_chain) {
   (void)good_length;
   (void)max_lazy;
   (void)nice_length;
   (void)max_chain;
   return strm != nullptr && strm->state != nullptr ? Z_OK : Z_STREAM_ERROR;
}

KSF_PUBLIC int deflateSetDictionary(z_streamp strm, const Bytef* dictionary,
                                    uInt dictLength) {
   if (strm == nullptr || strm->state == nullptr || dictionary == nullptr) {
      return Z_STREAM_ERROR;
   }
   DeflateState& state = *reinterpret_cast<DeflateState*>(strm->state);
   // gzip has no DICTID field, so zlib refuses preset dictionaries for it.
   if (state.gzip) {
      return Z_STREAM_ERROR;
   }
   if (state.raw) {
      // Raw deflate may set a dictionary between blocks, but not while input
      // is still unprocessed (zlib checks lookahead for the same reason).
      if (!state.pending.empty()) {
         return Z_STREAM_ERROR;
      }
   } else if (state.header_done || strm->total_in != 0 ||
              !state.pending.empty()) {
      // The zlib header carries the dictionary checksum, so the dictionary
      // must be set before the first byte is emitted.
      return Z_STREAM_ERROR;
   }
   if (dictLength == 0) {
      return Z_OK;
   }

   const std::uint8_t* bytes = dictionary;
   state.dict_adler = adler32_update(1, bytes, dictLength);
   state.dict_set = true;
   state.preset.assign(
      bytes + (dictLength > kWindowSize ? dictLength - kWindowSize : 0),
      bytes + dictLength);

   if (state.raw && !state.history.empty()) {
      // Raw mid-stream: zlib appends the dictionary to the existing window,
      // or replaces the window when the dictionary fills it.
      if (dictLength >= kWindowSize) {
         state.history = state.preset;
      } else {
         state.history.insert(state.history.end(), bytes, bytes + dictLength);
         if (state.history.size() > kWindowSize) {
            const std::size_t excess = state.history.size() - kWindowSize;
            state.history.erase(state.history.begin(),
                                state.history.begin() + excess);
         }
      }
   } else {
      state.history = state.preset;
   }

   if (!state.raw) {
      // Until the header is written, zlib reports the dictionary checksum in
      // strm->adler.
      strm->adler = state.dict_adler;
   }
   return Z_OK;
}

KSF_PUBLIC int deflateGetDictionary(z_streamp strm, Bytef* dictionary,
                                    uInt* dictLength) {
   if (strm == nullptr || strm->state == nullptr) {
      return Z_STREAM_ERROR;
   }
   const DeflateState& state =
      *reinterpret_cast<const DeflateState*>(strm->state);
   const std::size_t total = state.history.size() + state.pending.size();
   const std::size_t len = std::min(total, kWindowSize);
   if (dictionary != nullptr && len != 0) {
      // The window ends at the end of pending, so take the tail of history
      // followed by as much of pending as fits.
      std::size_t written = 0;
      if (state.pending.size() < len) {
         const std::size_t from_history = len - state.pending.size();
         std::memcpy(dictionary,
                     state.history.data() + state.history.size() -
                        from_history,
                     from_history);
         written = from_history;
      }
      const std::size_t from_pending =
         std::min(len - written, state.pending.size());
      if (from_pending != 0) {
         std::memcpy(dictionary + written,
                     state.pending.data() + state.pending.size() -
                        from_pending,
                     from_pending);
      }
   }
   if (dictLength != nullptr) {
      *dictLength = static_cast<uInt>(len);
   }
   return Z_OK;
}

KSF_PUBLIC int deflatePrime(z_streamp strm, int bits, int value) {
   (void)bits;
   (void)value;
   return strm != nullptr && strm->state != nullptr ? Z_OK : Z_STREAM_ERROR;
}

KSF_PUBLIC int deflateSetHeader(z_streamp strm, gz_headerp head) {
   (void)head;
   // Custom gzip headers are ignored; the default header is still valid.
   return strm != nullptr && strm->state != nullptr ? Z_OK : Z_STREAM_ERROR;
}
