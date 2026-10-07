/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/tracing/service/proto_group_rewriter.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/public/pb_utils.h"

namespace perfetto::tracing_v2 {
namespace {

namespace pu = ::protozero::proto_utils;

// Reads nonempty slices as one byte stream. At the packet end,
// read_ptr_ equals current_slice_end_.
class SliceCursor {
 public:
  explicit SliceCursor(const Slices& slices)
      : next_slice_(slices.data()),
        slice_list_end_(slices.data() + slices.size()) {
    for (const Slice& slice : slices)
      input_size_ += slice.size;

    LoadNextSlice();
  }

  // Total input bytes across all slices.
  size_t input_size() const { return input_size_; }
  size_t offset() const { return offset_; }
  bool at_end() const { return read_ptr_ == current_slice_end_; }

  // Before this call, at_end() must be false.
  uint8_t PeekByte() const { return *read_ptr_; }

  // Read one varint across slice boundaries. Failure can advance the cursor.
  bool ReadVarInt(uint64_t* value) {
    const uint8_t* next_ptr =
        pu::ParseVarIntFast(read_ptr_, current_slice_end_, value);
    if (PERFETTO_LIKELY(next_ptr)) {
      AdvanceTo(next_ptr);
      return true;
    }

    // No complete varint was found in this slice. If at least 10 bytes
    // remain, reject it: a 64-bit varint must end within 10 bytes.
    if (PERFETTO_UNLIKELY(slice_bytes_left() >=
                          PERFETTO_PB_VARINT_MAX_SIZE_64)) {
      return false;
    }

    // Read one byte at a time across slices.
    // As in ParseVarInt(), only the low bit of the tenth byte contributes.
    uint64_t decoded_value = 0;
    for (uint32_t shift = 0; shift < 64 && !at_end(); shift += 7) {
      const uint8_t byte = *read_ptr_;
      AdvanceTo(read_ptr_ + 1);

      // Place the seven data bits at this byte's bit offset in the value.
      decoded_value |= static_cast<uint64_t>(byte & 0b0111'1111) << shift;

      // A clear high bit marks the last byte of the varint.
      if ((byte & 0b1000'0000) == 0) {
        *value = decoded_value;
        return true;
      }
    }
    return false;
  }

  // Skip bytes across slices. Return false without advancing on truncated
  // input.
  bool Skip(uint64_t num_bytes) {
    if (PERFETTO_LIKELY(num_bytes <= slice_bytes_left())) {
      AdvanceTo(read_ptr_ + num_bytes);
      return true;
    }
    if (num_bytes > input_bytes_left())
      return false;

    CopyOrSkipAcrossSlices(/*dst=*/nullptr, static_cast<size_t>(num_bytes));
    return true;
  }

  // Copy bytes across slices. The input and |dst| must have enough space.
  void CopyTo(uint8_t* dst, size_t num_bytes) {
    PERFETTO_DCHECK(num_bytes <= input_bytes_left());
    if (PERFETTO_LIKELY(num_bytes <= slice_bytes_left())) {
      memcpy(dst, read_ptr_, num_bytes);
      AdvanceTo(read_ptr_ + num_bytes);
      return;
    }
    CopyOrSkipAcrossSlices(dst, num_bytes);
  }

 private:
  size_t input_bytes_left() const { return input_size_ - offset(); }

  size_t slice_bytes_left() const {
    return static_cast<size_t>(current_slice_end_ - read_ptr_);
  }

  // |next_ptr| must be in [read_ptr_, current_slice_end_].
  // At the slice end, select the next slice, if available.
  PERFETTO_ALWAYS_INLINE void AdvanceTo(const uint8_t* next_ptr) {
    PERFETTO_DCHECK(next_ptr >= read_ptr_ && next_ptr <= current_slice_end_);
    offset_ += static_cast<size_t>(next_ptr - read_ptr_);
    read_ptr_ = next_ptr;

    if (PERFETTO_UNLIKELY(read_ptr_ == current_slice_end_))
      LoadNextSlice();
  }

  void LoadNextSlice() {
    if (next_slice_ == slice_list_end_)
      return;

    PERFETTO_DCHECK(next_slice_->size > 0);
    read_ptr_ = static_cast<const uint8_t*>(next_slice_->start);
    current_slice_end_ = read_ptr_ + next_slice_->size;
    ++next_slice_;
  }

  // The caller checks the input size. A null |dst| skips bytes without a copy.
  void CopyOrSkipAcrossSlices(uint8_t* dst, size_t num_bytes) {
    while (num_bytes > 0) {
      const size_t chunk_size = std::min(num_bytes, slice_bytes_left());
      if (dst) {
        memcpy(dst, read_ptr_, chunk_size);
        dst += chunk_size;
      }
      num_bytes -= chunk_size;
      AdvanceTo(read_ptr_ + chunk_size);
    }
  }

  // The next slice to load. read_ptr_ already points into the current slice.
  const Slice* next_slice_;
  const Slice* const slice_list_end_;
  const uint8_t* current_slice_end_ = nullptr;
  const uint8_t* read_ptr_ = nullptr;
  size_t offset_ = 0;
  size_t input_size_ = 0;
};

// Converts group boundaries and copies the input between them.
// BeginGroup() allocates output on the first group and reserves a length slot.
// EndGroup() writes the length and removes 3 slot bytes for short messages.
//
//   Open group:      ... | tag | _ _ _ _ | contents ... |
//   Contents <= 127: ... | tag | length  | contents ... |
//   Contents > 127:  ... | tag | L L L L | contents ... |
//
// Group stack offsets stay valid if the output buffer moves.
class OutputWriter {
 public:
  using GroupLengthOffsets = base::SmallVector<size_t, 128>;

  OutputWriter(const SliceCursor& input_cursor,
               GroupLengthOffsets& group_length_offsets)
      : copy_cursor_(input_cursor),
        open_group_length_offsets_(group_length_offsets) {}

  // Convert the start tag. Return false if the group exceeds the nesting limit.
  bool BeginGroup(size_t tag_start_offset, size_t tag_size) {
    if (PERFETTO_UNLIKELY(open_group_length_offsets_.size() >=
                          ProtoGroupRewriter::kMaxNestingDepth)) {
      return false;
    }

    // Copy the input fields before this group's start tag.
    CopyInputUntil(tag_start_offset);

    // Reserve space for the tag and the group's length slot.
    EnsureAdditionalCapacity(tag_size + pu::kMessageLengthFieldSize);

    // Copy the start tag and change its wire type to length-delimited.
    uint8_t* const tag_ptr = output_buffer_.get() + output_size_;
    copy_cursor_.CopyTo(tag_ptr, tag_size);
    // Clear bit 0 to change wire type 3 to 2 while preserving the field ID
    // and continuation bit in the same byte.
    PERFETTO_DCHECK(pu::GetTagFieldType(*tag_ptr) == pu::kWireTypeStartGroup);
    *tag_ptr &= 0b1111'1110;
    output_size_ += tag_size;

    // Save the length slot offset for EndGroup() and leave space for its value.
    open_group_length_offsets_.emplace_back(output_size_);
    output_size_ += pu::kMessageLengthFieldSize;
    return true;
  }

  // Close the innermost group and fill its length slot.
  RewriteResult EndGroup(size_t end_byte_offset) {
    if (PERFETTO_UNLIKELY(!HasOpenGroups()))
      return RewriteResult::kMalformedInput;

    // The group contains output already written and input fields not yet
    // copied. Measure both parts to calculate the final group length.
    const size_t group_length_offset = open_group_length_offsets_.back();
    const size_t group_content_offset =
        group_length_offset + pu::kMessageLengthFieldSize;
    const size_t content_bytes_written = output_size_ - group_content_offset;
    PERFETTO_DCHECK(end_byte_offset >= copy_cursor_.offset());
    const size_t pending_input_size = end_byte_offset - copy_cursor_.offset();

    // Check both sizes before adding them to prevent overflow.
    if (PERFETTO_UNLIKELY(content_bytes_written > pu::kMaxMessageLength))
      return RewriteResult::kGroupTooLarge;
    const size_t max_pending_bytes =
        pu::kMaxMessageLength - content_bytes_written;
    if (PERFETTO_UNLIKELY(pending_input_size > max_pending_bytes)) {
      return RewriteResult::kGroupTooLarge;
    }
    const size_t content_size = content_bytes_written + pending_input_size;

    // Copy the remaining contents and skip the group's end byte.
    CopyInputUntil(end_byte_offset);
    copy_cursor_.Skip(1);

    // CopyInputUntil() can allocate a larger buffer and invalidate old
    // pointers. Use the saved offset to get a pointer into the current buffer.
    uint8_t* const length_ptr = output_buffer_.get() + group_length_offset;

    // Write the final length into the reserved slot.
    if (content_size > pu::kMaxOneByteMessageLength) {
      pu::WriteRedundantVarInt(static_cast<uint32_t>(content_size), length_ptr);
    } else {
      // The length needs only one of the four reserved bytes.
      // Move the contents 3 bytes earlier to close the gap after the length.
      uint8_t* const content_ptr = output_buffer_.get() + group_content_offset;
      memmove(length_ptr + 1, content_ptr, content_size);
      *length_ptr = static_cast<uint8_t>(content_size);
      output_size_ -= (pu::kMessageLengthFieldSize - 1);
    }

    // Remove the closed group from the stack.
    open_group_length_offsets_.pop_back();
    return RewriteResult::kRewritten;
  }

  // Transfer the output after the parser validates and closes all groups.
  Slice Finish() {
    PERFETTO_DCHECK(HasRewrittenGroups() && !HasOpenGroups());

    // Copy any fields after the last group. Input offsets start at zero.
    CopyInputUntil(copy_cursor_.input_size());
    return Slice::TakeOwnership(std::move(output_buffer_), output_size_);
  }

  bool HasOpenGroups() const { return !open_group_length_offsets_.empty(); }

  // True after the first BeginGroup(). Only BeginGroup() allocates output.
  bool HasRewrittenGroups() const { return output_buffer_ != nullptr; }

 private:
  // Append input bytes before |input_end_offset| to the output.
  // The parser has validated these bytes.
  void CopyInputUntil(size_t input_end_offset) {
    PERFETTO_DCHECK(input_end_offset >= copy_cursor_.offset());
    const size_t bytes_to_copy = input_end_offset - copy_cursor_.offset();
    if (bytes_to_copy == 0)
      return;

    // Reserve space before taking a pointer into the output buffer.
    EnsureAdditionalCapacity(bytes_to_copy);

    // Append the bytes. CopyTo() advances the input cursor.
    copy_cursor_.CopyTo(output_buffer_.get() + output_size_, bytes_to_copy);
    output_size_ += bytes_to_copy;
  }

  // Ensure room for |additional_bytes| after the current output.
  // Buffer growth invalidates pointers into the old buffer.
  void EnsureAdditionalCapacity(size_t additional_bytes) {
    const size_t required_capacity = output_size_ + additional_bytes;
    if (PERFETTO_LIKELY(required_capacity <= output_capacity_))
      return;

    size_t new_capacity;
    if (output_capacity_ == 0) {
      // Heuristic for the first allocation.
      // - input_size: space for the original packet bytes.
      // - input_size / 16: spare for groups that grow by 3 bytes.
      // - 64: spare for length slots reserved before their groups close.
      const size_t input_size = copy_cursor_.input_size();
      new_capacity = input_size + input_size / 16 + 64;
    } else {
      // Double the existing capacity to limit repeated allocations and copies.
      new_capacity = output_capacity_ * 2;
    }
    new_capacity = std::max(new_capacity, required_capacity);

    // Allocate a larger buffer and copy the output already written.
    std::unique_ptr<uint8_t[]> larger_buffer(new uint8_t[new_capacity]);
    if (output_size_)
      memcpy(larger_buffer.get(), output_buffer_.get(), output_size_);

    // Replace the old buffer after the copy completes.
    output_buffer_ = std::move(larger_buffer);
    output_capacity_ = new_capacity;
  }

  // Next input byte to copy. The parser can be ahead of this cursor.
  SliceCursor copy_cursor_;
  GroupLengthOffsets& open_group_length_offsets_;
  std::unique_ptr<uint8_t[]> output_buffer_;
  // Used output bytes, including reserved length slots.
  size_t output_size_ = 0;
  // Allocated bytes in output_buffer_.
  size_t output_capacity_ = 0;
};

// Parse field boundaries. A 0x04 inside a field value is ordinary data.
RewriteResult RewriteFields(SliceCursor input_cursor, OutputWriter* writer) {
  // A tag value has 32 bits: 29 for the field ID and 3 for the wire type.
  constexpr uint64_t kMaxFieldId = UINT32_MAX >> pu::kFieldTypeNumBits;

  while (!input_cursor.at_end()) {
    // The bare end byte has no field ID. Handle it before tag validation.
    if (input_cursor.PeekByte() == pu::kProtoGroupEndByte) {
      const RewriteResult result = writer->EndGroup(input_cursor.offset());
      PERFETTO_DCHECK(result != RewriteResult::kUnchanged);
      if (PERFETTO_UNLIKELY(result != RewriteResult::kRewritten))
        return result;
      input_cursor.Skip(1);
      continue;
    }

    // Save the start before ReadVarInt() advances past the tag.
    const size_t tag_start_offset = input_cursor.offset();
    uint64_t tag = 0;
    if (PERFETTO_UNLIKELY(!input_cursor.ReadVarInt(&tag)))
      return RewriteResult::kMalformedInput;

    const uint64_t field_id = pu::GetTagFieldId(tag);
    if (PERFETTO_UNLIKELY(field_id == 0 || field_id > kMaxFieldId))
      return RewriteResult::kMalformedInput;

    uint64_t value = 0;
    bool valid = false;
    switch (pu::GetTagFieldType(tag)) {
      case pu::kWireTypeStartGroup:
        valid = writer->BeginGroup(tag_start_offset,
                                   input_cursor.offset() - tag_start_offset);
        break;
      case static_cast<uint32_t>(pu::ProtoWireType::kVarInt):
        valid = input_cursor.ReadVarInt(&value);
        break;
      case static_cast<uint32_t>(pu::ProtoWireType::kFixed64):
        valid = input_cursor.Skip(sizeof(uint64_t));
        break;
      case static_cast<uint32_t>(pu::ProtoWireType::kFixed32):
        valid = input_cursor.Skip(sizeof(uint32_t));
        break;
      case static_cast<uint32_t>(pu::ProtoWireType::kLengthDelimited):
        valid = input_cursor.ReadVarInt(&value) && input_cursor.Skip(value);
        break;
      default:
        // Only the bare byte 0x04 closes a group.
        // Reject standard end-group tags. Wire types 6 and 7 are undefined.
        return RewriteResult::kMalformedInput;
    }

    if (PERFETTO_UNLIKELY(!valid))
      return RewriteResult::kMalformedInput;
  }

  if (writer->HasOpenGroups())
    return RewriteResult::kMalformedInput;

  return writer->HasRewrittenGroups() ? RewriteResult::kRewritten
                                      : RewriteResult::kUnchanged;
}

}  // namespace

RewriteResult ProtoGroupRewriter::Rewrite(const Slices& input,
                                          std::optional<Slice>* output) {
  open_group_length_offsets_.clear();

  // Cursor over the input for RewriteFields() to read and validate fields.
  const SliceCursor initial_cursor(input);
  // Writes validated fields to the output buffer and replaces group boundaries.
  OutputWriter writer(initial_cursor, open_group_length_offsets_);

  const RewriteResult result = RewriteFields(initial_cursor, &writer);
  if (result != RewriteResult::kRewritten)
    return result;

  output->emplace(writer.Finish());
  return RewriteResult::kRewritten;
}

}  // namespace perfetto::tracing_v2
