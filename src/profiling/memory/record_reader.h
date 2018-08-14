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

#ifndef SRC_PROFILING_MEMORY_RECORD_READER_H_
#define SRC_PROFILING_MEMORY_RECORD_READER_H_

#include <functional>
#include <memory>

#include <stdint.h>

#include "perfetto/base/utils.h"

namespace perfetto {

class RecordReader {
 public:
  struct ReceiveBuffer {
    uint8_t* data;
    size_t size;
  };

  enum class Result {
    Noop = 0,
    RecordReceived,
    KillConnection,
  };

  struct Record {
    std::unique_ptr<uint8_t[]> data;
    // This is not size_t so we can directly copy the received uint64_t
    // into it.
    uint64_t size = 0;
  };

  ReceiveBuffer BeginReceive();
  Result EndReceive(size_t recv_size,
                    Record* record) PERFETTO_WARN_UNUSED_RESULT;

 private:
  void Reset();

  // if < sizeof(uint64_t) we are still filling the record_size_buf_,
  // otherwise we are filling |record_.data|
  size_t read_idx_ = 0;
  alignas(uint64_t) uint8_t record_size_buf_[sizeof(uint64_t)];
  Record record_;

  static_assert(sizeof(record_size_buf_) == sizeof(record_.size),
                "sizes mismatch");
};

}  // namespace perfetto
#endif  // SRC_PROFILING_MEMORY_RECORD_READER_H_
