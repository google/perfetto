// Copyright (C) 2026 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
#ifndef BENCHMARKS_PIPELINE_PROTOCOL_H_
#define BENCHMARKS_PIPELINE_PROTOCOL_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <ostream>

namespace pipeline_benchmark {

// Both native runners consume every cell through this sink. Text encoding is
// enabled only for the untimed validation query. Timing includes the same hash
// work in both engines. Summing per-row hashes makes the checksum insensitive
// to SQL output ordering; exact untimed rows remain the correctness oracle.
class Sink {
 public:
  explicit Sink(std::ostream* output = nullptr) : output_(output) {}

  void Null() {
    Byte('N');
    if (output_) {
      Field();
      *output_ << 'N';
    }
  }
  void Integer(int64_t value) {
    Byte('I');
    Word(static_cast<uint64_t>(value));
    if (output_) {
      Field();
      *output_ << "I:" << std::dec << value;
    }
  }
  void Double(double value) {
    Byte('F');
    uint64_t bits;
    static_assert(sizeof(bits) == sizeof(value), "double must be 64 bits");
    memcpy(&bits, &value, sizeof(bits));
    Word(bits);
    if (output_) {
      Field();
      *output_ << "F:" << std::hexfloat << value;
    }
  }
  void String(const char* data, size_t size) {
    Byte('S');
    Word(static_cast<uint64_t>(size));
    for (size_t i = 0; i < size; ++i)
      Byte(static_cast<uint8_t>(data[i]));
    if (output_) {
      Field();
      *output_ << "S:";
      constexpr char hex[] = "0123456789abcdef";
      for (size_t i = 0; i < size; ++i) {
        auto ch = static_cast<uint8_t>(data[i]);
        *output_ << hex[ch >> 4] << hex[ch & 15];
      }
    }
  }
  void EndRow() {
    Byte('\n');
    checksum_ += row_hash_;
    row_hash_ = UINT64_C(14695981039346656037);
    ++rows_;
    if (output_) {
      *output_ << '\n';
      fields_ = 0;
    }
  }
  uint64_t checksum() const { return checksum_; }
  uint64_t rows() const { return rows_; }

 private:
  void Byte(uint8_t value) {
    row_hash_ ^= value;
    row_hash_ *= UINT64_C(1099511628211);
  }
  void Word(uint64_t value) {
    for (uint32_t i = 0; i < 8; ++i) {
      Byte(static_cast<uint8_t>(value));
      value >>= 8;
    }
  }
  void Field() {
    if (fields_++)
      *output_ << '\t';
  }
  std::ostream* output_;
  uint64_t checksum_ = 0;
  uint64_t row_hash_ = UINT64_C(14695981039346656037);
  uint64_t rows_ = 0;
  uint32_t fields_ = 0;
};

}  // namespace pipeline_benchmark
#endif  // BENCHMARKS_PIPELINE_PROTOCOL_H_
