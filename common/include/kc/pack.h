// Compression of the world files sent to a joining player (Windows' own XPRESS Huffman codec,
// cabinet.dll: no extra library to ship). A Kenshi save is mostly text and repeated records: it
// packs to a fraction of its size, which is what crosses the Steam relay.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kc {

// Packs `size` bytes into `out`. False (out cleared) when the codec fails or the result would not
// be smaller than the input: the file is then sent as it is.
bool PackBytes(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
// Unpacks into exactly `rawSize` bytes. False when the data is not a valid packed stream of that size.
bool UnpackBytes(const uint8_t* data, size_t size, size_t rawSize, std::vector<uint8_t>& out);

} // namespace kc
