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

#ifndef FTRACE_READER_FTRACE_CPU_READER_H_
#define FTRACE_READER_FTRACE_CPU_READER_H_

#include <stdint.h>

#include "base/scoped_file.h"
#include "ftrace_event_bundle.pbzero.h"

namespace perfetto {

class FtraceToProtoTranslationTable;

class FtraceCpuReader {
 public:
  class Config {};

  FtraceCpuReader(const FtraceToProtoTranslationTable*,
                  size_t cpu,
                  base::ScopedFile fd);
  ~FtraceCpuReader();
  FtraceCpuReader(FtraceCpuReader&&);

  void Read(const Config&, pbzero::FtraceEventBundle*);

  int GetFileDescriptor();

 private:
  FtraceCpuReader(const FtraceCpuReader&) = delete;
  FtraceCpuReader& operator=(const FtraceCpuReader&) = delete;

  const FtraceToProtoTranslationTable* table_;
  const size_t cpu_;
  base::ScopedFile fd_;
};

} // namespace perfetto

#endif  // FTRACE_READER_FTRACE_CPU_READER_H_
