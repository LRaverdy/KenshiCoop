#include "kc/pack.h"

#include <windows.h>
#include <compressapi.h>

namespace kc {

bool PackBytes(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    if (!data || size == 0) return false;
    COMPRESSOR_HANDLE c = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &c)) return false;
    SIZE_T need = 0;
    // a first call with no buffer gives the size the packed data needs at most
    if (!Compress(c, data, size, nullptr, 0, &need) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) { CloseCompressor(c); return false; }
    out.resize(need ? need : size + 1024);
    SIZE_T got = 0;
    const bool ok = Compress(c, data, size, out.data(), out.size(), &got) != FALSE;
    CloseCompressor(c);
    if (!ok || got == 0 || got >= size) { out.clear(); return false; }
    out.resize(got);
    out.shrink_to_fit();
    return true;
}

bool UnpackBytes(const uint8_t* data, size_t size, size_t rawSize, std::vector<uint8_t>& out) {
    out.clear();
    if (!data || size == 0 || rawSize == 0) return false;
    DECOMPRESSOR_HANDLE d = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &d)) return false;
    out.resize(rawSize);
    SIZE_T got = 0;
    // the output buffer is exactly what the host announced: a stream claiming more is refused
    const bool ok = Decompress(d, data, size, out.data(), out.size(), &got) != FALSE;
    CloseDecompressor(d);
    if (!ok || got != rawSize) { out.clear(); return false; }
    return true;
}

} // namespace kc
