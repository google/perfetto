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

#ifndef SRC_TRACE_PROCESSOR_FILE_READER_H_
#define SRC_TRACE_PROCESSOR_FILE_READER_H_

#include <stdint.h>

#include "perfetto/base/scoped_file.h"
#include "src/trace_processor/blob_reader.h"

namespace perfetto {
namespace trace_processor {

class FileReader : public BlobReader {
 public:
  explicit FileReader(const char* path);
  ~FileReader() override;

  uint32_t Read(uint64_t offset, uint32_t len, uint8_t* dst) override;
  uint64_t file_size() const { return file_size_; }

 private:
  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  base::ScopedFile fd_;
  uint64_t file_size_ = 0;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_FILE_READER_H_
