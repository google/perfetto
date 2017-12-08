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

#include "src/tracing/core/chunked_protobuf_input_stream.h"

#include <algorithm>

#include "perfetto/base/logging.h"

namespace perfetto {

ChunkedProtobufInputStream::ChunkedProtobufInputStream(
    const ChunkSequence* chunks)
    : chunks_(chunks), cur_chunk_(chunks_->begin()) {}

ChunkedProtobufInputStream::~ChunkedProtobufInputStream() = default;

bool ChunkedProtobufInputStream::Next(const void** data, int* size) {
  if (cur_chunk_ == chunks_->end())
    return false;

  PERFETTO_DCHECK(Validate());
  *data = reinterpret_cast<const void*>(
      reinterpret_cast<uintptr_t>(cur_chunk_->start) + pos_in_cur_chunk_);
  *size = static_cast<int>(cur_chunk_->size - pos_in_cur_chunk_);
  cur_chunk_++;
  pos_in_cur_chunk_ = 0;
  PERFETTO_DCHECK(Validate());

  return true;
}

void ChunkedProtobufInputStream::BackUp(int count) {
  size_t n = static_cast<size_t>(count);
  PERFETTO_DCHECK(Validate());
  while (n) {
    if (cur_chunk_ == chunks_->end() || pos_in_cur_chunk_ == 0) {
      if (cur_chunk_ == chunks_->begin()) {
        // The protobuf library is violating its contract and backing up more
        // bytes than available.
        PERFETTO_DCHECK(false);
        return;
      }
      cur_chunk_--;
      pos_in_cur_chunk_ = cur_chunk_->size;
      continue;
    }

    const size_t decrement = std::min(n, pos_in_cur_chunk_);
    pos_in_cur_chunk_ -= decrement;
    n -= decrement;
  }
  PERFETTO_DCHECK(Validate());
}

bool ChunkedProtobufInputStream::Skip(int count) {
  PERFETTO_DCHECK(Validate());
  size_t n = static_cast<size_t>(count);
  while (n) {
    PERFETTO_DCHECK(Validate());
    if (cur_chunk_ == chunks_->end())
      return false;

    const size_t increment = std::min(n, cur_chunk_->size - pos_in_cur_chunk_);
    pos_in_cur_chunk_ += increment;
    n -= increment;

    if (pos_in_cur_chunk_ >= cur_chunk_->size) {
      cur_chunk_++;
      pos_in_cur_chunk_ = 0;
    }
  }
  PERFETTO_DCHECK(Validate());
  return true;
}

google::protobuf::int64 ChunkedProtobufInputStream::ByteCount() const {
  PERFETTO_DCHECK(Validate());
  google::protobuf::int64 count = 0;
  for (auto it = chunks_->begin(); it != chunks_->end(); it++) {
    if (it == cur_chunk_) {
      count += static_cast<google::protobuf::int64>(pos_in_cur_chunk_);
      break;
    }
    count += static_cast<google::protobuf::int64>(it->size);
  }
  return count;
}

bool ChunkedProtobufInputStream::Validate() const {
  return ((cur_chunk_ == chunks_->end() && pos_in_cur_chunk_ == 0) ||
          pos_in_cur_chunk_ < cur_chunk_->size ||
          (pos_in_cur_chunk_ == 0 && cur_chunk_->size == 0));
}

}  // namespace perfetto
