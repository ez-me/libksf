/* Concurrent engine test.
 *
 * Strategy12Parser used to keep its scratch in process-wide statics, so two
 * simultaneous compressions would corrupt each other. The parser now owns that
 * state, so this test compresses the same set of inputs from several threads and
 * checks every result against the serial output.
 *
 * Links the engine sources directly (no zlib involvement).
 */
#include "strategy0_encoder.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

std::vector<std::uint8_t> make_input(int seed, std::size_t size) {
    std::vector<std::uint8_t> data(size);
    std::uint32_t state = 0x9e3779b9u ^ (static_cast<std::uint32_t>(seed) * 2654435761u);
    for (std::size_t i = 0; i < size; ++i) {
        state = state * 1664525u + 1013904223u;
        // Mix compressible runs with incompressible noise.
        if ((i / 97 + static_cast<std::size_t>(seed)) % 3 == 0) {
            data[i] = static_cast<std::uint8_t>(state >> 24);
        } else {
            data[i] = static_cast<std::uint8_t>((i * 7 + static_cast<std::size_t>(seed)) & 0xff);
        }
    }
    return data;
}

std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& input,
                                   int strategy, std::size_t passes) {
    std::vector<std::size_t> ends{input.size()};
    kzip::EncoderOptions options;
    options.strategy = strategy;
    options.max_passes = passes;
    return kzip::encode_strategy0(input, ends, options).deflate;
}

} // namespace

int main() {
    constexpr int kInputs = 8;
    constexpr int kThreads = 6;
    constexpr int kRounds = 2;

    std::vector<std::vector<std::uint8_t>> inputs;
    inputs.reserve(kInputs);
    for (int i = 0; i < kInputs; ++i) {
        inputs.push_back(make_input(i, 4096 + static_cast<std::size_t>(i) * 12000));
    }

    // Serial references for every strategy. Strategy 0 exercises the parser's
    // row arena and best-distance frontier, which are the state that used to be
    // static.
    std::vector<std::vector<std::vector<std::uint8_t>>> expected(3);
    for (int strategy = 0; strategy < 3; ++strategy) {
        expected[strategy].reserve(kInputs);
        for (const auto& input : inputs) {
            expected[strategy].push_back(compress(input, strategy, 2));
        }
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int round = 0; round < kRounds; ++round) {
                for (int j = 0; j < kInputs; ++j) {
                    const int index = (j + t * 3 + round) % kInputs;
                    const int strategy = (round + t) % 3;
                    const std::vector<std::uint8_t> out =
                        compress(inputs[index], strategy, 2);
                    if (out != expected[strategy][index]) {
                        ++failures;
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    if (failures.load() != 0) {
        std::printf("%d concurrent mismatch(es)\n", failures.load());
        return 1;
    }
    std::printf("concurrent ok: %d inputs, %d threads, %d rounds\n", kInputs,
                kThreads, kRounds);
    return 0;
}
