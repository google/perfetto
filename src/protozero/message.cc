/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "perfetto/protozero/message.h"

#include <atomic>
#include <type_traits>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/protozero/message_arena.h"
#include "perfetto/protozero/message_handle.h"

#if !PERFETTO_IS_LITTLE_ENDIAN()
// The memcpy() for float and double below needs to be adjusted if we want to
// support big endian CPUs. There doesn't seem to be a compelling need today.
#error Unimplemented for big endian archs.
#endif

namespace protozero {

namespace {

constexpr int kBytesToCompact = proto_utils::kMessageLengthFieldSize - 1u;

#if PERFETTO_DCHECK_IS_ON()
std::atomic<uint32_t> g_generation;
#endif

}  // namespace

// Do NOT put any code in the constructor or use default initialization.
// Use the Reset() method below instead.

// Keep the original exported overload for callers built before
// NestedMessageEncoding was added.
void Message::Reset(ScatteredStreamWriter* stream_writer, MessageArena* arena) {
  Reset(stream_writer, arena, NestedMessageEncoding::kLengthDelimited);
}

// This method is called to initialize both root and nested messages. Every
// member must be assigned before any generated setter can observe the object.
void Message::Reset(ScatteredStreamWriter* stream_writer,
                    MessageArena* arena,
                    NestedMessageEncoding nested_message_encoding) {
// Older versions of libstdcxx don't have is_trivially_constructible.
#if !defined(__GLIBCXX__) || __GLIBCXX__ >= 20170516
  static_assert(std::is_trivially_constructible<Message>::value,
                "Message must be trivially constructible");
#endif

  static_assert(std::is_trivially_destructible<Message>::value,
                "Message must be trivially destructible");

  // An unknown encoding must fail in release builds too, or it silently
  // becomes length-delimited framing and is misread. The C API aborts on the
  // same input.
  PERFETTO_CHECK(nested_message_encoding ==
                     NestedMessageEncoding::kLengthDelimited ||
                 nested_message_encoding == NestedMessageEncoding::kProtoGroup);

  stream_writer_ = stream_writer;
  arena_ = arena;
  size_ = 0;
  size_field_ = nullptr;
  nested_message_ = nullptr;
  message_state_ = MessageState::kNotFinalized;
  nested_message_encoding_ = nested_message_encoding;
  needs_proto_group_end_ = false;
#if PERFETTO_DCHECK_IS_ON()
  handle_ = nullptr;
  generation_ = g_generation.fetch_add(1, std::memory_order_relaxed);
#endif
}

void Message::AppendString(uint32_t field_id, const char* str) {
  AppendBytes(field_id, str, strlen(str));
}

void Message::AppendBytes(uint32_t field_id, const void* src, size_t size) {
  PERFETTO_DCHECK(field_id);
  if (nested_message_)
    EndNestedMessage();

  PERFETTO_DCHECK(size < proto_utils::kMaxMessageLength);
  // Write the proto preamble (field id, type and length of the field).
  uint8_t buffer[proto_utils::kMaxSimpleFieldEncodedSize];
  uint8_t* pos = buffer;
  pos = proto_utils::WriteVarInt(proto_utils::MakeTagLengthDelimited(field_id),
                                 pos);
  pos = proto_utils::WriteVarInt(static_cast<uint32_t>(size), pos);
  WriteToStream(buffer, pos);

  const uint8_t* src_u8 = reinterpret_cast<const uint8_t*>(src);
  WriteToStream(src_u8, src_u8 + size);
}

size_t Message::AppendScatteredBytes(uint32_t field_id,
                                     ContiguousMemoryRange* ranges,
                                     size_t num_ranges) {
  PERFETTO_DCHECK(field_id);
  if (nested_message_)
    EndNestedMessage();

  size_t size = 0;
  for (size_t i = 0; i < num_ranges; ++i) {
    size += ranges[i].size();
  }

  PERFETTO_DCHECK(size < proto_utils::kMaxMessageLength);

  uint8_t buffer[proto_utils::kMaxSimpleFieldEncodedSize];
  uint8_t* pos = buffer;
  pos = proto_utils::WriteVarInt(proto_utils::MakeTagLengthDelimited(field_id),
                                 pos);
  pos = proto_utils::WriteVarInt(static_cast<uint32_t>(size), pos);
  WriteToStream(buffer, pos);

  for (size_t i = 0; i < num_ranges; ++i) {
    auto& range = ranges[i];
    WriteToStream(range.begin, range.end);
  }

  return size;
}

uint32_t Message::Finalize() {
  if (is_finalized())
    return size_;

  if (nested_message_)
    EndNestedMessage();

  PERFETTO_DCHECK(!(needs_proto_group_end_ && size_field_));

  if (PERFETTO_UNLIKELY(needs_proto_group_end_)) {
    const uint8_t end = proto_utils::kProtoGroupEndByte;
    WriteToStream(&end, &end + 1);
    message_state_ = MessageState::kFinalized;
  } else if (size_field_) {
    // Write the length of the nested message a posteriori, using a leading-zero
    // redundant varint encoding. This can be nullptr for the root message,
    // among many reasons, because the TraceWriterImpl delegate is keeping track
    // of the root fragment size independently.
    PERFETTO_DCHECK(!is_finalized());
    PERFETTO_DCHECK(size_ < proto_utils::kMaxMessageLength);
    //
    // Normally the size of a protozero message is written with 4 bytes just
    // before the contents of the message itself:
    //
    //    size          message data
    //   [aa bb cc dd] [01 23 45 67 ...]
    //
    // We always reserve 4 bytes for the size, because the real size of the
    // message isn't known until the call to Finalize(). This is possible
    // because we can use leading zero redundant varint coding to expand any
    // size smaller than 256 MiB to 4 bytes.
    //
    // However this is wasteful for short, frequently written messages, so the
    // code below uses a 1 byte size field when possible. This is done by
    // shifting the already-written data (which should still be in the cache)
    // back by 3 bytes, resulting in this layout:
    //
    //   size  message data
    //   [aa] [01 23 45 67 ...]
    //
    // We can only do this optimization if the message is contained in a single
    // chunk (since we can't modify previously committed chunks). We can check
    // this by verifying that the size field is immediately before the message
    // in memory and is fully contained by the current chunk.
    //
    if (PERFETTO_LIKELY(size_ <= proto_utils::kMaxOneByteMessageLength &&
                        size_field_ ==
                            stream_writer_->write_ptr() - size_ -
                                proto_utils::kMessageLengthFieldSize &&
                        size_field_ >= stream_writer_->cur_range().begin)) {
      stream_writer_->Rewind(size_, kBytesToCompact);
      PERFETTO_DCHECK(size_field_ == stream_writer_->write_ptr() - size_ - 1u);
      *size_field_ = static_cast<uint8_t>(size_);
      message_state_ = MessageState::kFinalizedWithCompaction;
    } else {
      proto_utils::WriteRedundantVarInt(size_, size_field_);
      message_state_ = MessageState::kFinalized;
    }
    size_field_ = nullptr;
  } else {
    message_state_ = MessageState::kFinalized;
  }

#if PERFETTO_DCHECK_IS_ON()
  if (handle_)
    handle_->reset_message();
#endif

  return size_;
}

Message* Message::BeginNestedMessageInternal(uint32_t field_id) {
  PERFETTO_DCHECK(field_id);
  if (nested_message_)
    EndNestedMessage();

  const bool use_proto_group =
      nested_message_encoding_ == NestedMessageEncoding::kProtoGroup;
  uint8_t data[proto_utils::kMaxTagEncodedSize];
  const uint32_t tag = use_proto_group
                           ? proto_utils::MakeTagStartGroup(field_id)
                           : proto_utils::MakeTagLengthDelimited(field_id);
  uint8_t* data_end = proto_utils::WriteVarInt(tag, data);
  WriteToStream(data, data_end);

  Message* message = arena_->NewMessage();
  message->Reset(stream_writer_, arena_, nested_message_encoding_);

  if (PERFETTO_UNLIKELY(use_proto_group)) {
    message->needs_proto_group_end_ = true;
  } else {
    // The length of the nested message cannot be known upfront. So right now
    // just reserve the bytes to encode the size after the nested message is
    // done.
    message->set_size_field(
        stream_writer_->ReserveBytes(proto_utils::kMessageLengthFieldSize));
    size_ += proto_utils::kMessageLengthFieldSize;
  }

  nested_message_ = message;
  return message;
}

void Message::EndNestedMessage() {
  size_ += nested_message_->Finalize();
  if (nested_message_->message_state_ ==
      MessageState::kFinalizedWithCompaction) {
    size_ -= kBytesToCompact;
  }
  arena_->DeleteLastMessage(nested_message_);
  nested_message_ = nullptr;
}

}  // namespace protozero
