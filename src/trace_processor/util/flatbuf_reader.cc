/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/util/flatbuf_reader.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#include "perfetto/base/compiler.h"

namespace perfetto::trace_processor::util {

// static
uint16_t FlatBufferReader::ReadU16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}

// static
uint32_t FlatBufferReader::ReadU32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

const uint8_t* FlatBufferReader::FollowOffset(const uint8_t* p) const {
  uint32_t rel = ReadU32(p);
  const uint8_t* target = p + rel;
  if (PERFETTO_UNLIKELY(target < buf_ || target >= buf_ + buf_size_)) {
    return nullptr;
  }
  return target;
}

const uint8_t* FlatBufferReader::FieldRaw(uint32_t field_index) const {
  if (PERFETTO_UNLIKELY(!table_)) {
    return nullptr;
  }
  // Table starts with a signed offset backwards to its vtable.
  int32_t soff;
  memcpy(&soff, table_, 4);
  const uint8_t* vtable = table_ - soff;
  if (PERFETTO_UNLIKELY(vtable < buf_ || vtable + 4 > buf_ + buf_size_)) {
    return nullptr;
  }
  uint16_t vtable_size = ReadU16(vtable);
  uint32_t slot_pos = 4 + field_index * 2;
  if (PERFETTO_UNLIKELY(slot_pos + 2 > vtable_size)) {
    return nullptr;
  }
  uint16_t field_off = ReadU16(vtable + slot_pos);
  if (PERFETTO_UNLIKELY(field_off == 0)) {
    return nullptr;
  }
  const uint8_t* result = table_ + field_off;
  if (PERFETTO_UNLIKELY(result < buf_ || result >= buf_ + buf_size_)) {
    return nullptr;
  }
  return result;
}

std::string_view FlatBufferReader::String(uint32_t field_index) const {
  const uint8_t* p = FieldRaw(field_index);
  if (PERFETTO_UNLIKELY(!p)) {
    return {};
  }
  const uint8_t* s = FollowOffset(p);
  if (PERFETTO_UNLIKELY(!s || s + 4 > buf_ + buf_size_)) {
    return {};
  }
  uint32_t len = ReadU32(s);
  if (PERFETTO_UNLIKELY(s + 4 + len > buf_ + buf_size_)) {
    return {};
  }
  return {reinterpret_cast<const char*>(s + 4), len};
}

FlatBufferReader FlatBufferReader::Table(uint32_t field_index) const {
  const uint8_t* p = FieldRaw(field_index);
  if (PERFETTO_UNLIKELY(!p)) {
    return {};
  }
  const uint8_t* t = FollowOffset(p);
  if (PERFETTO_UNLIKELY(!t)) {
    return {};
  }
  return {buf_, buf_size_, t};
}

FlatBufferTableVec FlatBufferReader::VecTable(uint32_t field_index) const {
  const uint8_t* p = FieldRaw(field_index);
  if (PERFETTO_UNLIKELY(!p)) {
    return {};
  }
  const uint8_t* vec = FollowOffset(p);
  if (PERFETTO_UNLIKELY(!vec || vec + 4 > buf_ + buf_size_)) {
    return {};
  }
  uint32_t count = ReadU32(vec);
  return {buf_, buf_size_, vec + 4, count};
}

FlatBufferStringVec FlatBufferReader::VecString(uint32_t field_index) const {
  const uint8_t* p = FieldRaw(field_index);
  if (PERFETTO_UNLIKELY(!p)) {
    return {};
  }
  const uint8_t* vec = FollowOffset(p);
  if (PERFETTO_UNLIKELY(!vec || vec + 4 > buf_ + buf_size_)) {
    return {};
  }
  uint32_t count = ReadU32(vec);
  return {buf_, buf_size_, vec + 4, count};
}

// static
std::optional<FlatBufferReader> FlatBufferReader::GetRoot(const uint8_t* data,
                                                          uint32_t size) {
  if (PERFETTO_UNLIKELY(size < 4)) {
    return std::nullopt;
  }
  uint32_t root_off = ReadU32(data);
  if (PERFETTO_UNLIKELY(root_off >= size)) {
    return std::nullopt;
  }
  return FlatBufferReader(data, size, data + root_off);
}

FlatBufferReader FlatBufferTableVec::operator[](uint32_t i) const {
  const uint8_t* slot = base_ + i * 4;
  uint32_t rel = FlatBufferReader::ReadU32(slot);
  const uint8_t* target = slot + rel;
  if (PERFETTO_UNLIKELY(target < buf_ || target >= buf_ + buf_size_)) {
    return {};
  }
  return {buf_, buf_size_, target};
}

std::string_view FlatBufferStringVec::operator[](uint32_t i) const {
  const uint8_t* slot = base_ + i * 4;
  uint32_t rel = FlatBufferReader::ReadU32(slot);
  const uint8_t* s = slot + rel;
  if (PERFETTO_UNLIKELY(s < buf_ || s + 4 > buf_ + buf_size_)) {
    return {};
  }
  uint32_t len = FlatBufferReader::ReadU32(s);
  if (PERFETTO_UNLIKELY(s + 4 + len > buf_ + buf_size_)) {
    return {};
  }
  return {reinterpret_cast<const char*>(s + 4), len};
}

}  // namespace perfetto::trace_processor::util
