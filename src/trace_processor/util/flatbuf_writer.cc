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

#include "src/trace_processor/util/flatbuf_writer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "perfetto/base/logging.h"

namespace perfetto::trace_processor::util {

FlatBufferWriter::FlatBufferWriter(uint32_t initial_capacity)
    : buf_(initial_capacity, 0), head_(initial_capacity) {}

void FlatBufferWriter::GrowIfNeeded(uint32_t bytes) {
  if (head_ >= bytes) {
    return;
  }
  uint32_t old_size = static_cast<uint32_t>(buf_.size());
  uint32_t new_size = std::max(old_size * 2, old_size + bytes);
  buf_.resize(new_size, 0);
  // Shift existing data to the end of the new buffer.
  uint32_t data_len = old_size - head_;
  uint32_t new_head = new_size - data_len;
  memmove(&buf_[new_head], &buf_[head_], data_len);
  // Zero the gap.
  memset(&buf_[head_], 0, new_head - head_);
  head_ = new_head;
}

void FlatBufferWriter::Align(uint32_t alignment) {
  uint32_t pad = (alignment - (GetSize() % alignment)) % alignment;
  GrowIfNeeded(pad);
  for (uint32_t i = 0; i < pad; i++) {
    buf_[--head_] = 0;
  }
}

void FlatBufferWriter::PrependBytes(const void* src, uint32_t len) {
  GrowIfNeeded(len);
  head_ -= len;
  memcpy(&buf_[head_], src, len);
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

FlatBufferWriter::Offset FlatBufferWriter::WriteString(std::string_view s) {
  // Final buffer layout: [u32 len][chars...][null][pad to 4-byte boundary].
  // Back-to-front: pad, null, chars, len.
  Align(sizeof(uint32_t));
  Prepend<uint8_t>(0);
  PrependBytes(s.data(), static_cast<uint32_t>(s.size()));
  Prepend<uint32_t>(static_cast<uint32_t>(s.size()));
  return Offset{GetSize()};
}

// ---------------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------------

FlatBufferWriter::Offset FlatBufferWriter::WriteVecOffsets(
    const Offset* offsets,
    uint32_t count) {
  // Elements back-to-front (last element first).
  for (int32_t i = static_cast<int32_t>(count) - 1; i >= 0; i--) {
    Align(sizeof(uint32_t));
    // Relative offset: from the field position to the target.
    uint32_t stored = GetSize() + sizeof(uint32_t) - offsets[i].o;
    Prepend(stored);
  }
  Prepend<uint32_t>(count);
  return Offset{GetSize()};
}

FlatBufferWriter::Offset FlatBufferWriter::WriteVecStruct(
    const void* data,
    uint32_t elem_size,
    uint32_t count,
    uint32_t elem_align) {
  Align(elem_align);
  PrependBytes(data, elem_size * count);
  Prepend<uint32_t>(count);
  return Offset{GetSize()};
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

void FlatBufferWriter::StartTable() {
  table_body_start_ = GetSize();
  fields_.clear();
}

void FlatBufferWriter::FieldBool(uint32_t index, bool val) {
  Align(sizeof(uint8_t));
  Prepend<uint8_t>(val ? 1 : 0);
  fields_.push_back({index, GetSize()});
}

void FlatBufferWriter::FieldU8(uint32_t index, uint8_t val) {
  Align(sizeof(uint8_t));
  Prepend(val);
  fields_.push_back({index, GetSize()});
}

void FlatBufferWriter::FieldI16(uint32_t index, int16_t val) {
  Align(sizeof(int16_t));
  Prepend(val);
  fields_.push_back({index, GetSize()});
}

void FlatBufferWriter::FieldI32(uint32_t index, int32_t val) {
  Align(sizeof(int32_t));
  Prepend(val);
  fields_.push_back({index, GetSize()});
}

void FlatBufferWriter::FieldI64(uint32_t index, int64_t val) {
  Align(sizeof(int64_t));
  Prepend(val);
  fields_.push_back({index, GetSize()});
}

void FlatBufferWriter::FieldOffset(uint32_t index, Offset off) {
  Align(sizeof(uint32_t));
  // Relative offset from field position to target.
  uint32_t stored = GetSize() + sizeof(uint32_t) - off.o;
  Prepend(stored);
  fields_.push_back({index, GetSize()});
}

FlatBufferWriter::Offset FlatBufferWriter::EndTable() {
  PERFETTO_DCHECK(!fields_.empty() || table_body_start_ == GetSize());

  // Prepend the soffset placeholder (will be patched below).
  Align(sizeof(int32_t));
  Prepend<int32_t>(0);
  uint32_t soffset_buf_pos = head_;
  uint32_t table_off = GetSize();

  // Compute vtable dimensions.
  uint32_t max_index = 0;
  for (const auto& f : fields_) {
    max_index = std::max(max_index, f.index);
  }
  uint32_t num_slots = max_index + 1;
  uint32_t vtable_size = 4 + num_slots * 2;
  uint32_t table_size = table_off - table_body_start_;

  // Build vtable entries. Each entry is the byte offset from the table start
  // (soffset position) to the field data. 0 means "not present".
  std::vector<uint16_t> vt_entries(num_slots, 0);
  for (const auto& f : fields_) {
    // field.off_from_end > table_body_start_ (field was written between
    // StartTable and EndTable). table_off > field.off_from_end (soffset
    // was written after fields). vtable_entry = table_off - field.off_from_end.
    vt_entries[f.index] =
        static_cast<uint16_t>(table_off - f.off_from_end);
  }

  // Prepend vtable back-to-front: entries (last first), table_size, vtable_size.
  Align(sizeof(uint16_t));
  for (int32_t i = static_cast<int32_t>(num_slots) - 1; i >= 0; i--) {
    Prepend(vt_entries[static_cast<uint32_t>(i)]);
  }
  Prepend(static_cast<uint16_t>(table_size));
  Prepend(static_cast<uint16_t>(vtable_size));
  uint32_t vtable_off = GetSize();

  // Patch soffset: signed offset from table to vtable.
  // Reader does: vtable = table - soff, so soff = table_pos - vtable_pos.
  // In off-from-end terms: soff = vtable_off - table_off.
  auto soff = static_cast<int32_t>(vtable_off - table_off);
  memcpy(&buf_[soffset_buf_pos], &soff, sizeof(int32_t));

  return Offset{table_off};
}

// ---------------------------------------------------------------------------
// Finalize
// ---------------------------------------------------------------------------

void FlatBufferWriter::Finish(Offset root) {
  Align(sizeof(uint32_t));
  uint32_t stored = GetSize() + sizeof(uint32_t) - root.o;
  Prepend(stored);
}

std::vector<uint8_t> FlatBufferWriter::Release() {
  std::vector<uint8_t> result(buf_.begin() + head_, buf_.end());
  buf_.clear();
  head_ = 0;
  return result;
}

}  // namespace perfetto::trace_processor::util
