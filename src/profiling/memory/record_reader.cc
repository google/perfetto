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
constexpr size_t kMaxReadSize = 16u * 4096u;
}

RecordReader::RecordReader(
    std::function<void(size_t, std::unique_ptr<uint8_t[]>)> callback_function)
    : callback_function_(std::move(callback_function)) {
  Reset();
}

ssize_t RecordReader::Read(int fd) {
  if (read_idx_ < sizeof(record_size_)) {
    ssize_t rd = ReadRecordSize(fd);
    if (rd != -1) {
      PERFETTO_DCHECK(rd >= 0);
      read_idx_ += static_cast<size_t>(rd);
    }
    if (read_idx_ == sizeof(record_size_)) {
      buf_.reset(new uint8_t[record_size_]);
      // Make sure zero sized records don't make us return zero from this
      // method which gets interpreted as fd closed.
      //
      // Without this check, the caller would re-enter in here, and ReadRecord
      // would be called with a record_size_ of zero, which will make read(2)
      // return 0.
      MaybeFinishAndReset();
    }
    return rd;
  }

  ssize_t rd = ReadRecord(fd);
  if (rd != -1) {
    PERFETTO_DCHECK(rd >= 0);
    read_idx_ += static_cast<size_t>(rd);
  }
  MaybeFinishAndReset();
  return rd;
}

void RecordReader::MaybeFinishAndReset() {
  if (done()) {
    PERFETTO_DCHECK(record_size_ < std::numeric_limits<size_t>::max());
    callback_function_(static_cast<size_t>(record_size_), std::move(buf_));
    Reset();
  }
}

void RecordReader::Reset() {
  read_idx_ = 0;
  record_size_ = 0;
}

bool RecordReader::done() {
  return read_idx_ >= sizeof(record_size_) &&
         read_idx_ - sizeof(record_size_) == record_size_;
}

ssize_t RecordReader::ReadRecordSize(int fd) {
  ssize_t rd = PERFETTO_EINTR(
      read(fd, reinterpret_cast<uint8_t*>(&record_size_) + read_idx_,
           sizeof(record_size_) - read_idx_));
  if (rd == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    PERFETTO_PLOG("read record size (fd: %d)", fd);
    PERFETTO_DCHECK(false);
  }
  return rd;
}

ssize_t RecordReader::ReadRecord(int fd) {
  PERFETTO_DCHECK(record_size_ <= std::numeric_limits<size_t>::max());
  size_t read_so_far = read_idx_ - sizeof(record_size_);
  size_t sz =
      std::min(kMaxReadSize, static_cast<size_t>(record_size_) - read_so_far);
  ssize_t rd = read(fd, buf_.get() + read_so_far, sz);
  if (rd == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
    PERFETTO_PLOG("read record (fd: %d)", fd);
    PERFETTO_DCHECK(false);
  }
  return rd;
}

}  // namespace perfetto
