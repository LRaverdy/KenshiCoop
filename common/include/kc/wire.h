// Bounds-checked little-endian serialization.
// Every read is checked; a failed read latches `ok() == false` and returns zero values,
// so decoders can read a whole message and test `ok()` once at the end.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace kc {

class Writer {
public:
    explicit Writer(size_t reserve = 256) { buf_.reserve(reserve); }

    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { put(&v, 2); }
    void u32(uint32_t v) { put(&v, 4); }
    void u64(uint64_t v) { put(&v, 8); }
    void i32(int32_t v) { put(&v, 4); }
    void f32(float v) { put(&v, 4); }
    void f64(double v) { put(&v, 8); }
    void boolean(bool v) { u8(v ? 1 : 0); }

    // LEB128 unsigned varint.
    void varint(uint64_t v) {
        while (v >= 0x80) { u8(static_cast<uint8_t>(v | 0x80)); v >>= 7; }
        u8(static_cast<uint8_t>(v));
    }
    void str(std::string_view s) {
        varint(s.size());
        put(s.data(), s.size());
    }
    void bytes(const void* p, size_t n) { put(p, n); }

    const uint8_t* data() const { return buf_.data(); }
    size_t size() const { return buf_.size(); }
    std::vector<uint8_t>& vec() { return buf_; }
    void clear() { buf_.clear(); }

private:
    void put(const void* p, size_t n) {
        const size_t at = buf_.size();
        buf_.resize(at + n);
        if (n) std::memcpy(buf_.data() + at, p, n);
    }
    std::vector<uint8_t> buf_;
};

class Reader {
public:
    Reader(const void* data, size_t size) : p_(static_cast<const uint8_t*>(data)), n_(size) {}

    uint8_t u8() { uint8_t v = 0; get(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; get(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; get(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; get(&v, 8); return v; }
    int32_t i32() { int32_t v = 0; get(&v, 4); return v; }
    bool boolean() { return u8() != 0; }

    // Floats are rejected if non-finite: nothing NaN/Inf from the network ever reaches game memory.
    float f32() {
        float v = 0; get(&v, 4);
        if (!std::isfinite(v)) { ok_ = false; v = 0; }
        return v;
    }
    double f64() {
        double v = 0; get(&v, 8);
        if (!std::isfinite(v)) { ok_ = false; v = 0; }
        return v;
    }

    uint64_t varint() {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const uint8_t b = u8();
            if (!ok_) return 0;
            v |= uint64_t(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
        }
        ok_ = false;
        return 0;
    }
    // `max` bounds the length before any allocation happens.
    std::string str(size_t max) {
        const uint64_t len = varint();
        if (!ok_ || len > max || len > remaining()) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + pos_), static_cast<size_t>(len));
        pos_ += static_cast<size_t>(len);
        return s;
    }
    // Count prefix for arrays; `max` bounds it and each element must take at least `minElemSize` bytes.
    uint32_t count(uint32_t max, size_t minElemSize = 1) {
        const uint64_t c = varint();
        if (!ok_ || c > max || c * minElemSize > remaining()) { ok_ = false; return 0; }
        return static_cast<uint32_t>(c);
    }
    void bytes(void* out, size_t n) { get(out, n); }

    bool ok() const { return ok_; }
    bool atEnd() const { return pos_ == n_; }
    size_t remaining() const { return n_ - pos_; }
    void fail() { ok_ = false; }

private:
    void get(void* out, size_t n) {
        if (!ok_ || n > n_ - pos_) { ok_ = false; std::memset(out, 0, n); return; }
        std::memcpy(out, p_ + pos_, n);
        pos_ += n;
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
    bool ok_ = true;
};

static_assert(sizeof(float) == 4 && sizeof(double) == 8, "IEEE-754 floats required");

} // namespace kc
