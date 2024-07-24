/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_BUGREPORT_ANDROID_BUGREPORT_READER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_BUGREPORT_ANDROID_BUGREPORT_READER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/importers/android_bugreport/android_log_reader.h"
#include "src/trace_processor/util/zip_reader.h"

namespace perfetto ::trace_processor {

namespace util {
class ZipReader;
}

class TraceProcessorContext;

// Trace importer for Android bugreport.zip archives.
class AndroidBugreportReader {
 public:
  static bool IsAndroidBugReport(
      const std::vector<util::ZipFile>& zip_file_entries);
  static util::Status Parse(TraceProcessorContext* context,
                            std::vector<util::ZipFile> zip_file_entries);

 private:
  AndroidBugreportReader(TraceProcessorContext* context,
                         std::vector<util::ZipFile> zip_file_entries);
  ~AndroidBugreportReader();
  util::Status ParseImpl();

  bool DetectYearAndBrFilename();
  base::StatusOr<std::vector<TimestampedAndroidLogEvent>>
  ParsePersistentLogcat();
  base::Status ParseDumpstateTxt(std::vector<TimestampedAndroidLogEvent>);

  TraceProcessorContext* const context_;
  std::vector<util::ZipFile> zip_file_entries_;
  int32_t br_year_ = 0;  // The year when the bugreport has been taken.
  const util::ZipFile* dumpstate_file_ =
      nullptr;  // The bugreport-xxx-2022-08-04....txt file
  std::string build_fpr_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ANDROID_BUGREPORT_ANDROID_BUGREPORT_READER_H_
