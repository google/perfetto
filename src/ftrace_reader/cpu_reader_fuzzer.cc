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

#include <stddef.h>
#include <stdint.h>

#include <algorithm>

#include "perfetto/base/utils.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "src/ftrace_reader/cpu_reader.h"
#include "test/cpu_reader_support.h"
#include "test/scattered_stream_null_delegate.h"

namespace perfetto {
namespace {

uint8_t g_page[base::kPageSize];

}  // namespace

using perfetto::protos::pbzero::FtraceEventBundle;

void FuzzCpuReaderParsePage(const uint8_t* data, size_t size);

void FuzzCpuReaderParsePage(const uint8_t* data, size_t size) {
  ScatteredStreamNullDelegate delegate(base::kPageSize);
  protozero::ScatteredStreamWriter stream(&delegate);
  FtraceEventBundle writer;

  ProtoTranslationTable* table = GetTable("synthetic");
  memset(g_page, 0, base::kPageSize);
  memcpy(g_page, data, std::min(base::kPageSize, size));

  EventFilter filter(*table, {"sched_switch", "print"});

  writer.Reset(&stream);
  FtraceMetadata metadata{};
  CpuReader::ParsePage(g_page, &filter, &writer, table, &metadata);
}

}  // namespace perfetto

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  perfetto::FuzzCpuReaderParsePage(data, size);
  return 0;
}
