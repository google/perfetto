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

namespace perfetto {

class RecordReader {
 public:
  RecordReader(std::function<void(size_t, std::unique_ptr<uint8_t[]>)>
                   callback_function);
  ssize_t Read(int fd);

 private:
  void MaybeFinishAndReset();
  void Reset();
  bool done();
  size_t read_idx();
  ssize_t ReadRecordSize(int fd);
  ssize_t ReadRecord(int fd);

  std::function<void(size_t, std::unique_ptr<uint8_t[]>)> callback_function_;
  size_t read_idx_;
  uint64_t record_size_;
  std::unique_ptr<uint8_t[]> buf_;
};

}  // namespace perfetto
#endif  // SRC_PROFILING_MEMORY_RECORD_READER_H_
