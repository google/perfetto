/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/profiling/memory/record_reader.h"

#include "perfetto/base/logging.h"

#include <unistd.h>
#include <algorithm>
#include <limits>

namespace perfetto {

namespace {
constexpr size_t kMaxRecordSize = 8 * 1024 * 1024;  // 8 MiB
static_assert(kMaxRecordSize <= std::numeric_limits<size_t>::max(),
              "kMaxRecordSize must fit into size_t");
}  // namespace

RecordReader::ReceiveBuffer RecordReader::BeginReceive() {
  if (read_idx_ < sizeof(record_size_buf_))
    return {&record_size_buf_[0] + read_idx_,
            sizeof(record_size_buf_) - read_idx_};
  PERFETTO_DCHECK(read_idx_ < record_.size + sizeof(record_size_buf_));
  const size_t buf_off = read_idx_ - sizeof(record_size_buf_);
  return {record_.data.get() + buf_off,
          static_cast<size_t>(record_.size) - buf_off};
}

RecordReader::Result RecordReader::EndReceive(size_t recv_size,
                                              Record* record) {
  if (record_.size == 0)
    // Still receiving header.
    PERFETTO_DCHECK(recv_size <= sizeof(uint64_t) - read_idx_);
  else
    // Receiving payload.
    PERFETTO_DCHECK(record_.data && recv_size <= record_.size);

  read_idx_ += recv_size;
  if (read_idx_ == sizeof(record_size_buf_)) {
    memcpy(&record_.size, record_size_buf_, sizeof(record_size_buf_));
    if (record_.size > kMaxRecordSize)
      return Result::KillConnection;
    record_.data.reset(new (std::nothrow) uint8_t[record_.size]);
    if (!record_.data)
      return Result::KillConnection;
  }

  if (read_idx_ == record_.size + sizeof(record_size_buf_)) {
    *record = std::move(record_);
    Reset();
    return Result::RecordReceived;
  }
  return Result::Noop;
}

void RecordReader::Reset() {
  read_idx_ = 0;
  record_.size = 0;
}

}  // namespace perfetto
