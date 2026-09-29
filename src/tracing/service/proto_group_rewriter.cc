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
#include <array>
#include <memory>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/public/pb_utils.h"

namespace perfetto::tracing_v2 {
namespace {

namespace pu = ::protozero::proto_utils;

// SliceCursor reads a packet from separate memory slices as one byte stream.
// The cursor skips empty slices.
//
// - read_ptr_ points to the next unread byte.
// - current_slice_end_ points immediately after the last byte in the slice.
// - offset() gives the number of bytes read or skipped in the packet.
//
// At the end of the packet, read_ptr_ equals current_slice_end_.
//
//   slices:   [a b c]  []  [d e]  [f g h i]
//   offsets:   0 1 2        3 4    5 6 7 8
//                             ^ read_ptr_: e, offset(): 4
//
// The diagram shows these cursor values:
// - current_slice_begin_ points to d.
// - current_slice_packet_offset_ is 3.
// - current_slice_end_ points one past e.
class SliceCursor {
 public:
  explicit SliceCursor(const Slices& slices)
      : next_slice_(slices.data()),
        slice_list_end_(slices.data() + slices.size()) {
    for (const Slice& slice : slices)
      packet_size_ += slice.size;
    LoadNextNonEmptySlice();
  }

  size_t packet_size() const { return packet_size_; }
  size_t offset() const {
    return current_slice_packet_offset_ +
           static_cast<size_t>(read_ptr_ - current_slice_begin_);
  }
  bool at_end() const { return read_ptr_ == current_slice_end_; }

  // Before this call, at_end() must be false.
  uint8_t PeekByte() const { return *read_ptr_; }

  // ReadVarInt() decodes one varint and moves the cursor past its bytes.
  // If the varint continues in another slice, this function reads that slice.
  //
  // Each varint byte carries 7 value bits and 1 continuation bit.
  // A 64-bit value needs at most 10 bytes. ParseVarIntFast() uses this limit.
  //
  // ReadVarInt() returns false if the varint is incomplete or exceeds 10 bytes.
  // The byte-by-byte loop advances the cursor after each byte.
  // It does not restore the cursor if decoding fails.
  bool ReadVarInt(uint64_t* value) {
    const uint8_t* next_ptr =
        pu::ParseVarIntFast(read_ptr_, current_slice_end_, value);
    if (PERFETTO_LIKELY(next_ptr)) {
      AdvanceTo(next_ptr);
      return true;
    }
    // With at least 10 unread bytes, ParseVarIntFast() had enough input.
    // Its failure means the first 10 bytes all have the continuation bit set.
    if (current_slice_end_ - read_ptr_ >= kMaxVarIntBytes)
      return false;

    // With fewer than 10 unread bytes, the varint can end in a later slice.
    // Read one byte at a time so the cursor can move between slices.
    //
    // For a 10-byte varint, the first 9 bytes supply 63 value bits (9 * 7).
    // In the tenth byte:
    // - The lowest bit supplies the remaining bit of the 64-bit value.
    // - The middle 6 bits are ignored, as in ParseVarInt().
    // - The highest bit must be 0. A value of 1 would require an eleventh byte.
    uint64_t decoded_value = 0;
    for (uint32_t shift = 0; shift < 64 && !at_end();
         shift += kVarIntValueBits) {
      const uint8_t byte = *read_ptr_;
      AdvanceTo(read_ptr_ + 1);
      const uint64_t value_bits = byte & kVarIntValueMask;
      decoded_value |= value_bits << shift;
      if ((byte & kVarIntContinuationBit) == 0) {
        *value = decoded_value;
        return true;
      }
    }
    return false;
  }

  // Skip() moves the cursor forward by |num_bytes| bytes without copying data.
  // The cursor can move through multiple slices.
  //
  // If fewer than |num_bytes| bytes remain, Skip() returns false.
  // In that case, the cursor does not move.
  bool Skip(uint64_t num_bytes) {
    if (PERFETTO_LIKELY(num_bytes <= static_cast<uint64_t>(current_slice_end_ -
                                                           read_ptr_))) {
      AdvanceTo(read_ptr_ + num_bytes);
      return true;
    }
    if (num_bytes > remaining_bytes())
      return false;
    CopyOrSkipAcrossSlices(/*dst=*/nullptr, static_cast<size_t>(num_bytes));
    return true;
  }

  // CopyTo() copies |num_bytes| bytes to |dst| and advances the cursor.
  // The copy can continue through multiple slices.
  //
  // The packet must have at least |num_bytes| unread bytes.
  // |dst| must have space for |num_bytes| bytes.
  void CopyTo(uint8_t* dst, size_t num_bytes) {
    PERFETTO_DCHECK(num_bytes <= remaining_bytes());
    if (PERFETTO_LIKELY(num_bytes <=
                        static_cast<size_t>(current_slice_end_ - read_ptr_))) {
      memcpy(dst, read_ptr_, num_bytes);
      AdvanceTo(read_ptr_ + num_bytes);
      return;
    }
    CopyOrSkipAcrossSlices(dst, num_bytes);
  }

 private:
  // The low 7 bits carry the value.
  // If the highest bit is set, another byte follows.
  static constexpr uint32_t kVarIntValueBits = 7;
  static constexpr uint8_t kVarIntValueMask = (1u << kVarIntValueBits) - 1;
  static constexpr uint8_t kVarIntContinuationBit = 1u << kVarIntValueBits;
  static constexpr ptrdiff_t kMaxVarIntBytes = PERFETTO_PB_VARINT_MAX_SIZE_64;

  size_t remaining_bytes() const { return packet_size_ - offset(); }

  // |next_ptr| must be in [read_ptr_, current_slice_end_].
  // At the slice end, select the next non-empty slice, if available.
  PERFETTO_ALWAYS_INLINE void AdvanceTo(const uint8_t* next_ptr) {
    PERFETTO_DCHECK(next_ptr >= read_ptr_ && next_ptr <= current_slice_end_);
    read_ptr_ = next_ptr;
    if (PERFETTO_UNLIKELY(read_ptr_ == current_slice_end_))
      LoadNextNonEmptySlice();
  }

  // Empty slices have equal start and end pointers, so the loop skips them.
  // If no slice remains, read_ptr_ equals current_slice_end_.
  void LoadNextNonEmptySlice() {
    while (read_ptr_ == current_slice_end_ && next_slice_ != slice_list_end_) {
      current_slice_packet_offset_ +=
          static_cast<size_t>(current_slice_end_ - current_slice_begin_);
      current_slice_begin_ = static_cast<const uint8_t*>(next_slice_->start);
      current_slice_end_ = current_slice_begin_ + next_slice_->size;
      read_ptr_ = current_slice_begin_;
      ++next_slice_;
    }
  }

  // Move the cursor forward by |num_bytes| bytes across slices.
  // If |dst| is not null, also copy these bytes to |dst|.
  //
  // Before this call, num_bytes <= remaining_bytes() must be true.
  void CopyOrSkipAcrossSlices(uint8_t* dst, size_t num_bytes) {
    while (num_bytes > 0) {
      const size_t chunk_size = std::min(
          num_bytes, static_cast<size_t>(current_slice_end_ - read_ptr_));
      if (dst) {
        memcpy(dst, read_ptr_, chunk_size);
        dst += chunk_size;
      }
      num_bytes -= chunk_size;
      AdvanceTo(read_ptr_ + chunk_size);
    }
  }

  // These pointers identify positions in the Slice list.
  const Slice* next_slice_;
  const Slice* const slice_list_end_;
  // These pointers identify bytes in the current slice.
  const uint8_t* current_slice_begin_ = nullptr;
  const uint8_t* current_slice_end_ = nullptr;
  const uint8_t* read_ptr_ = nullptr;
  size_t current_slice_packet_offset_ = 0;
  size_t packet_size_ = 0;
};

// OutputWriter converts groups to length-delimited messages.
// BeginGroup() reserves a 4-byte length slot.
// EndGroup() writes the final content size into this slot.
//
//   After the writer copies the last bytes in the group:
//                    ... | tag' | _ _ _ _ | content ... |
//                                 ^ saved length-slot offset
//   Content <= 127:  ... | tag' | L | content ... |
//   Content > 127:   ... | tag' | L L L L | content ... |
//
// tag' is the tag with wire type 2. L is one byte of the encoded length.
// - For content up to 127 bytes, the writer moves the content back 3 bytes.
//   The length field then uses one byte.
// - For larger content, the writer fills the slot with a 4-byte padded varint.
// - The writer does not copy the end byte from the input.
//
// OutputWriter allocates output for the first group.
// It copies each input range once, except for end bytes.
// Compaction and buffer growth can copy the output bytes again.
//
// The nesting stack stores output offsets.
// These offsets stay valid if the output buffer moves.
class OutputWriter {
 public:
  using OpenGroups = std::array<size_t, ProtoGroupRewriter::kMaxNestingDepth>;

  OutputWriter(const SliceCursor& input_cursor, OpenGroups* open_groups)
      : copy_cursor_(input_cursor), open_groups_(open_groups) {
    PERFETTO_DCHECK(open_groups_);
  }

  // BeginGroup() converts the start tag at input offset |tag_offset|.
  // |tag_size| is the tag length in bytes.
  // It returns false if kMaxNestingDepth groups are already open.
  bool BeginGroup(size_t tag_offset, size_t tag_size) {
    if (PERFETTO_UNLIKELY(num_open_groups_ == open_groups_->size()))
      return false;

    CopyInputUntil(tag_offset);

    EnsureAdditionalCapacity(tag_size + pu::kMessageLengthFieldSize);
    uint8_t* const tag_ptr = output_buffer_.get() + output_size_;
    copy_cursor_.CopyTo(tag_ptr, tag_size);

    // Change wire type 3 (binary 011) to wire type 2 (binary 010).
    // Clear the lowest bit. Keep the field ID and continuation bits.
    static_assert((pu::kWireTypeStartGroup ^ 1u) ==
                  static_cast<uint32_t>(pu::ProtoWireType::kLengthDelimited));
    *tag_ptr &= static_cast<uint8_t>(~1u);
    output_size_ += tag_size;

    (*open_groups_)[num_open_groups_++] = output_size_;
    output_size_ += pu::kMessageLengthFieldSize;
    return true;
  }

  // EndGroup() closes the innermost open group.
  // |end_byte_offset| is the input offset of the end byte.
  // The function checks the group state and content size before the final copy.
  // It returns kRewritten after it closes the group.
  RewriteResult EndGroup(size_t end_byte_offset) {
    if (PERFETTO_UNLIKELY(!HasOpenGroups()))
      return RewriteResult::kMalformedInput;

    const size_t length_slot_offset = (*open_groups_)[num_open_groups_ - 1];
    const size_t written_content_size =
        output_size_ - length_slot_offset - pu::kMessageLengthFieldSize;
    if (PERFETTO_UNLIKELY(written_content_size > pu::kMaxMessageLength))
      return RewriteResult::kGroupTooLarge;

    PERFETTO_DCHECK(end_byte_offset >= copy_cursor_.offset());
    const size_t pending_input_size = end_byte_offset - copy_cursor_.offset();
    if (PERFETTO_UNLIKELY(pending_input_size >
                          pu::kMaxMessageLength - written_content_size)) {
      return RewriteResult::kGroupTooLarge;
    }
    const size_t content_size = written_content_size + pending_input_size;

    CopyInputUntil(end_byte_offset);
    copy_cursor_.Skip(1);  // Skip the end byte.
    --num_open_groups_;

    // CopyInputUntil() can move the buffer. Get pointers after that call.
    uint8_t* const length_ptr = output_buffer_.get() + length_slot_offset;
    if (PERFETTO_UNLIKELY(content_size > pu::kMaxOneByteMessageLength)) {
      pu::WriteRedundantVarInt(static_cast<uint32_t>(content_size), length_ptr);
      return RewriteResult::kRewritten;
    }

    // A short length uses one byte. Remove the other 3 reserved bytes.
    uint8_t* const content_ptr = length_ptr + pu::kMessageLengthFieldSize;
    memmove(length_ptr + 1, content_ptr, content_size);
    *length_ptr = static_cast<uint8_t>(content_size);
    output_size_ -= pu::kMessageLengthFieldSize - 1;
    return RewriteResult::kRewritten;
  }

  // Before Finish(), the parser must validate all input and close all groups.
  // At least one group must exist in the packet.
  // Finish() transfers the output buffer to the returned Slice.
  Slice Finish() {
    PERFETTO_DCHECK(HasRewrittenGroups() && !HasOpenGroups());
    CopyInputUntil(copy_cursor_.packet_size());
    return Slice::TakeOwnership(std::move(output_buffer_), output_size_);
  }

  bool HasOpenGroups() const { return num_open_groups_ > 0; }

  // True after the first BeginGroup(). Only BeginGroup() allocates output.
  bool HasRewrittenGroups() const { return output_buffer_ != nullptr; }

 private:
  // CopyInputUntil() copies this input range to the output:
  //   [copy_cursor_.offset(), input_end_offset)
  // It then moves copy_cursor_ to |input_end_offset|.
  //
  // |input_end_offset| must not precede the cursor or exceed the packet size.
  void CopyInputUntil(size_t input_end_offset) {
    PERFETTO_DCHECK(input_end_offset >= copy_cursor_.offset());
    const size_t num_bytes = input_end_offset - copy_cursor_.offset();
    if (num_bytes == 0)
      return;
    EnsureAdditionalCapacity(num_bytes);
    copy_cursor_.CopyTo(output_buffer_.get() + output_size_, num_bytes);
    output_size_ += num_bytes;
  }

  // Reserve space for |num_bytes| additional output bytes.
  void EnsureAdditionalCapacity(size_t num_bytes) {
    const size_t required_capacity = output_size_ + num_bytes;
    if (PERFETTO_LIKELY(required_capacity <= output_capacity_))
      return;

    // Reserve space for the input and extra space for rewritten group lengths.
    // - input_size covers the original packet size.
    // - input_size / 16 adds about 6.25% spare space for larger group lengths.
    // - 64 adds spare bytes for temporary length slots in open groups.
    //
    // Always reserve at least |required_capacity| bytes.
    // Later allocations at least double the previous capacity.
    const size_t input_size = copy_cursor_.packet_size();
    const size_t initial_capacity = input_size + input_size / 16 + 64;
    const size_t new_capacity =
        std::max({required_capacity, initial_capacity, output_capacity_ * 2});
    std::unique_ptr<uint8_t[]> new_buffer(new uint8_t[new_capacity]);
    if (output_size_)
      memcpy(new_buffer.get(), output_buffer_.get(), output_size_);
    output_buffer_ = std::move(new_buffer);
    output_capacity_ = new_capacity;
  }

  SliceCursor copy_cursor_;
  // ProtoGroupRewriter owns the stack storage.
  // The first |num_open_groups_| entries belong to open groups.
  // The last of them belongs to the innermost open group.
  OpenGroups* const open_groups_;
  size_t num_open_groups_ = 0;
  std::unique_ptr<uint8_t[]> output_buffer_;
  size_t output_size_ = 0;
  size_t output_capacity_ = 0;
};

// RewriteFields() validates fields and converts group boundaries.
// SliceCursor handles reads and skips across slices.
// A 0x04 within a field value is ordinary data.
//
// TODO(sashwinbalaji): Measure the cost of cursor updates per field.
// Consider processing several fields per cursor update if this cost is high.
RewriteResult RewriteFields(SliceCursor input_cursor, OutputWriter* writer) {
  // A tag value has 32 bits: 29 for the field ID and 3 for the wire type.
  constexpr uint64_t kMaxFieldId = UINT32_MAX >> pu::kFieldTypeNumBits;

  while (!input_cursor.at_end()) {
    if (input_cursor.PeekByte() == pu::kProtoGroupEndByte) {
      const RewriteResult result = writer->EndGroup(input_cursor.offset());
      if (PERFETTO_UNLIKELY(result != RewriteResult::kRewritten))
        return result;
      input_cursor.Skip(1);
      continue;
    }

    const size_t tag_offset = input_cursor.offset();
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
        valid =
            writer->BeginGroup(tag_offset, input_cursor.offset() - tag_offset);
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
  // The constructor calculates the packet size once.
  // The parser and writer use separate copies of this cursor.
  const SliceCursor initial_cursor(input);
  OutputWriter writer(initial_cursor, &open_groups_);

  const RewriteResult result = RewriteFields(initial_cursor, &writer);
  if (result != RewriteResult::kRewritten)
    return result;

  output->emplace(writer.Finish());
  return RewriteResult::kRewritten;
}

}  // namespace perfetto::tracing_v2
