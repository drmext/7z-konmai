// DDR ARC LZ77 decompressor (from examples/lz77-cpp; decompress-only).

#pragma once

#include <cstdint>
#include <vector>

namespace util {
namespace lz77 {

std::vector<uint8_t> decompress(uint8_t *input, size_t input_length);

} // namespace lz77
} // namespace util
