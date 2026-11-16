/* History (dictionary priming) test.
 *
 * The parser can be told about up to 32 KiB of bytes that precede its input in
 * the same deflate stream. This test checks that:
 *
 *   - a body encoded with a dictionary round-trips through zlib's raw inflate
 *     with the same dictionary, and is smaller than the same body encoded
 *     without one;
 *   - two non-final/final segments with chained dictionaries concatenate into
 *     one valid stream (the sync marker between them is correct).
 *
 * Links the engine sources directly plus zlib for decompression.
 */
#include "block_splitter.hpp"
#include "strategy0_encoder.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

std::vector<std::uint8_t> make_data(std::size_t size, std::uint32_t seed) {
   std::vector<std::uint8_t> data(size);
   std::uint32_t state = seed;
   for (std::size_t i = 0; i < size; ++i) {
      state = state * 1664525u + 1013904223u;
      // Mix a small alphabet with occasional noise so matches exist.
      data[i] = (i % 11 == 0) ? static_cast<std::uint8_t>(state >> 24)
                              : static_cast<std::uint8_t>((i / 3 + seed) & 0x1f);
   }
   return data;
}

struct Segment {
   std::vector<std::uint8_t> bytes;
   std::uint8_t end_bits{0};
};

Segment encode(std::span<const std::uint8_t> input,
               std::span<const std::uint8_t> history, bool final_block) {
   kzip::EncoderOptions options;
   options.strategy = 0;
   options.max_passes = 2;
   options.final_block = final_block;
   options.history = history;
   const std::vector<std::size_t> ends =
      kzip::choose_block_ends(input, kzip::SplitOptions{});
   kzip::EncoderResult result = kzip::encode_strategy0(input, ends, options);
   return Segment{std::move(result.deflate), result.end_bits};
}

// Append the empty stored block that marks a sync point, matching the
// ksflate_compress() wrapper.
void append_sync_marker(std::vector<std::uint8_t>& out, std::uint8_t end_bits) {
   if (end_bits >= 1 && end_bits <= 5) {
      static const std::uint8_t marker[4] = {0x00, 0x00, 0xff, 0xff};
      out.insert(out.end(), marker, marker + 4);
   }
   else {
      static const std::uint8_t marker[5] = {0x00, 0x00, 0x00, 0xff, 0xff};
      out.insert(out.end(), marker, marker + 5);
   }
}

bool inflate_with_dictionary(const std::vector<std::uint8_t>& packed,
                             const std::vector<std::uint8_t>& dictionary,
                             std::vector<std::uint8_t>& out) {
   z_stream strm{};
   if (inflateInit2(&strm, -15) != Z_OK)
      return false;
   if (!dictionary.empty() &&
       inflateSetDictionary(&strm, dictionary.data(),
                            static_cast<unsigned>(dictionary.size())) != Z_OK) {
      inflateEnd(&strm);
      return false;
   }
   out.assign(1024 * 1024, 0);
   strm.next_in = const_cast<Bytef*>(packed.data());
   strm.avail_in = static_cast<unsigned>(packed.size());
   strm.next_out = out.data();
   strm.avail_out = static_cast<unsigned>(out.size());
   const int ret = inflate(&strm, Z_FINISH);
   const std::size_t produced = out.size() - strm.avail_out;
   out.resize(produced);
   inflateEnd(&strm);
   return ret == Z_STREAM_END;
}

std::vector<std::uint8_t> tail(std::span<const std::uint8_t> data,
                               std::size_t count) {
   if (count > data.size())
      count = data.size();
   return std::vector<std::uint8_t>(data.end() - count, data.end());
}

} // namespace

int main() {
   const std::vector<std::uint8_t> dictionary = make_data(32768, 0x1234);
   std::vector<std::uint8_t> body = make_data(200000, 0x5678);
   // Make the body depend on the dictionary: copy recognizable runs from it.
   for (std::size_t i = 0; i < 16384; ++i)
      body[i] = dictionary[9000 + (i % 20000)];

   const Segment plain = encode(body, {}, true);
   const Segment primed = encode(body, dictionary, true);

   std::vector<std::uint8_t> round;
   if (!inflate_with_dictionary(primed.bytes, dictionary, round)) {
      std::printf("history: inflate failed\n");
      return 1;
   }
   if (round != body) {
      std::printf("history: round-trip mismatch (%zu vs %zu)\n", round.size(),
                  body.size());
      return 1;
   }
   if (!(primed.bytes.size() < plain.bytes.size())) {
      std::printf("history: priming did not help (%zu >= %zu)\n",
                  primed.bytes.size(), plain.bytes.size());
      return 1;
   }

   // Two segments: the second is primed with the tail of the first, exactly as
   // a parallel compressor would chain chunks.
   const std::size_t split = body.size() / 2;
   const Segment first =
      encode(std::span<const std::uint8_t>(body.data(), split), dictionary,
             false);
   std::vector<std::uint8_t> chained = dictionary;
   chained.insert(chained.end(), body.begin(), body.begin() + split);
   const Segment second = encode(
      std::span<const std::uint8_t>(body.data() + split, body.size() - split),
      tail(chained, 32768), true);

   std::vector<std::uint8_t> joined = first.bytes;
   append_sync_marker(joined, first.end_bits);
   joined.insert(joined.end(), second.bytes.begin(), second.bytes.end());
   if (!inflate_with_dictionary(joined, dictionary, round)) {
      std::printf("history: chained inflate failed\n");
      return 1;
   }
   if (round != body) {
      std::printf("history: chained round-trip mismatch (%zu vs %zu)\n",
                  round.size(), body.size());
      return 1;
   }

   std::printf("history ok: plain=%zu primed=%zu chained=%zu\n",
               plain.bytes.size(), primed.bytes.size(), joined.size());
   return 0;
}
