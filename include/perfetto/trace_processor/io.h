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

#ifndef INCLUDE_PERFETTO_TRACE_PROCESSOR_IO_H_
#define INCLUDE_PERFETTO_TRACE_PROCESSOR_IO_H_

#include <cstddef>
#include <memory>
#include <string>

#include "perfetto/base/export.h"
#include "perfetto/base/status.h"

namespace perfetto::trace_processor::io {

// A synchronous writable file. Each instance is used from a single thread.
class PERFETTO_EXPORT_COMPONENT File {
 public:
  File();
  virtual ~File();

  File(const File&) = delete;
  File& operator=(const File&) = delete;

  virtual base::Status Write(const void* data, size_t size) = 0;
};

// A synchronous filesystem used by Trace Processor features which write named
// files. Paths are opaque to Trace Processor and interpreted by the embedder.
// Implementations must be thread-safe, although each returned File is only
// used from one thread.
class PERFETTO_EXPORT_COMPONENT FileSystem {
 public:
  FileSystem();
  virtual ~FileSystem();

  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;

  // Opens |path| for writing, creating it if needed and replacing any existing
  // contents. On success, stores the opened file in |file|.
  virtual base::Status OpenFile(const std::string& path,
                                std::unique_ptr<File>* file) = 0;
};

}  // namespace perfetto::trace_processor::io

#endif  // INCLUDE_PERFETTO_TRACE_PROCESSOR_IO_H_
